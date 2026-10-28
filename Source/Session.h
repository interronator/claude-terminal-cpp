#pragma once
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "Pty.h"
#include "Store.h"
#include "Terminal.h"

enum class Status { Starting, Busy, Waiting, Idle, Exited };

// A Claude CLI process + its terminal, plus the activity detection behind the status dots.
class Session
{
public:
    using StatusFn = std::function<void(Session&, Status OldStatus, Status NewStatus, double BusySeconds)>;
    static std::function<void()> Wake;  // wakes the UI loop from reader threads
    static const char* StatusName(Status CurrentStatus);

    explicit Session(SessionRec CurrentRec) : Rec(std::move(CurrentRec)) {}
    ~Session();

    SessionRec Rec;
    Status CurrentStatus = Status::Exited;
    bool Unseen = false;     // finished / needs input while you were looking elsewhere
    bool Attempted = false;  // auto-start already tried this run
    std::unique_ptr<Terminal> Term;
    std::string PendingPrompt;  // sent once Claude has finished starting up
    std::string ExtraGuidance;  // appended to the system prompt on launch
    std::time_t LaunchedAt = 0;  // wall-clock time the current Claude process started
    int BackgroundTasks = 0;     // background shells and monitors still running, from the transcript

    // per-view state owned by the UI
    int ScrollOffset = 0;
    long long LastPushed = 0;
    bool Selecting = false, HasSelection = false;
    long long SelectionStartRow = 0, SelectionEndRow = 0;
    int SelectionStartCol = 0, SelectionEndCol = 0;
    bool ReportingDrag = false;  // left button went down while the program had mouse reporting on
    int ReportedRow = -1, ReportedCol = -1;

    bool InputSelected = false;  // the current selection is the whole prompt text (Ctrl+A), so edits act on it
    bool FindOpen = false, FindFocus = false, FindHasMatch = false, FindNoMatch = false;
    long long FindStableRow = 0;
    int FindCol = 0;
    char FindBuffer[256] = "";

    bool IsRunning() const { return Pty_ != nullptr && Exited_ == false; }
    // Needs your attention: waiting on a prompt right now, or finished while you weren't looking.
    // Waiting is OR'd in explicitly because Unseen clears on focus even while still Waiting.
    bool RequiresAction() const { return CurrentStatus == Status::Waiting || (CurrentStatus == Status::Idle && Unseen && InBackground() == false); }
    // Claude's turn ended but background shells or monitors are still running, and it will pick up again when they report.
    bool InBackground() const { return CurrentStatus == Status::Idle && BackgroundTasks > 0; }
    bool Start(const std::string& Command, int Cols, int Rows, double Now, std::string& Err);
    void Kill(double Now, const StatusFn& Callback);
    void Tick(double Now, const StatusFn& Callback);                       // drain output, run timers
    void Input(const char* Data, size_t Length, double Now, const StatusFn& Callback);  // user keystrokes / paste
    void Resize(int Cols, int Rows);
    // Applies a new size only once it has stayed the same for RESIZE_SETTLE_SECONDS, so transient layout sizes
    // (window drags, mode switches, minimize) never reach the program and make it re-wrap its output narrow.
    void RequestResize(const int NewCols, const int NewRows, const double Now)
    {
        if (NewCols == Cols_ && NewRows == Rows_)
        {
            PendingCols_ = 0;
            return;
        }
        if (NewCols != PendingCols_ || NewRows != PendingRows_)
        {
            PendingCols_ = NewCols;
            PendingRows_ = NewRows;
            PendingSince_ = Now;
            return;
        }
        if (Now - PendingSince_ >= RESIZE_SETTLE_SECONDS)
        {
            PendingCols_ = 0;
            Resize(NewCols, NewRows);
        }
    }
    // Runs action with the terminal so bytes it emits (keys, mouse, paste markers) count as user input.
    void Typing(double Now, const StatusFn& Callback, const std::function<void(Terminal&)>& Action);
    void SendText(const std::string& Text, double Now, const StatusFn& Callback) { Input(Text.data(), Text.size(), Now, Callback); }

private:
    bool Spawn(const std::string& Command, int Cols, int Rows, bool Resume, double Now, std::string& Err);
    void SetStatus(Status CurrentStatus, double BusySeconds, const StatusFn& Callback);
    void Settle(double Now, const StatusFn& Callback);
    void WriteRaw(const char* Data, size_t Length);
    static bool LooksLikeWaiting(const std::string& Screen);

    std::unique_ptr<Pty> Pty_;
    std::mutex Mutex_;
    std::string Pending_;
    bool Exited_ = false;
    int ExitCode_ = 0;
    bool ResumeMode_ = false;
    double StartedAt_ = 0, FirstBurst_ = 0, LastInput_ = -10, IdleAt_ = 0;
    const StatusFn* TypingCallback_ = nullptr;
    double TypingNow_ = 0;
    std::string Command_;
    int Cols_ = 80, Rows_ = 24;
    int PendingCols_ = 0, PendingRows_ = 0;
    double PendingSince_ = 0;
    static constexpr double RESIZE_SETTLE_SECONDS = 0.3;
};
