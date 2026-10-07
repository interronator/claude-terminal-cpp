#pragma once
#include <string>
#include <vector>

struct ProcResult {
  int code = -1;
  std::string out;  // stdout + stderr
};

// Runs a program (looked up on PATH) without a console window and captures its output.
ProcResult runProcess(const std::vector<std::string>& argv, const std::string& cwd);

namespace git {
struct Info {
  bool isGit = false;
  std::string root, branch;
};
Info info(const std::string& dir);

struct Worktree {
  std::string repoRoot, branch, path, cwd;  // cwd = same subfolder as the project, inside the worktree
};
bool createWorktree(const std::string& projectPath, const std::string& name, Worktree& out, std::string& err);
bool isDirty(const std::string& path);
// Retries briefly: on Windows the folder can stay locked for a moment after the process exits.
bool removeWorktree(const std::string& repoRoot, const std::string& path, const std::string& branch, bool force,
                    bool& branchKept, std::string& err);
}  // namespace git
