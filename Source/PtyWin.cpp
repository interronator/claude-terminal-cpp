// Windows pseudo-console (ConPTY) backend.
#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "Pty.h"
#include "WinStrings.h"

// Resolve ConPTY at runtime: works with old MinGW headers and degrades
// gracefully on Windows older than 10 1809.
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

class ConPtyApi
{
public:
    typedef VOID* Handle;
    typedef HRESULT(WINAPI* CreateFn)(COORD, HANDLE, HANDLE, DWORD, Handle*);
    typedef HRESULT(WINAPI* ResizeFn)(Handle, COORD);
    typedef void(WINAPI* CloseFn)(Handle);

    static const ConPtyApi& Instance()
    {
        static ConPtyApi const API;
        return API;
    }

    CreateFn Create = nullptr;
    ResizeFn Resize = nullptr;
    CloseFn Close = nullptr;

    bool IsAvailable() const
    {
        return Create != nullptr && Resize != nullptr && Close != nullptr;
    }

private:
    ConPtyApi()
    {
        HMODULE const Kernel32 = GetModuleHandleW(L"kernel32.dll");
        Create = (CreateFn)(void*)GetProcAddress(Kernel32, "CreatePseudoConsole");
        Resize = (ResizeFn)(void*)GetProcAddress(Kernel32, "ResizePseudoConsole");
        Close = (CloseFn)(void*)GetProcAddress(Kernel32, "ClosePseudoConsole");
    }
};
#define HPCON ConPtyApi::Handle
#define CREATE_PSEUDO_CONSOLE(...) ConPtyApi::Instance().Create(__VA_ARGS__)
#define RESIZE_PSEUDO_CONSOLE(...) ConPtyApi::Instance().Resize(__VA_ARGS__)
#define CLOSE_PSEUDO_CONSOLE(...) ConPtyApi::Instance().Close(__VA_ARGS__)

namespace
{

class WinPty : public Pty
{
public:
    ~WinPty() override
    {
        Kill();
        if (Waiter_.joinable())
        {
            Waiter_.join();
        }
        else
        {  // Start() failed part-way
            if (Hpc_ != nullptr)
            {
                CLOSE_PSEUDO_CONSOLE(Hpc_);
            }
            if (OutRead_ != nullptr)
            {
                CloseHandle(OutRead_);
            }
        }
        if (InWrite_ != nullptr)
        {
            CloseHandle(InWrite_);
        }
        if (Proc_ != nullptr)
        {
            CloseHandle(Proc_);
        }
        if (Job_ != nullptr)
        {
            CloseHandle(Job_);
        }
    }

    bool Start(const std::vector<std::string>& Argv, const std::string& Cwd, int Cols, int Rows, PtyCallbacks Callbacks,
               std::string& Err) override
    {
        Callbacks_ = std::move(Callbacks);
        if (Argv.empty())
        {
            Err = "empty command";
            return false;
        }

        if (ConPtyApi::Instance().IsAvailable() == false)
        {
            Err = "CreatePseudoConsole failed (needs Windows 10 1809 or newer)";
            return false;
        }

        HANDLE InRead = nullptr, OutWrite = nullptr;
        if (CreatePipe(&InRead, &InWrite_, nullptr, 0) == FALSE || CreatePipe(&OutRead_, &OutWrite, nullptr, 0) == FALSE)
        {
            Err = "CreatePipe failed";
            return false;
        }
        COORD const Size{(SHORT)Cols, (SHORT)Rows};
        HRESULT const Hr = CREATE_PSEUDO_CONSOLE(Size, InRead, OutWrite, 0, &Hpc_);
        CloseHandle(InRead);
        CloseHandle(OutWrite);
        if (FAILED(Hr))
        {
            Err = "CreatePseudoConsole failed (needs Windows 10 1809 or newer)";
            return false;
        }

        // cmd.exe resolves both claude.exe and npm's claude.cmd; /s keeps our quoting intact.
        std::wstring Inner;
        for (size_t Index = 0; Index < Argv.size(); Index++)
        {
            if (Index != 0)
            {
                Inner += L' ';
            }
            Inner += WinStrings::QuoteArg(WinStrings::Widen(Argv[Index]));
        }
        std::wstring CommandLine = L"cmd.exe /d /s /c \"" + Inner + L"\"";

        STARTUPINFOEXW StartupInfo{};
        StartupInfo.StartupInfo.cb = sizeof(StartupInfo);
        SIZE_T AttributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &AttributeSize);
        std::vector<char> AttributeBuffer(AttributeSize);
        StartupInfo.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttributeBuffer.data());
        InitializeProcThreadAttributeList(StartupInfo.lpAttributeList, 1, 0, &AttributeSize);
        UpdateProcThreadAttribute(StartupInfo.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, Hpc_, sizeof(Hpc_),
                                  nullptr, nullptr);

        SetEnvironmentVariableW(L"COLORTERM", L"truecolor");
        std::wstring const WideCwd = WinStrings::Widen(Cwd);
        PROCESS_INFORMATION ProcessInfo{};
        BOOL const Ok = CreateProcessW(nullptr, CommandLine.data(), nullptr, nullptr, FALSE,
                                 EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, nullptr,
                                 WideCwd.empty() ? nullptr : WideCwd.c_str(), &StartupInfo.StartupInfo, &ProcessInfo);
        DeleteProcThreadAttributeList(StartupInfo.lpAttributeList);
        if (Ok == FALSE)
        {
            Err = "CreateProcess failed (error " + std::to_string(GetLastError()) + ")";
            return false;
        }

        // A kill-on-close job makes sure the whole process tree dies with the session.
        Job_ = CreateJobObjectW(nullptr, nullptr);
        if (Job_ != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION LimitInfo{};
            LimitInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(Job_, JobObjectExtendedLimitInformation, &LimitInfo, sizeof(LimitInfo));
            AssignProcessToJobObject(Job_, ProcessInfo.hProcess);
        }
        ResumeThread(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hThread);
        Proc_ = ProcessInfo.hProcess;

        Reader_ = std::thread([this] { ReadLoop(); });
        Waiter_ = std::thread([this] { WaitLoop(); });
        return true;
    }

    void Write(const char* Data, size_t Length) override
    {
        while (Length > 0 && InWrite_ != nullptr)
        {
            DWORD Written = 0;
            if (WriteFile(InWrite_, Data, (DWORD)Length, &Written, nullptr) == FALSE)
            {
                return;
            }
            Data += Written;
            Length -= Written;
        }
    }

    void Resize(int Cols, int Rows) override
    {
        std::lock_guard<std::mutex> const Guard(PseudoConsoleMutex_);
        if (Hpc_ != nullptr)
        {
            RESIZE_PSEUDO_CONSOLE(Hpc_, COORD{(SHORT)Cols, (SHORT)Rows});
        }
    }

    void Kill() override
    {
        Stop_ = true;
        if (Job_ != nullptr)
        {
            TerminateJobObject(Job_, 1);
        }
        else if (Proc_ != nullptr)
        {
            TerminateProcess(Proc_, 1);
        }
    }

private:
    void ReadLoop()
    {
        char Buffer[16384];
        DWORD BytesRead = 0;
        while (ReadFile(OutRead_, Buffer, sizeof Buffer, &BytesRead, nullptr) != FALSE && BytesRead > 0)
        {
            if (Stop_ == false && Callbacks_.OnData)
            {
                Callbacks_.OnData(Buffer, BytesRead);
            }
        }
    }

    void WaitLoop()
    {
        WaitForSingleObject(Proc_, INFINITE);
        DWORD ExitCode = 0;
        GetExitCodeProcess(Proc_, &ExitCode);
        {
            // Closing the pseudo-console flushes remaining output and ends the read pipe.
            std::lock_guard<std::mutex> const Guard(PseudoConsoleMutex_);
            if (Hpc_ != nullptr)
            {
                CLOSE_PSEUDO_CONSOLE(Hpc_);
            }
            Hpc_ = nullptr;
        }
        if (Reader_.joinable())
        {
            Reader_.join();
        }
        CloseHandle(OutRead_);
        OutRead_ = nullptr;
        if (Stop_ == false && Callbacks_.OnExit)
        {
            Callbacks_.OnExit((int)ExitCode);
        }
    }

    PtyCallbacks Callbacks_;
    HPCON Hpc_ = nullptr;
    HANDLE InWrite_ = nullptr, OutRead_ = nullptr, Proc_ = nullptr, Job_ = nullptr;
    std::thread Reader_, Waiter_;
    std::mutex PseudoConsoleMutex_;
    std::atomic<bool> Stop_{false};
};

}  // namespace

std::unique_ptr<Pty> Pty::Create()
{
    return std::make_unique<WinPty>();
}
