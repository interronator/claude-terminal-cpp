// Windows pseudo-console (ConPTY) backend.
#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "pty_host.h"

// Resolve ConPTY at runtime: works with old MinGW headers and degrades
// gracefully on Windows older than 10 1809.
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif
namespace conpty {
typedef VOID* HPC;
typedef HRESULT(WINAPI* CreateFn)(COORD, HANDLE, HANDLE, DWORD, HPC*);
typedef HRESULT(WINAPI* ResizeFn)(HPC, COORD);
typedef void(WINAPI* CloseFn)(HPC);
struct Api {
  CreateFn create = nullptr;
  ResizeFn resize = nullptr;
  CloseFn close = nullptr;
  Api() {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    create = (CreateFn)(void*)GetProcAddress(k, "CreatePseudoConsole");
    resize = (ResizeFn)(void*)GetProcAddress(k, "ResizePseudoConsole");
    close = (CloseFn)(void*)GetProcAddress(k, "ClosePseudoConsole");
  }
};
const Api& api() {
  static Api a;
  return a;
}
}  // namespace conpty
#define HPCON conpty::HPC
#define CreatePseudoConsole(...) conpty::api().create(__VA_ARGS__)
#define ResizePseudoConsole(...) conpty::api().resize(__VA_ARGS__)
#define ClosePseudoConsole(...) conpty::api().close(__VA_ARGS__)

namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::wstring quoteArg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') {
      backslashes++;
    } else if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out += L'"';
      backslashes = 0;
    } else {
      out.append(backslashes, L'\\');
      backslashes = 0;
      out += c;
    }
  }
  out.append(backslashes * 2, L'\\');
  return out + L"\"";
}

class WinPty : public Pty {
 public:
  ~WinPty() override {
    kill();
    if (waiter_.joinable()) waiter_.join();
    else {  // start() failed part-way
      if (hpc_) ClosePseudoConsole(hpc_);
      if (outRead_) CloseHandle(outRead_);
    }
    if (inWrite_) CloseHandle(inWrite_);
    if (proc_) CloseHandle(proc_);
    if (job_) CloseHandle(job_);
  }

  bool start(const std::vector<std::string>& argv, const std::string& cwd, int cols, int rows, PtyCallbacks cb,
             std::string& err) override {
    cb_ = std::move(cb);
    if (argv.empty()) {
      err = "empty command";
      return false;
    }

    HANDLE inRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite_, nullptr, 0) || !CreatePipe(&outRead_, &outWrite, nullptr, 0)) {
      err = "CreatePipe failed";
      return false;
    }
    COORD size{(SHORT)cols, (SHORT)rows};
    HRESULT hr = CreatePseudoConsole(size, inRead, outWrite, 0, &hpc_);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (FAILED(hr)) {
      err = "CreatePseudoConsole failed (needs Windows 10 1809 or newer)";
      return false;
    }

    // cmd.exe resolves both claude.exe and npm's claude.cmd; /s keeps our quoting intact.
    std::wstring inner;
    for (size_t i = 0; i < argv.size(); i++) {
      if (i) inner += L' ';
      inner += quoteArg(widen(argv[i]));
    }
    std::wstring cmdline = L"cmd.exe /d /s /c \"" + inner + L"\"";

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc_, sizeof(hpc_), nullptr,
                              nullptr);

    SetEnvironmentVariableW(L"COLORTERM", L"truecolor");
    std::wstring wcwd = widen(cwd);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                             EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, nullptr,
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok) {
      err = "CreateProcess failed (error " + std::to_string(GetLastError()) + ")";
      return false;
    }

    // A kill-on-close job makes sure the whole process tree dies with the session.
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
      li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof(li));
      AssignProcessToJobObject(job_, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;

    reader_ = std::thread([this] { readLoop(); });
    waiter_ = std::thread([this] { waitLoop(); });
    return true;
  }

  void write(const char* data, size_t n) override {
    while (n > 0 && inWrite_) {
      DWORD w = 0;
      if (!WriteFile(inWrite_, data, (DWORD)n, &w, nullptr)) return;
      data += w;
      n -= w;
    }
  }

  void resize(int cols, int rows) override {
    std::lock_guard<std::mutex> g(pcMutex_);
    if (hpc_) ResizePseudoConsole(hpc_, COORD{(SHORT)cols, (SHORT)rows});
  }

  void kill() override {
    stop_ = true;
    if (job_) TerminateJobObject(job_, 1);
    else if (proc_) TerminateProcess(proc_, 1);
  }

 private:
  void readLoop() {
    char buf[16384];
    DWORD n = 0;
    while (ReadFile(outRead_, buf, sizeof buf, &n, nullptr) && n > 0) {
      if (!stop_ && cb_.onData) cb_.onData(buf, n);
    }
  }

  void waitLoop() {
    WaitForSingleObject(proc_, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(proc_, &code);
    {
      // Closing the pseudo-console flushes remaining output and ends the read pipe.
      std::lock_guard<std::mutex> g(pcMutex_);
      if (hpc_) ClosePseudoConsole(hpc_);
      hpc_ = nullptr;
    }
    if (reader_.joinable()) reader_.join();
    CloseHandle(outRead_);
    outRead_ = nullptr;
    if (!stop_ && cb_.onExit) cb_.onExit((int)code);
  }

  PtyCallbacks cb_;
  HPCON hpc_ = nullptr;
  HANDLE inWrite_ = nullptr, outRead_ = nullptr, proc_ = nullptr, job_ = nullptr;
  std::thread reader_, waiter_;
  std::mutex pcMutex_;
  std::atomic<bool> stop_{false};
};

}  // namespace

std::unique_ptr<Pty> Pty::create() { return std::make_unique<WinPty>(); }
