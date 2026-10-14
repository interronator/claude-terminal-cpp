#include "session.h"

#include <algorithm>
#include <cctype>

#include "build_scripts.h"
#include "util.h"

namespace
{
constexpr double IDLE_SEC = 2.2;       // silence before a session counts as settled
constexpr double BUSY_AFTER_SEC = 0.8;  // continuous output before we call it "busy"
constexpr double ECHO_SEC = 0.25;       // output this soon after a keystroke is just echo

// Phrases that mean Claude is blocked on you. Checked only against the bottom of the screen.
const char* WaitPatterns[] = {"Do you want to", "Would you like to", "Esc to cancel", "(y/n)", "[y/N]", "\xE2\x9D\xAF 1. Yes",
                               "\xE2\x9D\xAF 1. Yes,"};
// Usage-limit notices; matched case-insensitively against the bottom of the screen.
const char* LimitPatterns[] = {"limit reached", "hit your limit", "limit will reset", "wait for limit to reset", "limit resets"};
}  // namespace

std::function<void()> Session::Wake;

const char* Session::StatusName(Status Status)
{
    switch (Status)
    {
        case Status::Starting: return "starting";
        case Status::Busy: return "busy";
        case Status::Waiting: return "waiting";
        case Status::Idle: return "idle";
        case Status::Exited: return "exited";
    }
    return "";
}

bool Session::LooksLikeWaiting(const std::string& Screen)
{
    // keep only the last ~14 non-empty lines
    std::vector<std::string> Lines;
    size_t Pos = 0;
    while (Pos <= Screen.size())
    {
        size_t const End = Screen.find('\n', Pos);
        std::string const Line = Screen.substr(Pos, End == std::string::npos ? std::string::npos : End - Pos);
        if (Util::Trim(Line).empty() == false)
        {
            Lines.push_back(Line);
        }
        if (End == std::string::npos)
        {
            break;
        }
        Pos = End + 1;
    }
    std::string Tail;
    for (size_t Index = Lines.size() > 14 ? Lines.size() - 14 : 0; Index < Lines.size(); Index++)
    {
        Tail += Lines[Index] + "\n";
    }
    std::string LowerTail = Tail;
    std::transform(LowerTail.begin(), LowerTail.end(), LowerTail.begin(), [](unsigned char Character) { return (char)std::tolower(Character); });
    for (const char* const Pattern : LimitPatterns)
    {
        if (LowerTail.find(Pattern) != std::string::npos)
        {
            return true;
        }
    }
    for (const char* const Pattern : WaitPatterns)
    {
        if (Tail.find(Pattern) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

Session::~Session()
{
    Pty_.reset();  // joins reader threads before the members they touch are destroyed
}

bool Session::Spawn(const std::string& Command, int Cols, int Rows, bool Resume, double Now, std::string& Err)
{
    Pty_.reset();
    {
        std::lock_guard<std::mutex> const Guard(Mutex_);
        Pending_.clear();
        Exited_ = false;
        ExitCode_ = 0;
    }
    Term = std::make_unique<Terminal>(Rows, Cols, [this](const char* Data, size_t Length) { WriteRaw(Data, Length); });
    ScrollOffset = 0;
    LastPushed = 0;
    HasSelection = Selecting = false;

    std::vector<std::string> Argv = Util::SplitCommand(Command.empty() ? "claude" : Command);
    if (Argv.empty())
    {
        Argv.push_back("claude");
    }
    Argv.push_back("--append-system-prompt");
    Argv.push_back(BuildScripts::Guidance());
    Argv.push_back(Resume ? "--resume" : "--session-id");
    Argv.push_back(Rec.ClaudeSessionId);

    std::unique_ptr<Pty> NewPty = Pty::Create();
    PtyCallbacks Callbacks;
    Callbacks.OnData = [this](const char* Data, size_t Length)
    {
        {
            std::lock_guard<std::mutex> const Guard(Mutex_);
            Pending_.append(Data, Length);
        }
        if (Wake)
        {
            Wake();
        }
    };
    Callbacks.OnExit = [this](int Code)
    {
        {
            std::lock_guard<std::mutex> const Guard(Mutex_);
            Exited_ = true;
            ExitCode_ = Code;
        }
        if (Wake)
        {
            Wake();
        }
    };
    if (NewPty->Start(Argv, Rec.Cwd, Cols, Rows, std::move(Callbacks), Err) == false)
    {
        return false;
    }
    Pty_ = std::move(NewPty);
    Cols_ = Cols;
    Rows_ = Rows;
    Command_ = Command;
    ResumeMode_ = Resume;
    StartedAt_ = Now;
    FirstBurst_ = 0;
    LastInput_ = -10;
    IdleAt_ = Now + IDLE_SEC;
    return true;
}

bool Session::Start(const std::string& Command, int Cols, int Rows, double Now, std::string& Err)
{
    if (IsRunning())
    {
        return true;
    }
    if (Spawn(Command, Cols, Rows, Rec.Started, Now, Err) == false)
    {
        CurrentStatus = Status::Exited;
        return false;
    }
    Rec.Started = true;
    CurrentStatus = Status::Starting;
    return true;
}

void Session::Kill(double, const StatusFn& Callback)
{
    Pty_.reset();
    {
        std::lock_guard<std::mutex> const Guard(Mutex_);
        Pending_.clear();
        Exited_ = false;
    }
    IdleAt_ = 0;
    SetStatus(Status::Exited, 0, Callback);
}

void Session::SetStatus(Status NewStatus, double BusySeconds, const StatusFn& Callback)
{
    if (NewStatus == CurrentStatus)
    {
        return;
    }
    Status const OldStatus = CurrentStatus;
    CurrentStatus = NewStatus;
    if (Callback)
    {
        Callback(*this, OldStatus, NewStatus, BusySeconds);
    }
}

void Session::Settle(double Now, const StatusFn& Callback)
{
    double const BusySeconds = FirstBurst_ > 0 ? std::max(0.0, Now - FirstBurst_ - IDLE_SEC) : 0.0;
    FirstBurst_ = 0;
    IdleAt_ = 0;
    bool const Waiting = Term && LooksLikeWaiting(Term->GetScreenText());
    SetStatus(Waiting ? Status::Waiting : Status::Idle, BusySeconds, Callback);
}

void Session::Tick(double Now, const StatusFn& Callback)
{
    if (Pty_ == nullptr)
    {
        return;
    }
    std::string Data;
    bool Exited;
    int Code;
    {
        std::lock_guard<std::mutex> const Guard(Mutex_);
        Data.swap(Pending_);
        Exited = Exited_;
        Code = ExitCode_;
    }

    if (Data.empty() == false && Term)
    {
        Term->Feed(Data.data(), Data.size());
        if (Now - LastInput_ >= ECHO_SEC)
        {  // ignore keystroke echo
            if (FirstBurst_ == 0)
            {
                FirstBurst_ = Now;
            }
            if (CurrentStatus != Status::Busy && Now - FirstBurst_ >= BUSY_AFTER_SEC)
            {
                SetStatus(Status::Busy, 0, Callback);
            }
            IdleAt_ = Now + IDLE_SEC;
        }
    }

    if (Exited)
    {
        if (ResumeMode_ && Code != 0 && Now - StartedAt_ < 6.0)
        {
            // Nothing to resume yet (the session never got a message): start it fresh instead.
            std::string Err;
            if (Spawn(Command_, Cols_, Rows_, false, Now, Err))
            {
                CurrentStatus = Status::Starting;
                return;
            }
        }
        Pty_.reset();
        IdleAt_ = 0;
        SetStatus(Status::Exited, 0, Callback);
        return;
    }

    if (IdleAt_ > 0 && Now >= IdleAt_)
    {
        Settle(Now, Callback);
    }
}

void Session::WriteRaw(const char* Data, size_t Length)
{
    if (TypingCallback_ != nullptr)
    {
        Input(Data, Length, TypingNow_, *TypingCallback_);
    }
    else if (Pty_)
    {
        Pty_->Write(Data, Length);
    }
}

void Session::Typing(double Now, const StatusFn& Callback, const std::function<void(Terminal&)>& Action)
{
    if (Term == nullptr || Pty_ == nullptr)
    {
        return;
    }
    TypingCallback_ = &Callback;
    TypingNow_ = Now;
    ScrollOffset = 0;
    Action(*Term);
    TypingCallback_ = nullptr;
}

void Session::Input(const char* Data, size_t Length, double Now, const StatusFn& Callback)
{
    if (Pty_ == nullptr || Exited_)
    {
        return;
    }
    if (CurrentStatus == Status::Starting)
    {
        return;
    }
    LastInput_ = Now;
    if (std::find(Data, Data + Length, '\r') != Data + Length && CurrentStatus != Status::Busy)
    {
        // Submitting a prompt or answering an approval: assume work starts right away.
        FirstBurst_ = Now;
        IdleAt_ = Now + IDLE_SEC;
        SetStatus(Status::Busy, 0, Callback);
    }
    Pty_->Write(Data, Length);
}

void Session::Resize(int Cols, int Rows)
{
    if (Cols < 2 || Rows < 1)
    {
        return;
    }
    if (Term && (Cols != Cols_ || Rows != Rows_))
    {
        Cols_ = Cols;
        Rows_ = Rows;
        Term->Resize(Rows, Cols);
        if (Pty_)
        {
            Pty_->Resize(Cols, Rows);
        }
    }
}
