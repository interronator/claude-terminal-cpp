#include "git.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "util.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

static std::wstring quote(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  size_t bs = 0;
  for (wchar_t c : a) {
    if (c == L'\\') bs++;
    else if (c == L'"') {
      out.append(bs * 2 + 1, L'\\');
      out += L'"';
      bs = 0;
    } else {
      out.append(bs, L'\\');
      bs = 0;
      out += c;
    }
  }
  out.append(bs * 2, L'\\');
  return out + L"\"";
}

ProcResult runProcess(const std::vector<std::string>& argv, const std::string& cwd) {
  ProcResult res;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return res;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

  std::wstring cmd;
  for (size_t i = 0; i < argv.size(); i++) {
    if (i) cmd += L' ';
    cmd += quote(widen(argv[i]));
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = si.hStdError = wr;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  std::wstring wcwd = widen(cwd);
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                           wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    res.out = "could not start " + (argv.empty() ? std::string("process") : argv[0]);
    return res;
  }
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0) res.out.append(buf, n);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  res.code = (int)code;
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(rd);
  return res;
}
#else
ProcResult runProcess(const std::vector<std::string>& argv, const std::string& cwd) {
  ProcResult res;
  int fds[2];
  if (pipe(fds) != 0) return res;
  pid_t pid = fork();
  if (pid < 0) return res;
  if (pid == 0) {
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[0]);
    close(fds[1]);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }
  close(fds[1]);
  char buf[4096];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof buf)) > 0) res.out.append(buf, (size_t)n);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  res.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return res;
}
#endif

namespace git {

static std::string firstLine(const std::string& s) {
  size_t e = s.find_first_of("\r\n");
  return util::trim(e == std::string::npos ? s : s.substr(0, e));
}

static std::string normalize(const std::string& p) { return fs::path(p).lexically_normal().make_preferred().string(); }

Info info(const std::string& dir) {
  Info i;
  auto r = runProcess({"git", "rev-parse", "--show-toplevel"}, dir);
  if (r.code != 0) return i;
  i.isGit = true;
  i.root = normalize(firstLine(r.out));
  auto b = runProcess({"git", "rev-parse", "--abbrev-ref", "HEAD"}, dir);
  if (b.code == 0) i.branch = firstLine(b.out);
  return i;
}

static void excludeWorktreeDir(const std::string& root) {
  auto r = runProcess({"git", "rev-parse", "--git-path", "info/exclude"}, root);
  if (r.code != 0) return;
  fs::path file(firstLine(r.out));
  if (file.is_relative()) file = fs::path(root) / file;
  const std::string line = ".claude/worktrees/";
  std::string existing;
  {
    std::ifstream in(file);
    if (in) existing.assign(std::istreambuf_iterator<char>(in), {});
  }
  if (existing.find(line) != std::string::npos) return;
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  std::ofstream out(file, std::ios::app);
  if (out) out << (existing.empty() || existing.back() == '\n' ? "" : "\n") << line << "\n";
}

bool createWorktree(const std::string& projectPath, const std::string& name, Worktree& out, std::string& err) {
  Info i = info(projectPath);
  if (!i.isGit) {
    err = "This project folder is not inside a git repository.";
    return false;
  }
  std::string s = util::slug(name);
  if (s.empty()) s = "session";
  std::string suffix = util::uuid4().substr(0, 4);
  std::string branch = "claude/" + s + "-" + suffix;
  fs::path wt = fs::path(i.root) / ".claude" / "worktrees" / (s + "-" + suffix);
  auto r = runProcess({"git", "worktree", "add", "-b", branch, wt.string()}, i.root);
  if (r.code != 0) {
    err = util::trim(r.out);
    return false;
  }
  excludeWorktreeDir(i.root);
  std::error_code ec;
  fs::path rel = fs::relative(fs::weakly_canonical(projectPath, ec), fs::weakly_canonical(i.root, ec), ec);
  out.repoRoot = i.root;
  out.branch = branch;
  out.path = wt.string();
  out.cwd = (ec || rel.empty() || rel == ".") ? wt.string() : (wt / rel).string();
  return true;
}

bool isDirty(const std::string& path) {
  auto r = runProcess({"git", "status", "--porcelain"}, path);
  return r.code == 0 && !util::trim(r.out).empty();
}

bool removeWorktree(const std::string& repoRoot, const std::string& path, const std::string& branch, bool force,
                    bool& branchKept, std::string& err) {
  std::vector<std::string> args = {"git", "worktree", "remove", path};
  if (force) args.push_back("--force");
  ProcResult r;
  for (int i = 0; i < 4; i++) {
    r = runProcess(args, repoRoot);
    if (r.code == 0 || r.out.find("not a working tree") != std::string::npos ||
        r.out.find("is not registered") != std::string::npos) {
      r.code = 0;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  if (r.code != 0) {
    err = util::trim(r.out);
    return false;
  }
  auto b = runProcess({"git", "branch", force ? "-D" : "-d", branch}, repoRoot);
  branchKept = b.code != 0;  // unmerged work stays reachable on its branch
  return true;
}

}  // namespace git
