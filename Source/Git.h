#pragma once
#include <string>
#include <vector>

struct ProcResult
{
    int Code = -1;
    std::string Output;  // stdout + stderr
};

class Git
{
public:
    struct Info
    {
        bool IsGitRepo = false;
        std::string Root, Branch;
    };

    struct Worktree
    {
        std::string RepoRoot, Branch, Path, Cwd;  // Cwd = same subfolder as the project, inside the worktree
    };

    struct GitHubStatus
    {
        enum class State { Unknown, NotInstalled, LoggedOut, LoggedIn };
        State Value = State::Unknown;
        std::string User;
    };

    // Asks the gh CLI whether github.com is logged in. Blocking: call from a background thread.
    static GitHubStatus QueryGitHub();
    // Logs the given account out of github.com (any account when User is empty). Blocking; on failure Error says why.
    static bool LogoutGitHub(const std::string& User, std::string& Error);

    // Runs a program (looked up on PATH) without a console window and captures its output.
    static ProcResult RunProcess(const std::vector<std::string>& Argv, const std::string& Cwd);

    static Info GetInfo(const std::string& Dir);
    // Adds Line to the repo's local ignore list (.git/info/exclude) unless it is already there.
    static void ExcludePath(const std::string& Root, const std::string& Line);
    static bool CreateWorktree(const std::string& ProjectPath, const std::string& Name, Worktree& Out, std::string& Err);
    static bool IsDirty(const std::string& Path);
    // Retries briefly: on Windows the folder can stay locked for a moment after the process exits.
    static bool RemoveWorktree(const std::string& RepoRoot, const std::string& Path, const std::string& Branch, bool Force,
                               bool& BranchKept, std::string& Err);

private:
    static std::string FirstLine(const std::string& Text);
    static std::string ExtractAccountName(const std::string& Block);
    static std::string Normalize(const std::string& Path);
    static void ExcludeWorktreeDir(const std::string& Root);
};
