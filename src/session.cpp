#include "session.h"

#include <algorithm>

#include "util.h"

namespace {
constexpr double kIdleSec = 2.2;       // silence before a session counts as settled
constexpr double kBusyAfterSec = 0.8;  // continuous output before we call it "busy"
constexpr double kEchoSec = 0.25;      // output this soon after a keystroke is just echo

// Phrases that mean Claude is blocked on you. Checked only against the bottom of the screen.
const char* kWaitPatterns[] = {"Do you want to", "Would you like to", "Esc to cancel", "(y/n)", "[y/N]", "\xE2\x9D\xAF 1. Yes",
                               "\xE2\x9D\xAF 1. Yes,"};

bool looksLikeWaiting(const std::string& screen) {
  // keep only the last ~14 non-empty lines
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos <= screen.size()) {
    size_t e = screen.find('\n', pos);
    std::string l = screen.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    if (!util::trim(l).empty()) lines.push_back(l);
    if (e == std::string::npos) break;
    pos = e + 1;
  }
  std::string tail;
  for (size_t i = lines.size() > 14 ? lines.size() - 14 : 0; i < lines.size(); i++) tail += lines[i] + "\n";
  for (const char* p : kWaitPatterns)
    if (tail.find(p) != std::string::npos) return true;
  return false;
}
}  // namespace

std::function<void()> Session::wake;

const char* statusName(Status s) {
  switch (s) {
    case Status::Starting: return "starting";
    case Status::Busy: return "busy";
    case Status::Waiting: return "waiting";
    case Status::Idle: return "idle";
    case Status::Exited: return "exited";
  }
  return "";
}

Session::~Session() {
  pty_.reset();  // joins reader threads before the members they touch are destroyed
}

bool Session::spawn(const std::string& command, int cols, int rows, bool resume, double now, std::string& err) {
  pty_.reset();
  {
    std::lock_guard<std::mutex> g(mu_);
    pending_.clear();
    exited_ = false;
    exitCode_ = 0;
  }
  term = std::make_unique<Terminal>(rows, cols, [this](const char* d, size_t n) { writeRaw(d, n); });
  scrollOffset = 0;
  lastPushed = 0;
  hasSel = selecting = false;

  auto argv = util::splitCommand(command.empty() ? "claude" : command);
  if (argv.empty()) argv.push_back("claude");
  argv.push_back(resume ? "--resume" : "--session-id");
  argv.push_back(rec.claudeSessionId);

  auto pty = Pty::create();
  PtyCallbacks cb;
  cb.onData = [this](const char* d, size_t n) {
    {
      std::lock_guard<std::mutex> g(mu_);
      pending_.append(d, n);
    }
    if (wake) wake();
  };
  cb.onExit = [this](int code) {
    {
      std::lock_guard<std::mutex> g(mu_);
      exited_ = true;
      exitCode_ = code;
    }
    if (wake) wake();
  };
  if (!pty->start(argv, rec.cwd, cols, rows, std::move(cb), err)) return false;
  pty_ = std::move(pty);
  cols_ = cols;
  rows_ = rows;
  command_ = command;
  resumeMode_ = resume;
  startedAt_ = now;
  firstBurst_ = 0;
  lastInput_ = -10;
  idleAt_ = now + kIdleSec;
  return true;
}

bool Session::start(const std::string& command, int cols, int rows, double now, std::string& err) {
  if (running()) return true;
  if (!spawn(command, cols, rows, rec.started, now, err)) {
    status = Status::Exited;
    return false;
  }
  rec.started = true;
  status = Status::Starting;
  return true;
}

void Session::kill(double, const StatusFn& cb) {
  pty_.reset();
  {
    std::lock_guard<std::mutex> g(mu_);
    pending_.clear();
    exited_ = false;
  }
  idleAt_ = 0;
  setStatus(Status::Exited, 0, cb);
}

void Session::setStatus(Status s, double busy, const StatusFn& cb) {
  if (s == status) return;
  Status old = status;
  status = s;
  if (cb) cb(*this, old, s, busy);
}

void Session::settle(double now, const StatusFn& cb) {
  double busy = firstBurst_ > 0 ? std::max(0.0, now - firstBurst_ - kIdleSec) : 0.0;
  firstBurst_ = 0;
  idleAt_ = 0;
  bool waiting = term && looksLikeWaiting(term->screenText());
  setStatus(waiting ? Status::Waiting : Status::Idle, busy, cb);
}

void Session::tick(double now, const StatusFn& cb) {
  if (!pty_) return;
  std::string data;
  bool exited;
  int code;
  {
    std::lock_guard<std::mutex> g(mu_);
    data.swap(pending_);
    exited = exited_;
    code = exitCode_;
  }

  if (!data.empty() && term) {
    term->feed(data.data(), data.size());
    if (now - lastInput_ >= kEchoSec) {  // ignore keystroke echo
      if (firstBurst_ == 0) firstBurst_ = now;
      if (status != Status::Busy && now - firstBurst_ >= kBusyAfterSec) setStatus(Status::Busy, 0, cb);
      idleAt_ = now + kIdleSec;
    }
  }

  if (exited) {
    if (resumeMode_ && code != 0 && now - startedAt_ < 6.0) {
      // Nothing to resume yet (the session never got a message): start it fresh instead.
      std::string err;
      if (spawn(command_, cols_, rows_, false, now, err)) {
        status = Status::Starting;
        return;
      }
    }
    pty_.reset();
    idleAt_ = 0;
    setStatus(Status::Exited, 0, cb);
    return;
  }

  if (idleAt_ > 0 && now >= idleAt_) settle(now, cb);
}

void Session::writeRaw(const char* d, size_t n) {
  if (typingCb_) input(d, n, typingNow_, *typingCb_);
  else if (pty_) pty_->write(d, n);
}

void Session::typing(double now, const StatusFn& cb, const std::function<void(Terminal&)>& f) {
  if (!term || !pty_) return;
  typingCb_ = &cb;
  typingNow_ = now;
  scrollOffset = 0;
  f(*term);
  typingCb_ = nullptr;
}

void Session::input(const char* data, size_t n, double now, const StatusFn& cb) {
  if (!pty_ || exited_) return;
  lastInput_ = now;
  if (std::find(data, data + n, '\r') != data + n && status != Status::Busy) {
    // Submitting a prompt or answering an approval: assume work starts right away.
    firstBurst_ = now;
    idleAt_ = now + kIdleSec;
    setStatus(Status::Busy, 0, cb);
  }
  pty_->write(data, n);
}

void Session::resize(int cols, int rows) {
  if (cols < 2 || rows < 1) return;
  if (term && (cols != cols_ || rows != rows_)) {
    cols_ = cols;
    rows_ = rows;
    term->resize(rows, cols);
    if (pty_) pty_->resize(cols, rows);
  }
}
