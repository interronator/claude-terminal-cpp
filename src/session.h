#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "pty_host.h"
#include "store.h"
#include "terminal.h"

enum class Status { Starting, Busy, Waiting, Idle, Exited };

// A Claude CLI process + its terminal, plus the activity detection behind the status dots.
class Session {
 public:
  using StatusFn = std::function<void(Session&, Status oldStatus, Status newStatus, double busySeconds)>;
  static std::function<void()> wake;  // wakes the UI loop from reader threads

  explicit Session(SessionRec r) : rec(std::move(r)) {}
  ~Session();

  SessionRec rec;
  Status status = Status::Exited;
  bool unseen = false;      // finished / needs input while you were looking elsewhere
  bool attempted = false;   // auto-start already tried this run
  std::unique_ptr<Terminal> term;

  // per-view state owned by the UI
  int scrollOffset = 0;
  long long lastPushed = 0;
  bool selecting = false, hasSel = false;
  long long selA_row = 0, selB_row = 0;
  int selA_col = 0, selB_col = 0;

  bool running() const { return pty_ != nullptr && !exited_; }
  bool start(const std::string& command, int cols, int rows, double now, std::string& err);
  void kill(double now, const StatusFn& cb);
  void tick(double now, const StatusFn& cb);       // drain output, run timers
  void input(const char* data, size_t n, double now, const StatusFn& cb);  // user keystrokes / paste
  void resize(int cols, int rows);
  // Runs f with the terminal so bytes it emits (keys, mouse, paste markers) count as user input.
  void typing(double now, const StatusFn& cb, const std::function<void(Terminal&)>& f);
  void sendText(const std::string& s, double now, const StatusFn& cb) { input(s.data(), s.size(), now, cb); }

 private:
  bool spawn(const std::string& command, int cols, int rows, bool resume, double now, std::string& err);
  void setStatus(Status s, double busy, const StatusFn& cb);
  void settle(double now, const StatusFn& cb);
  void writeRaw(const char* d, size_t n);

  std::unique_ptr<Pty> pty_;
  std::mutex mu_;
  std::string pending_;
  bool exited_ = false;
  int exitCode_ = 0;
  bool resumeMode_ = false;
  double startedAt_ = 0, firstBurst_ = 0, lastInput_ = -10, idleAt_ = 0;
  const StatusFn* typingCb_ = nullptr;
  double typingNow_ = 0;
  std::string command_;
  int cols_ = 80, rows_ = 24;
};

const char* statusName(Status s);
