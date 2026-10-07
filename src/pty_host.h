#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct PtyCallbacks {
  std::function<void(const char*, size_t)> onData;  // called from a reader thread
  std::function<void(int)> onExit;                  // called once, after the last onData
};

// A child process attached to a pseudo-terminal (ConPTY on Windows, forkpty elsewhere).
class Pty {
 public:
  virtual ~Pty() = default;
  static std::unique_ptr<Pty> create();

  // argv[0] is looked up on PATH. Windows goes through cmd.exe so claude.exe and claude.cmd both work.
  virtual bool start(const std::vector<std::string>& argv, const std::string& cwd, int cols, int rows,
                     PtyCallbacks cb, std::string& err) = 0;
  virtual void write(const char* data, size_t n) = 0;
  virtual void resize(int cols, int rows) = 0;
  virtual void kill() = 0;  // also kills child processes; the destructor joins all threads
};
