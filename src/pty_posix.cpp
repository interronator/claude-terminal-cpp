#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <thread>

#include "pty_host.h"

namespace {

std::string shQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  return out + "'";
}

class PosixPty : public Pty {
 public:
  ~PosixPty() override {
    kill();
    if (reader_.joinable()) reader_.join();
    if (fd_ >= 0) ::close(fd_);
  }

  bool start(const std::vector<std::string>& argv, const std::string& cwd, int cols, int rows, PtyCallbacks cb,
             std::string& err) override {
    if (argv.empty()) {
      err = "empty command";
      return false;
    }
    cb_ = std::move(cb);
    std::string cmd = "exec";
    for (auto& a : argv) cmd += " " + shQuote(a);
    const char* sh = std::getenv("SHELL");
    std::string shell = (sh && *sh) ? sh : "/bin/bash";

    struct winsize ws {};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    pid_ = forkpty(&fd_, nullptr, nullptr, &ws);
    if (pid_ < 0) {
      err = "forkpty failed";
      return false;
    }
    if (pid_ == 0) {
      // A parent that started us in the background may have left these ignored; children must not inherit that.
      for (int sig : {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE, SIGCHLD}) signal(sig, SIG_DFL);
      sigset_t none;
      sigemptyset(&none);
      sigprocmask(SIG_SETMASK, &none, nullptr);
      if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
      setenv("TERM", "xterm-256color", 1);
      setenv("COLORTERM", "truecolor", 1);
      execl(shell.c_str(), shell.c_str(), "-lc", cmd.c_str(), (char*)nullptr);
      _exit(127);
    }
    reader_ = std::thread([this] { readLoop(); });
    return true;
  }

  void write(const char* data, size_t n) override {
    while (n > 0 && fd_ >= 0) {
      ssize_t w = ::write(fd_, data, n);
      if (w < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        return;
      }
      data += w;
      n -= (size_t)w;
    }
  }

  void resize(int cols, int rows) override {
    struct winsize ws {};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    if (fd_ >= 0) ioctl(fd_, TIOCSWINSZ, &ws);
  }

  void kill() override {
    stop_ = true;
    if (pid_ > 0 && !reaped_) {
      ::kill(-pid_, SIGHUP);
      ::kill(-pid_, SIGTERM);
      ::kill(-pid_, SIGKILL);
    }
  }

 private:
  void readLoop() {
    char buf[16384];
    for (;;) {
      struct pollfd p {fd_, POLLIN, 0};
      int r = poll(&p, 1, 100);
      if (r < 0 && errno != EINTR) break;
      if (r > 0) {
        ssize_t n = ::read(fd_, buf, sizeof buf);
        if (n > 0) {
          if (!stop_ && cb_.onData) cb_.onData(buf, (size_t)n);
          continue;
        }
        if (n == 0 || (errno != EINTR && errno != EAGAIN)) break;
      }
      if (stop_) break;
    }
    int status = 0;
    int code = -1;
    if (waitpid(pid_, &status, stop_ ? WNOHANG : 0) > 0 || stop_) {
      if (WIFEXITED(status)) code = WEXITSTATUS(status);
      else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
    }
    reaped_ = true;
    if (!stop_ && cb_.onExit) cb_.onExit(code);
  }

  PtyCallbacks cb_;
  pid_t pid_ = -1;
  int fd_ = -1;
  std::thread reader_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> reaped_{false};
};

}  // namespace

std::unique_ptr<Pty> Pty::create() { return std::make_unique<PosixPty>(); }
