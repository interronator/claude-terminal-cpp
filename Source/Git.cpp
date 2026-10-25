#include "Git.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "Util.h"

#ifdef _WIN32
#include "WinStrings.h"
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
ProcResult Git::RunProcess(const std::vector<std::string>& Argv, const std::string& Cwd)
{
    ProcResult Result;
    SECURITY_ATTRIBUTES SecurityAttributes{sizeof(SecurityAttributes), nullptr, TRUE};
    HANDLE ReadHandle = nullptr, WriteHandle = nullptr;
    if (CreatePipe(&ReadHandle, &WriteHandle, &SecurityAttributes, 0) == FALSE)
    {
        return Result;
    }
    SetHandleInformation(ReadHandle, HANDLE_FLAG_INHERIT, 0);

    std::wstring CommandLine;
    for (size_t Index = 0; Index < Argv.size(); Index++)
    {
        if (Index != 0)
        {
            CommandLine += L' ';
        }
        CommandLine += WinStrings::QuoteArg(WinStrings::Widen(Argv[Index]));
    }
    STARTUPINFOW StartupInfo{};
    StartupInfo.cb = sizeof(StartupInfo);
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    StartupInfo.hStdOutput = StartupInfo.hStdError = WriteHandle;
    StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION ProcessInfo{};
    std::wstring const WideCwd = WinStrings::Widen(Cwd);
    BOOL const Ok = CreateProcessW(nullptr, CommandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             WideCwd.empty() ? nullptr : WideCwd.c_str(), &StartupInfo, &ProcessInfo);
    CloseHandle(WriteHandle);
    if (Ok == FALSE)
    {
        CloseHandle(ReadHandle);
        Result.Output = "could not start " + (Argv.empty() ? std::string("process") : Argv[0]);
        return Result;
    }
    char Buffer[4096];
    DWORD BytesRead = 0;
    while (ReadFile(ReadHandle, Buffer, sizeof Buffer, &BytesRead, nullptr) != FALSE && BytesRead > 0)
    {
        Result.Output.append(Buffer, BytesRead);
    }
    WaitForSingleObject(ProcessInfo.hProcess, INFINITE);
    DWORD ExitCode = 1;
    GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
    Result.Code = (int)ExitCode;
    CloseHandle(ProcessInfo.hProcess);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ReadHandle);
    return Result;
}
#else
ProcResult Git::RunProcess(const std::vector<std::string>& argv, const std::string& cwd)
{
    ProcResult result;
    int fds[2];
    if (pipe(fds) != 0)
    {
        return result;
    }
    std::vector<char*> args;
    for (const std::string& arg : argv)
    {
        args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    const char* const cwdPath = cwd.empty() == true ? nullptr : cwd.c_str();
    pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return result;
    }
    if (pid == 0)
    {
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        if (cwdPath != nullptr && chdir(cwdPath) != 0)
        {
            _exit(126);
        }
        execvp(args[0], args.data());
        _exit(127);
    }
    close(fds[1]);
    char buffer[4096];
    ssize_t bytesRead;
    while ((bytesRead = read(fds[0], buffer, sizeof buffer)) > 0)
    {
        result.Output.append(buffer, (size_t)bytesRead);
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    result.Code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}
#endif

Git::GitHubStatus Git::QueryGitHub()
{
    GitHubStatus Status;
    const ProcResult Result = RunProcess({"gh", "auth", "status", "--hostname", "github.com"}, "");
    if (Result.Code == -1 || Result.Code == 127)
    {
        Status.Value = GitHubStatus::State::NotInstalled;
        return Status;
    }
    if (Result.Code != 0)
    {
        Status.Value = GitHubStatus::State::LoggedOut;
        return Status;
    }
    Status.Value = GitHubStatus::State::LoggedIn;
    const std::string Header = "Logged in to";
    std::string FirstUser;
    size_t BlockAt = Result.Output.find(Header);
    while (BlockAt != std::string::npos)
    {
        const size_t NextAt = Result.Output.find(Header, BlockAt + Header.size());
        const std::string Block = Result.Output.substr(BlockAt, NextAt == std::string::npos ? std::string::npos : NextAt - BlockAt);
        const std::string BlockUser = ExtractAccountName(Block);
        if (FirstUser.empty() == true)
        {
            FirstUser = BlockUser;
        }
        if (Block.find("Active account: true") != std::string::npos)
        {
            Status.User = BlockUser;
            break;
        }
        BlockAt = NextAt;
    }
    if (Status.User.empty() == true)
    {
        Status.User = FirstUser;
    }
    return Status;
}

bool Git::LogoutGitHub(const std::string& User, std::string& Error)
{
    std::vector<std::string> Arguments = {"gh", "auth", "logout", "--hostname", "github.com"};
    if (User.empty() == false)
    {
        Arguments.push_back("--user");
        Arguments.push_back(User);
    }
    ProcResult Result = RunProcess(Arguments, "");
    if (Result.Code != 0 && User.empty() == false && Result.Output.find("unknown flag") != std::string::npos)
    {
        Result = RunProcess({"gh", "auth", "logout", "--hostname", "github.com"}, "");
    }
    if (Result.Code == 0)
    {
        return true;
    }
    Error = Util::Trim(Result.Output);
    if (Error.empty() == true)
    {
        Error = "gh auth logout failed.";
    }
    return false;
}

std::string Git::ExtractAccountName(const std::string& Block)
{
    for (const char* const Marker : {" account ", " as "})
    {
        const size_t MarkerAt = Block.find(Marker);
        if (MarkerAt == std::string::npos)
        {
            continue;
        }
        const size_t NameStart = MarkerAt + std::strlen(Marker);
        const size_t NameEnd = Block.find_first_of(" \r\n(", NameStart);
        return Block.substr(NameStart, NameEnd == std::string::npos ? std::string::npos : NameEnd - NameStart);
    }
    return std::string();
}

std::string Git::FirstLine(const std::string& Text)
{
    size_t const End = Text.find_first_of("\r\n");
    return Util::Trim(End == std::string::npos ? Text : Text.substr(0, End));
}

std::string Git::Normalize(const std::string& Path)
{
    return fs::path(Path).lexically_normal().make_preferred().string();
}

Git::Info Git::GetInfo(const std::string& Dir)
{
    Info Info;
    ProcResult const Toplevel = RunProcess({"git", "rev-parse", "--show-toplevel"}, Dir);
    if (Toplevel.Code != 0)
    {
        return Info;
    }
    Info.IsGitRepo = true;
    Info.Root = Normalize(FirstLine(Toplevel.Output));
    ProcResult const Branch = RunProcess({"git", "rev-parse", "--abbrev-ref", "HEAD"}, Dir);
    if (Branch.Code == 0)
    {
        Info.Branch = FirstLine(Branch.Output);
    }
    return Info;
}

void Git::ExcludeWorktreeDir(const std::string& Root)
{
    ExcludePath(Root, ".claude/worktrees/");
}

void Git::ExcludePath(const std::string& Root, const std::string& Line)
{
    ProcResult const ExcludeFile = RunProcess({"git", "rev-parse", "--git-path", "info/exclude"}, Root);
    if (ExcludeFile.Code != 0)
    {
        return;
    }
    fs::path File(FirstLine(ExcludeFile.Output));
    if (File.is_relative())
    {
        File = fs::path(Root) / File;
    }
    std::string Existing;
    {
        std::ifstream In(File);
        if (In.good())
        {
            Existing.assign(std::istreambuf_iterator<char>(In), {});
        }
    }
    if (Existing.find(Line) != std::string::npos)
    {
        return;
    }
    std::error_code ErrorCode;
    fs::create_directories(File.parent_path(), ErrorCode);
    std::ofstream Out(File, std::ios::app);
    if (Out.good())
    {
        Out << (Existing.empty() || Existing.back() == '\n' ? "" : "\n") << Line << "\n";
    }
}

bool Git::CreateWorktree(const std::string& ProjectPath, const std::string& Name, Worktree& Out, std::string& Err)
{
    Info const Info = GetInfo(ProjectPath);
    if (Info.IsGitRepo == false)
    {
        Err = "This project folder is not inside a git repository.";
        return false;
    }
    std::string Slug = Util::Slug(Name);
    if (Slug.empty())
    {
        Slug = "session";
    }
    std::string const Suffix = Util::Uuid4().substr(0, 4);
    std::string const Branch = "claude/" + Slug + "-" + Suffix;
    fs::path const WorktreePath = fs::path(Info.Root) / ".claude" / "worktrees" / (Slug + "-" + Suffix);
    ProcResult const AddResult = RunProcess({"git", "worktree", "add", "-b", Branch, WorktreePath.string()}, Info.Root);
    if (AddResult.Code != 0)
    {
        Err = Util::Trim(AddResult.Output);
        return false;
    }
    ExcludeWorktreeDir(Info.Root);
    std::error_code ErrorCode;
    fs::path const Relative = fs::relative(fs::weakly_canonical(ProjectPath, ErrorCode), fs::weakly_canonical(Info.Root, ErrorCode),
                                      ErrorCode);
    Out.RepoRoot = Info.Root;
    Out.Branch = Branch;
    Out.Path = WorktreePath.string();
    Out.Cwd = (ErrorCode || Relative.empty() || Relative == ".") ? WorktreePath.string() : (WorktreePath / Relative).string();
    return true;
}

bool Git::IsDirty(const std::string& Path)
{
    ProcResult const Status = RunProcess({"git", "status", "--porcelain"}, Path);
    return Status.Code == 0 && Util::Trim(Status.Output).empty() == false;
}

bool Git::RemoveWorktree(const std::string& RepoRoot, const std::string& Path, const std::string& Branch, bool Force,
                         bool& BranchKept, std::string& Err)
{
    std::vector<std::string> Args = {"git", "worktree", "remove", Path};
    if (Force)
    {
        Args.push_back("--force");
    }
    ProcResult Result;
    for (int Attempt = 0; Attempt < 4; Attempt++)
    {
        Result = RunProcess(Args, RepoRoot);
        if (Result.Code == 0 || Result.Output.find("not a working tree") != std::string::npos ||
            Result.Output.find("is not registered") != std::string::npos)
        {
            Result.Code = 0;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (Result.Code != 0)
    {
        Err = Util::Trim(Result.Output);
        return false;
    }
    ProcResult const BranchResult = RunProcess({"git", "branch", Force ? "-D" : "-d", Branch}, RepoRoot);
    BranchKept = BranchResult.Code != 0;  // unmerged work stays reachable on its branch
    return true;
}
