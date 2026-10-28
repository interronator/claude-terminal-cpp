#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "CommandTiming.h"
#include "FileIndex.h"
#include "Git.h"
#include "GitReview.h"
#include "GitHubLogin.h"
#include "imgui.h"
#include "LayoutDebug.h"
#include "LiveView.h"
#include "NightlyBuild.h"
#include "PeerLinks.h"
#include "PeerVeins.h"
#include "ProjectLog.h"
#include "ProjectLogWindow.h"
#include "ProjectSettingsWindow.h"
#include "ScreenshotFeed.h"
#include "Session.h"
#include "Store.h"
#include "TranscriptScanner.h"
#include "UsageClient.h"

struct GLFWwindow;
class ImageCache;

struct InputEvent
{
    enum class EventType { Char, Key };
    EventType Type = EventType::Char;
    uint32_t Cp = 0;
    int Key = 0, Mods = 0, Action = 0;
};

class App
{
public:
    App(GLFWwindow* CurrentWindow, Store& CurrentStore, float UiScale);
    ~App();

    // Raw input from GLFW callbacks (routed to the terminal during Frame()).
    void PushChar(uint32_t CodePoint);
    void PushKey(int CurrentKey, int Action, int Mods);
    void PushDrop(const std::vector<std::string>& Paths);

    // Call before ImGui::NewFrame(); returns true when the font atlas changed (re-upload the texture).
    bool RebuildFontsIfNeeded();
    void Frame(double Now);
    void TickSessions(double Now);
    double FrameWaitSeconds() const;
    void Shutdown();

private:
    enum class Modal { None, AddProject, NewSession, Rename, Confirm, CloseSession, Settings, Usage, GitHub, Search, Review };

    static constexpr size_t FEEDBACK_LINE_CAPACITY = 1024;
    static constexpr const char* PAUSE_LABEL = "Pause all";

    // ---- model ----
    Session* FindSession(const std::string& Id) const;
    Project* FindProject(const std::string& Id);
    Session* ActiveSession() const;
    std::vector<std::string> VisibleIds() const;
    std::vector<std::string> ActionFilteredIds() const;
    std::vector<std::string> FilteredOpenIds() const;
    void OpenSession(const std::string& Id);
    void HideSession(std::string Id);
    void RestoreArchivedSession(size_t ArchiveIndex);
    void SendPrompt(Session& CurrentSession, const std::string& Text);
    void DrawReview();
    void PollScreenshots(Session& CurrentSession);
    // ---- session-to-session messages, shown as veins along the pane borders ----
    void NotePeerMessages(const Session& CurrentSession, TranscriptScanner& Scanner);
    std::string PeerDisplayName(const std::string& Id) const;
    bool IsPeerBusy(const std::string& Id) const;
    void DrawPeerOverlay(const ImVec2 PanesMin, const ImVec2 PanesMax, const float Gap);
    void DrawWaitingPills(ImDrawList* const DrawList, const PeerVeins::Box& Pane, const std::vector<std::pair<std::string, ImU32>>& Labels);
    void RequestDeleteAllArchived();
    void BuildAndRun(Session& CurrentSession);
    // ---- auto mode: the user is away, so sessions keep going instead of stopping to ask ----
    static constexpr const char* AUTO_DONE_MARKER = "AUTO-DONE";
    static constexpr const char* AUTO_ON_PROMPT =
        "Auto mode is on: the user is away from the computer and can't answer questions for a while. Keep working on your current task "
        "without waiting for them. Whenever you would normally stop to ask the user something, make the most sensible choice yourself, say in "
        "one line what you chose and why, and carry on. Do not do anything destructive or hard to undo (deleting data, force-pushing, "
        "publishing, spending money); skip it and leave a note for the user instead. When the task is completely finished and there is "
        "nothing sensible left to do, reply with just: AUTO-DONE";
    static constexpr const char* AUTO_NUDGE_PROMPT =
        "(Auto mode: the user is still away.) Keep going without waiting for them. If you stopped to ask something, pick the most sensible "
        "option, note it in one line, and continue. Nothing destructive or hard to undo. If everything is finished, reply with just: AUTO-DONE";
    static constexpr const char* AUTO_OFF_PROMPT =
        "Auto mode is off: the user is back at the computer. Go back to normal and ask the user when you need a decision.";
    struct AutoState
    {
        double IdleSince = 0;   // when the session last settled after working; 0 while it works
        bool Nudged = false;    // already nudged for this idle spell
        int Nudges = 0;         // nudges in a row without real work in between
        bool Done = false;      // it replied AUTO-DONE
        bool Informed = false;  // it has been told auto mode is on
    };
    // Typing into an approval menu would pick one of its options, so a waiting session gets the message once it's idle.
    void SendWhenSafe(Session& Target, const std::string& Text)
    {
        if (Target.CurrentStatus == Status::Waiting)
        {
            Target.PendingPrompt = Text;
            return;
        }
        SendPrompt(Target, Text);
    }
    void SetAutoMode(const bool Enabled)
    {
        AutoMode_ = Enabled;
        int Told = 0;
        for (const std::unique_ptr<Session>& Candidate : Sessions_)
        {
            AutoState& State = AutoStates_[Candidate->Rec.Id];
            const bool WasInformed = State.Informed;
            State = AutoState{};
            if (Candidate->IsRunning() == false || Candidate->CurrentStatus == Status::Starting)
            {
                continue;
            }
            if (Enabled == true)
            {
                SendWhenSafe(*Candidate, AUTO_ON_PROMPT);
                State.Informed = true;
                Told++;
                continue;
            }
            if (WasInformed == true)
            {
                SendWhenSafe(*Candidate, AUTO_OFF_PROMPT);
                Told++;
            }
        }
        ShowToast(std::string(Enabled == true ? "Auto mode on: told " : "Auto mode off: told ") + std::to_string(Told) + " session" + (Told == 1 ? "" : "s"));
    }
    // Called every tick: a session that finished a turn gets a short "keep going" nudge after a few seconds, unless it said
    // it is done, is waiting on background tasks, or has already been nudged several times without doing real work.
    void AutoContinueTick(Session& Target)
    {
        constexpr double NUDGE_DELAY_SECONDS = 6.0;
        constexpr int MAX_NUDGES_IN_A_ROW = 4;
        if (AutoMode_ == false || Target.IsRunning() == false || Target.CurrentStatus != Status::Idle || Target.InBackground() == true ||
            Target.PendingPrompt.empty() == false)
        {
            return;
        }
        if (std::find(PausedIds_.begin(), PausedIds_.end(), Target.Rec.Id) != PausedIds_.end())
        {
            return;  // you paused it on purpose; Continue resumes it
        }
        AutoState& State = AutoStates_[Target.Rec.Id];
        if (State.IdleSince <= 0 || State.Nudged == true || State.Done == true || Now_ - State.IdleSince < NUDGE_DELAY_SECONDS)
        {
            return;
        }
        const std::map<std::string, TranscriptScanner>::const_iterator Scanner = TranscriptScanners_.find(Target.Rec.Id);
        const std::string LastReply = Scanner != TranscriptScanners_.end() ? Scanner->second.LastAssistantText() : std::string();
        if (LastReply.empty() == true)
        {
            return;  // nothing has happened in this session yet
        }
        State.Nudged = true;
        if (LastReply.find(AUTO_DONE_MARKER) != std::string::npos)
        {
            State.Done = true;
            return;
        }
        if (State.Nudges >= MAX_NUDGES_IN_A_ROW)
        {
            return;
        }
        State.Nudges++;
        SendPrompt(Target, State.Informed == true ? AUTO_NUDGE_PROMPT : AUTO_ON_PROMPT);
        State.Informed = true;
    }
    void AutoOnStatus(const Session& Target, const Status NewStatus, const double BusySeconds)
    {
        constexpr double REAL_WORK_SECONDS = 60.0;
        AutoState& State = AutoStates_[Target.Rec.Id];
        if (NewStatus == Status::Busy)
        {
            State.IdleSince = 0;
            State.Nudged = false;
            State.Done = false;
            return;
        }
        if (NewStatus != Status::Idle)
        {
            return;
        }
        State.IdleSince = Now_;
        if (BusySeconds >= REAL_WORK_SECONDS)
        {
            State.Nudges = 0;
        }
    }
    float AutoButtonWidth() const { return ImGui::CalcTextSize("Auto").x + ImGui::GetStyle().FramePadding.x * 2; }
    void DrawAutoButton();

    // ---- handoff: summarize a session to a file, then continue in a fresh session from it ----
    struct Handoff
    {
        std::string Path;
        std::time_t RequestedAt = 0;
        double CheckAt = 0;
        bool Ready = false;
    };
    static std::string HandoffPrompt(const std::string& Path)
    {
        return "Write a handoff summary so this work can be continued by a fresh Claude session that has none of this conversation's context. "
               "Save it as Markdown to " + Path + " (create the folder if needed). Include: the goal and what the user asked for; what has been "
               "done so far (files changed and why); the current state (what works, what is untested or broken, anything still running such as "
               "background tasks, editors or games); decisions made and the user's preferences and constraints from this conversation; open "
               "questions; and the exact next steps, in order. Be specific: name files, functions, commands and paths. Do not continue the work "
               "itself; only write the file, then reply with its path.";
    }
    // Feeds the summary itself to the new session, so it starts with the full context instead of having to go read it.
    static std::string ContinuePrompt(const std::string& Path)
    {
        std::ifstream Stream(Path, std::ios::binary);
        std::stringstream Buffer;
        Buffer << Stream.rdbuf();
        std::string Summary = Buffer.str();
        Summary.erase(std::remove(Summary.begin(), Summary.end(), '\r'), Summary.end());
        if (Summary.find_first_not_of(" \t\r\n") == std::string::npos)
        {
            return "You are continuing work another session handed off. Read the handoff summary at " + Path +
                   " first, check the current state of the files it names, then carry on with its next steps. Ask me if anything in it is unclear.";
        }
        return "You are continuing work another session handed off. Here is its handoff summary (also saved at " + Path + "):\n\n" + Summary +
               "\n\nCheck the current state of the files it names, then carry on with its next steps. Ask me if anything in it is unclear.";
    }
    void RequestHandoff(Session& CurrentSession)
    {
        const std::time_t Now = std::time(nullptr);
        std::tm Local{};
#ifdef _WIN32
        localtime_s(&Local, &Now);
#else
        localtime_r(&Now, &Local);
#endif
        char Stamp[32];
        std::strftime(Stamp, sizeof Stamp, "%Y-%m-%d_%H%M", &Local);
        std::string SafeName;
        for (const char Character : CurrentSession.Rec.Name)
        {
            SafeName += std::isalnum(static_cast<unsigned char>(Character)) != 0 ? Character : '-';
        }
        const std::filesystem::path Path = std::filesystem::path(SessionProjectDir(CurrentSession)) / "ClaudeHandoffs" / (std::string(Stamp) + "_" + SafeName + ".md");
        Handoffs_[CurrentSession.Rec.Id] = {Path.string(), Now, 0, false};
        SendPrompt(CurrentSession, HandoffPrompt(Path.string()));
        ShowToast("Asked " + CurrentSession.Rec.Name + " for a handoff summary. The button turns into Continue fresh when it's written.");
    }
    // Ready once the file exists, was written after the request, and the session has stopped working on it.
    const Handoff* PollHandoff(const Session& CurrentSession)
    {
        constexpr double CHECK_INTERVAL_SECONDS = 1.0;
        constexpr std::time_t CLOCK_SLACK_SECONDS = 2;
        std::map<std::string, Handoff>::iterator Found = Handoffs_.find(CurrentSession.Rec.Id);
        if (Found == Handoffs_.end())
        {
            return nullptr;
        }
        Handoff& Current = Found->second;
        if (Now_ >= Current.CheckAt)
        {
            Current.CheckAt = Now_ + CHECK_INTERVAL_SECONDS;
            std::error_code ErrorCode;
            const bool Exists = std::filesystem::exists(Current.Path, ErrorCode);
            const std::time_t Written = Exists == true ? FileTimeToTimeT(std::filesystem::last_write_time(Current.Path, ErrorCode)) : 0;
            Current.Ready = Exists == true && Written + CLOCK_SLACK_SECONDS >= Current.RequestedAt && CurrentSession.CurrentStatus != Status::Busy;
        }
        return &Current;
    }
    static std::time_t FileTimeToTimeT(const std::filesystem::file_time_type FileTime)
    {
        const std::chrono::system_clock::time_point System =
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(FileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        return std::chrono::system_clock::to_time_t(System);
    }
    void ContinueFromHandoff(const Session& Source)
    {
        const std::map<std::string, Handoff>::iterator Found = Handoffs_.find(Source.Rec.Id);
        if (Found == Handoffs_.end())
        {
            return;
        }
        const std::string Path = Found->second.Path;
        std::string Name = Source.Rec.Name;
        const std::string Suffix = " (cont.)";
        if (Name.size() < Suffix.size() || Name.compare(Name.size() - Suffix.size(), Suffix.size(), Suffix) != 0)
        {
            Name += Suffix;
        }
        std::string Err;
        Session* const Fresh = CreateSession(Source.Rec.ProjectId, Name, false, false, Err);
        if (Fresh == nullptr)
        {
            ShowToast("Could not start a fresh session: " + Err, true);
            return;
        }
        // Work in the same folder; a worktree stays owned by the original session, so closing this one never removes it.
        Fresh->Rec.Cwd = Source.Rec.Cwd;
        Fresh->PendingPrompt = ContinuePrompt(Path);
        Handoffs_.erase(Found);
        OpenSession(Fresh->Rec.Id);
        ShowToast("Started " + Name + " from the handoff. You can close " + Source.Rec.Name + " when you're happy.");
    }

    // Launches Claude with the project-log and screenshot-tagging instructions for the session's project.
    bool StartSession(Session& Target, const int Cols, const int Rows, std::string& Err)
    {
        const Project* const Owner = FindProject(Target.Rec.ProjectId);
        Target.ExtraGuidance = Owner != nullptr ? ProjectLog::Guidance(Owner->Path, Target.Rec.Name) : std::string();
        const std::string Tag = ScreenshotFinder::SessionTag(Target.Rec.Id);
        if (Tag.empty() == false)
        {
            const std::string ProjectDir = SessionProjectDir(Target);
            Target.ExtraGuidance += (Target.ExtraGuidance.empty() == true ? "" : " ") + ScreenshotFinder::Guidance(ProjectDir, Tag);
            if (IsUnrealSession(Target) == true)
            {
                LiveView::EnsureScript(ProjectDir);
                Target.ExtraGuidance += " " + LiveView::Guidance(ProjectDir, ScreenshotFinder::SessionShotDirectory(ProjectDir, Tag).string(), Tag);
            }
        }
        return Target.Start(State_.AppSettings.ClaudeCommand, Cols, Rows, Now_, Err);
    }
    bool IsUnrealSession(const Session& Target)
    {
        std::map<std::string, bool>::const_iterator Known = UnrealProjects_.find(Target.Rec.ProjectId);
        if (Known != UnrealProjects_.end())
        {
            return Known->second;
        }
        const bool IsUnreal = LiveView::IsUnrealProject(SessionProjectDir(Target));
        UnrealProjects_[Target.Rec.ProjectId] = IsUnreal;
        return IsUnreal;
    }
    void DrawLiveView(Session& CurrentSession, const ImVec2 PaneTopLeft, const ImVec2 PaneSize, const float HeaderHeight);
    // Shell commands, background shells and monitors the session is running, with elapsed time and a rough guess of
    // when each will finish, drawn over the top-left of its terminal.
    void DrawShellActivity(const Session& CurrentSession, const ImVec2 PaneTopLeft, const ImVec2 PaneSize, const float HeaderHeight);
    const std::string& OutputTail(const std::string& OutputFile)
    {
        constexpr double CHECK_INTERVAL_SECONDS = 1.0;
        OutputTailState& State = OutputTails_[OutputFile];
        if (Now_ >= State.CheckAt)
        {
            State.CheckAt = Now_ + CHECK_INTERVAL_SECONDS;
            State.Line = CommandTiming::LastOutputLine(OutputFile);
        }
        return State.Line;
    }
    static std::string FormatDuration(const double Seconds)
    {
        const long long Total = static_cast<long long>(Seconds + 0.5);
        char Buffer[32];
        if (Total < 60)
        {
            std::snprintf(Buffer, sizeof Buffer, "%llds", Total);
        }
        else if (Total < 3600)
        {
            std::snprintf(Buffer, sizeof Buffer, "%lldm %02llds", Total / 60, Total % 60);
        }
        else
        {
            std::snprintf(Buffer, sizeof Buffer, "%lldh %02lldm", Total / 3600, (Total % 3600) / 60);
        }
        return Buffer;
    }
    // Rough figure for "usually ~3m": whole minutes past a minute, whole seconds below.
    static std::string FormatRoughDuration(const double Seconds)
    {
        if (Seconds < 60)
        {
            return std::to_string(static_cast<long long>(Seconds + 0.5)) + "s";
        }
        if (Seconds < 3600)
        {
            return std::to_string(static_cast<long long>(Seconds / 60 + 0.5)) + "m";
        }
        char Buffer[32];
        std::snprintf(Buffer, sizeof Buffer, "%.1fh", Seconds / 3600);
        return Buffer;
    }
    static std::string TruncateToWidth(const std::string& Text, const float Width)
    {
        if (ImGui::CalcTextSize(Text.c_str()).x <= Width)
        {
            return Text;
        }
        std::string Cut = Text;
        while (Cut.empty() == false && ImGui::CalcTextSize((Cut + "...").c_str()).x > Width)
        {
            Cut.pop_back();
            while (Cut.empty() == false && (static_cast<unsigned char>(Cut.back()) & 0xC0) == 0x80)
            {
                Cut.pop_back();  // never split a UTF-8 character
            }
        }
        return Cut + "...";
    }
    static constexpr double LIVE_STALE_SECONDS = 5.0;
    static constexpr const char* LIVE_BUTTON_LABEL = "   Live";  // leading spaces leave room for the status dot
    // The session's newest live frame, re-checked on disk a few times a second.
    const LiveView::Frame& PollLiveFrame(const Session& CurrentSession)
    {
        constexpr double CHECK_INTERVAL_SECONDS = 0.25;
        const std::string& Id = CurrentSession.Rec.Id;
        if (Now_ >= LiveFrameCheckAt_[Id])
        {
            LiveFrameCheckAt_[Id] = Now_ + CHECK_INTERVAL_SECONDS;
            const std::string Tag = ScreenshotFinder::SessionTag(Id);
            LiveFrames_[Id] = LiveView::Latest(ScreenshotFinder::SessionShotDirectory(SessionProjectDir(CurrentSession), Tag).string(), Tag);
        }
        return LiveFrames_[Id];
    }
    static bool HasFreshLiveFrame(const LiveView::Frame& Frame)
    {
        return Frame.Path.empty() == false && Frame.AgeSeconds <= LIVE_STALE_SECONDS;
    }
    // Screenshots go under the main project folder even for worktree sessions, so they all land in one place.
    std::string SessionProjectDir(const Session& Target)
    {
        const Project* const Owner = FindProject(Target.Rec.ProjectId);
        return Owner != nullptr ? Owner->Path : Target.Rec.Cwd;
    }
    Session* FindSessionByTag(const std::string& Tag) const
    {
        if (Tag.empty() == true)
        {
            return nullptr;
        }
        for (const std::unique_ptr<Session>& Candidate : Sessions_)
        {
            if (ScreenshotFinder::SessionTag(Candidate->Rec.Id) == Tag)
            {
                return Candidate.get();
            }
        }
        return nullptr;
    }
    void DrawProjectLog()
    {
        const ProjectLogWindow::Palette Colors{AccentColorV4_, TextColorV4_, PaneBgColorU32_, TextColorU32_, TextDisabledColorU32_};
        std::string Error;
        ProjectLogWindow_.Draw(State_.Projects, Now_, UiScale_, Colors, Error);
        if (Error.empty() == false)
        {
            ShowToast(Error, true);
        }
    }
    void DrawProjectSettings()
    {
        std::string Message;
        bool IsError = false;
        ProjectSettingsWindow_.Draw(State_.Projects, UiScale_, AccentColorV4_, Message, IsError);
        if (Message.empty() == false)
        {
            ShowToast(Message, IsError);
        }
    }
    void OpenNightlyBuild(const std::string& ProjectId)
    {
        const Project* const Owner = FindProject(ProjectId);
        if (Owner == nullptr)
        {
            return;
        }
        NightlyBuildId_ = ProjectId;
        NightlyBuildCheckAt_ = 0;
        NightlyBuildFocus_ = true;
        NightlyBuildError_.clear();
        NightlyDraft_ = NightlyBuild::Job{};
        NightlyIsUnreal_ = NightlyBuild::IsUnreal(Owner->Path);
        NightlyHasScript_ = NightlyBuild::HasBuildScript(Owner->Path);
        NightlyDraft_.What = NightlyIsUnreal_ == true ? NightlyBuild::Kind::PackageGame : NightlyBuild::Kind::BuildScript;
        std::snprintf(NightlyNameBuffer_, sizeof NightlyNameBuffer_, "%s", "Nightly");
        NightlyOutputBuffer_[0] = 0;
        NightlyEngineBuffer_[0] = 0;
    }
    void DrawNightlyBuild();
    void DrawNightlyBuildJobs(const Project& Owner);
    void DrawNightlyBuildSetup(const Project& Owner);
    void OpenSessionFromSidebar(const std::string& Id)
    {
        const Session* const Target = FindSession(Id);
        if (State_.Ui.FocusProjectOnClick == true && Target != nullptr && State_.Ui.TabFilterProjectId != Target->Rec.ProjectId)
        {
            SetTabFilter(Target->Rec.ProjectId);
        }
        OpenSession(Id);
    }
    bool IsSessionOpen(const std::string& Id) const
    {
        return std::find(State_.Ui.OpenIds.begin(), State_.Ui.OpenIds.end(), Id) != State_.Ui.OpenIds.end();
    }
    bool ShuttingDown_ = false;
    static constexpr int BACKGROUND_COLS = 120;
    static constexpr int BACKGROUND_ROWS = 30;
    static constexpr const char* COMMIT_PROMPT =
        "Prepare a git commit for the changes made in this session. Review the current git diff, stage the relevant files, "
        "and write a clear commit message that accurately describes what was changed and why. Commit only; do not push.";
    static constexpr const char* PUSH_PROMPT =
        "Push the current branch's commits to GitHub (git push, using -u origin to set the upstream if the branch has none). "
        "Do not create any new commits.";
    GitReview Review_;
    bool ReviewShowActivity_ = false;
    Session* InputSession() const;
    void EnsureActiveVisible();
    void FocusSession(const std::string& Id);
    void ToggleMaximize(const std::string& Id);
    Session* CreateSession(const std::string& ProjectId, const std::string& Name, bool CurrentWorktree, bool ContinueLast, std::string& Err);
    void CloseSession(const std::string& Id, int WorktreeAction);
    // Asks for confirmation, or (with "close without confirmation" on) queues the close for after the sidebar loop.
    void RequestCloseSession(const std::string& Id);
    void CloseUnopenedSessions(const std::string& ProjectId);
    void RequestCloseUnopenedSessions(const std::string& ProjectId);
    void RemoveProject(const std::string& Id);
    bool AddProjectPath(const std::string& Path, std::string& Err, std::string* OutId = nullptr);
    std::string GeneralFolderPath() const;
    void StartGeneralSession();
    void OnStatus(Session& CurrentSession, Status OldStatus, Status NewStatus, double BusySeconds);
    void MarkDirty() { Dirty_ = true; }
    void SetTabFilter(const std::string& ProjectId)
    {
        State_.Ui.TabFilterProjectId = ProjectId;
        FilterExtraIds_.clear();
        MarkDirty();
    }
    // Swaps two sessions' positions in the open-tile order (drag-and-drop pane reordering).
    void SwapSessionOrder(const std::string& FirstId, const std::string& SecondId)
    {
        if (FirstId == SecondId)
        {
            return;
        }
        auto FirstIt = std::find(State_.Ui.OpenIds.begin(), State_.Ui.OpenIds.end(), FirstId);
        auto SecondIt = std::find(State_.Ui.OpenIds.begin(), State_.Ui.OpenIds.end(), SecondId);
        if (FirstIt == State_.Ui.OpenIds.end() || SecondIt == State_.Ui.OpenIds.end())
        {
            return;
        }
        std::swap(*FirstIt, *SecondIt);
        MarkDirty();
    }
    void SaveNow();
    void ShowToast(const std::string& Text, bool IsError = false);
    Session::StatusFn StatusCallback();

    // ---- input ----
    void HandleEvents();
    void HandleKey(const InputEvent& CurrentEvent);
    void DeleteSelectedInput(Session& Target);
    void CopySelection(Session&CurrentSession);
    void Paste(Session& CurrentSession);
    void ChangeFontSize(int Delta);

    // ---- ui ----
    void DrawRoot();
    void DrawSidebar(float Width);
    float RowCloseButtonWidth() const;
    bool RowCloseButton(const ImVec2& Pos, bool Hovered);
    void DrawMain(const float Width, const float Height);
    void DrawMainAndPanel();
    void DrawMainColumn(const float Width);
    void DrawFeedbackPanel(const float Width);
    float FeedbackPanelHeight();
    void StartFeedbackSessions();
    void DrawPauseButton();
    float PauseButtonWidth() const;
    // Sends "Pause" to every session that is busy working and remembers them so they can be told to continue.
    void PauseWorkingSessions();
    void ContinuePausedSessions();
    // Splits clipboard text into one item per numbered/bulleted entry; empty unless it holds at least two entries.
    static std::vector<std::string> ParseFeedbackList(const std::string& Text);
    bool PasteFeedbackList(std::vector<std::array<char, FEEDBACK_LINE_CAPACITY>>& Lines, const size_t Index);
    void DrawScreenshotPanel(const float Width);
    bool DrawScreenshotCard(const ScreenshotFeed::Entry& Item, const float Width);
    void DrawToolbar();
    float OverflowChipWidth() const;
    // A "..." chip with a status dot summarising the hidden sessions; clicking it lists them.
    void DrawOverflowChip(const char* const PopupId, const std::vector<const Session*>& Hidden);
    void DrawPane(Session& CurrentSession, ImVec2 Pos, ImVec2 Size, int Index);
    void DrawTerminal(Session& CurrentSession, ImVec2 Size, bool IsActive);
    void DrawFindBar(Session& Target, ImVec2 PanelPos, ImVec2 PanelSize);
    void RunFind(Session& Target, bool Forward, bool KeepCurrent);
    void DrawModals();
    void DrawAddProjectModal(const bool Enter);
    void DrawNewSessionModal(const bool Enter);
    void DrawRenameModal(const bool Enter);
    void DrawConfirmModal();
    void DrawCloseSessionModal();
    void DrawSettingsModal();
    void DrawUsageModal();
    void DrawGitHubModal();
    void DrawReviewModal();
    void DrawToasts();
    void OpenModal(Modal CurrentModal) { Modal_ = CurrentModal; ModalJustOpened_ = true; }
    // Only valid while the modal popup is the current ImGui window.
    void CloseModal()
    {
        ImGui::CloseCurrentPopup();
        Modal_ = Modal::None;
    }
    // Green until 20%, yellow by 25%, orange by 50%, red by 75%, then red that flashes harder on the way to 100%.
    ImVec4 ContextColor(const int Percent) const
    {
        struct ColorStop
        {
            float Percent;
            ImVec4 Color;
        };
        static const ColorStop STOPS[] = {{20.0f, ImVec4(0.35f, 0.80f, 0.45f, 1.0f)},
                                          {25.0f, ImVec4(0.95f, 0.85f, 0.25f, 1.0f)},
                                          {50.0f, ImVec4(0.98f, 0.55f, 0.15f, 1.0f)},
                                          {75.0f, ImVec4(0.92f, 0.22f, 0.22f, 1.0f)}};
        constexpr size_t STOP_COUNT = sizeof(STOPS) / sizeof(STOPS[0]);
        constexpr float FLASH_START_PERCENT = 75.0f;
        constexpr double FLASHES_PER_SECOND = 1.5;
        constexpr float MAX_FLASH_DIM = 0.8f;
        const float Value = static_cast<float>(Percent);
        if (Value <= STOPS[0].Percent)
        {
            return STOPS[0].Color;
        }
        if (Value >= FLASH_START_PERCENT)
        {
            ImVec4 Flashing = STOPS[STOP_COUNT - 1].Color;
            const float Depth = std::min(1.0f, (Value - FLASH_START_PERCENT) / (100.0f - FLASH_START_PERCENT));
            const float Pulse = 0.5f + 0.5f * static_cast<float>(std::sin(Now_ * FLASHES_PER_SECOND * 6.283185307));
            Flashing.w = 1.0f - MAX_FLASH_DIM * Depth * Pulse;
            return Flashing;
        }
        for (size_t Index = 1; Index < STOP_COUNT; Index++)
        {
            if (Value > STOPS[Index].Percent)
            {
                continue;
            }
            const ColorStop& Low = STOPS[Index - 1];
            const ColorStop& High = STOPS[Index];
            const float Blend = (Value - Low.Percent) / (High.Percent - Low.Percent);
            return ImVec4(Low.Color.x + (High.Color.x - Low.Color.x) * Blend, Low.Color.y + (High.Color.y - Low.Color.y) * Blend,
                          Low.Color.z + (High.Color.z - Low.Color.z) * Blend, 1.0f);
        }
        return STOPS[STOP_COUNT - 1].Color;
    }
    void DrawStatusDot(ImDrawList* DrawList, ImVec2 Center, float Radius, const Session& CurrentSession) const;
    static bool IsSessionRunning(const Session& CurrentSession)
    {
        return CurrentSession.CurrentStatus == Status::Busy || CurrentSession.CurrentStatus == Status::Starting || CurrentSession.InBackground() == true;
    }
    // The session whose dot best represents a group: waiting beats running beats unseen, otherwise the first one.
    static const Session* SummarySession(const std::vector<const Session*>& Group)
    {
        const Session* Running = nullptr;
        const Session* Unseen = nullptr;
        for (const Session* const Member : Group)
        {
            if (Member->CurrentStatus == Status::Waiting)
            {
                return Member;
            }
            if (Running == nullptr && IsSessionRunning(*Member) == true)
            {
                Running = Member;
            }
            if (Unseen == nullptr && Member->Unseen == true)
            {
                Unseen = Member;
            }
        }
        if (Running != nullptr)
        {
            return Running;
        }
        if (Unseen != nullptr)
        {
            return Unseen;
        }
        return Group.empty() == true ? nullptr : Group.front();
    }
    bool DrawBoxChar(ImDrawList* DrawList, float CellX, float CellY, float CellWidth, float CellHeight, uint32_t CodePoint, ImU32 CurrentColor);
    // Draws one small meter at the current cursor (background + proportional fill + centered "<Pct>%<Suffix>"
    // text), advancing the cursor like any other widget. Returns true if it was clicked this frame.
    bool DrawUsageMeter(ImDrawList* DrawList, float Width, const char* Label, bool HasValue, double Pct, ImU32 FillColor, float ResetProgress, const std::string& Suffix,
                        const std::string& ErrorText) const;
    ImU32 UsageFillColor(double Pct) const;
    void DrawResetProgress(ImDrawList* DrawList, ImVec2 Min, ImVec2 Max, float Progress) const;
    void SyncTheme();
    // Draws the GitHub sign-in panel inside the open modal. Returns true when the user pressed Close.
    bool DrawGitHubLogin();
    void DrawKeybindingEditor();
    void CaptureKeyBinding(const InputEvent& CurrentEvent);
    // Quick-open palette (Shift+Alt+O): jump to a project or session, or add a project file to the active session.
    struct SearchResult
    {
        enum class Kind { Project, Session, File };
        Kind Type = Kind::Project;
        std::string Label;
        std::string Detail;
        std::string Target;  // project id, session id, or file path relative to the search root
    };
    void OpenSearch();
    void RefreshSearchResults();
    void ActivateSearchResult(const SearchResult& Result);
    void DrawSearch(bool Confirm);
    void CenteredText(const std::string& Text, const ImVec4& Color) const;
    void StartGitHubCheck();
    Git::GitHubStatus ReadGitHubStatus() const;
    std::string GitHubLabel() const;
    void StartUsageFetch();
    void StartWindowPing();
    void ReadUsageFetch(bool& InFlight, bool& Done, UsageInfo& Info) const;

    // ---- state ----
    GLFWwindow* Window_;
    Store& Store_;
    State& State_;
    std::vector<std::unique_ptr<Session>> Sessions_;
    std::deque<InputEvent> Events_;
    std::string MaximizedId_;
    std::vector<std::string> FilterExtraIds_;  // sessions from other projects shown alongside the selected project
    std::string PendingCloseId_;
    double Now_ = 0;
    bool Focused_ = true, Dirty_ = false;
    double LastSave_ = 0;

    // fonts
    float UiScale_;
    bool FontsDirty_ = true;
    ImFont *UiFont_ = nullptr, *MonoFont_ = nullptr, *MonoBoldFont_ = nullptr;
    ImVec2 CellSize_{9, 18};
    float MonoFontPx_ = 16;
    ImVector<ImWchar> MonoRanges_;

    // modal state
    Modal Modal_ = Modal::None;
    bool ModalJustOpened_ = false;
    char PathBuffer_[1024] = "";
    char NameBuffer_[256] = "";
    std::string ModalError_;
    std::string ModalTarget_;  // project/session id the modal acts on
    bool RenameIsProject_ = false;
    int ProjectComboIndex_ = 0;
    bool WantWorktree_ = false;
    bool WantContinue_ = false;
    struct GitCache
    {
        std::string Path;
        bool IsGitRepo = false;
        std::string Branch;
    } GitCache_;
    std::string ConfirmTitle_, ConfirmText_;
    std::function<void()> ConfirmYesAction_;
    int CloseChoice_ = 0;
    bool CloseDirty_ = false;
    char CommandBuffer_[512] = "";
    int FontSizeEdit_ = 16;
    int PaneTitleScaleEdit_ = 100;
    bool NotifyEdit_ = true;
    bool AutoStartEdit_ = false;
    std::shared_ptr<std::atomic<bool>> WindowPingBusy_ = std::make_shared<std::atomic<bool>>(false);
    double WindowPingLastAt_ = -1000;
    bool GitHubEdit_ = false;
    KeyBindings KeysEdit_;
    int KeyCaptureIndex_ = -1;
    int KeyCaptureEscapeFrame_ = -1;
    std::string KeyCaptureError_;
    ThemeColors ThemeEdit_;
    std::map<std::string, std::array<char, 256>> SessionNameEdits_;

    struct Toast
    {
        std::string Text;
        double Until;
        bool IsError;
    };
    std::vector<Toast> Toasts_;

    // ---- theme cache (recomputed once per frame by SyncTheme()) ----
    ImU32 AccentColor_ = 0, BusyColor_ = 0, WaitingColor_ = 0, DoneColor_ = 0, IdleColor_ = 0, UsageWarningColor_ = 0, UsageFullColor_ = 0;
    ImU32 SurfaceColorU32_ = 0, PaneBgColorU32_ = 0, PaneBorderColorU32_ = 0, TerminalBgColorU32_ = 0, ChipBgColorU32_ = 0;
    ImU32 TextColorU32_ = 0, TextDisabledColorU32_ = 0;
    ImU32 SelectionColorU32_ = 0, CursorFillColorU32_ = 0, CursorOutlineColorU32_ = 0;
    ImVec4 BackgroundColorV4_{}, SurfaceColorV4_{}, PaneBgColorV4_{}, PaneBorderColorV4_{};
    ImVec4 AccentColorV4_{}, AccentHoverColorV4_{}, AccentActiveColorV4_{}, WaitingColorV4_{};
    ImVec4 TextColorV4_{}, TextDisabledColorV4_{};

    // ---- usage fetch ----
    struct UsageFetch
    {
        std::mutex Mutex;
        bool InFlight = false, Done = false;
        UsageInfo Result;
    };
    struct GitHubFetch
    {
        std::mutex Mutex;
        bool InFlight = false;
        Git::GitHubStatus Status;
    };
    std::shared_ptr<GitHubFetch> GitHubFetch_;
    GitHubLogin GitHubLogin_;

    bool FeedbackOpen_ = false;
    int FeedbackFocusIndex_ = -1;
    std::vector<std::string> PausedIds_;
    std::map<std::string, std::vector<std::array<char, FEEDBACK_LINE_CAPACITY>>> FeedbackLines_;

    ScreenshotFeed ScreenshotFeed_;
    std::string ProjectScopeId_;
    std::map<std::string, float> ShotAspects_;
    std::unique_ptr<ImageCache> Images_;
    LayoutDebug LayoutDebug_;
    std::map<std::string, TranscriptScanner> TranscriptScanners_;
    std::map<std::string, double> ScreenshotPollAt_;
    int UnseenScreenshots_ = 0;

    std::map<std::string, Handoff> Handoffs_;  // session id -> requested handoff summary
    bool AutoMode_ = false;
    std::map<std::string, AutoState> AutoStates_;
    CommandTiming CommandTiming_;
    std::map<std::string, std::vector<TranscriptScanner::ActiveCommand>> RunningCommands_;  // session id -> commands
    struct OutputTailState
    {
        double CheckAt = 0;
        std::string Line;
    };
    std::map<std::string, OutputTailState> OutputTails_;  // background output file -> its last line

    std::map<std::string, bool> UnrealProjects_;  // project id -> has a .uproject
    std::set<std::string> LiveViewIds_;           // sessions showing their live view
    std::map<std::string, double> LiveFrameCheckAt_;
    std::map<std::string, LiveView::Frame> LiveFrames_;

    PeerLinks PeerLinks_;
    PeerVeins PeerVeins_;

    ProjectLogWindow ProjectLogWindow_;
    ProjectSettingsWindow ProjectSettingsWindow_;

    NightlyBuild NightlyBuild_;
    std::string NightlyBuildId_;  // project whose nightly build window is open; empty when closed
    bool NightlyBuildFocus_ = false;
    double NightlyBuildCheckAt_ = 0;
    std::string NightlyBuildError_;
    NightlyBuild::Job NightlyDraft_;
    bool NightlyIsUnreal_ = false;  // checked when the window opens, not every frame
    bool NightlyHasScript_ = false;
    char NightlyNameBuffer_[64] = "";
    char NightlyOutputBuffer_[1024] = "";
    char NightlyEngineBuffer_[1024] = "";

    FileIndex FileIndex_;
    char SearchBuffer_[256] = "";
    std::string SearchLastQuery_;
    size_t SearchLastIndexSize_ = 0;
    std::string SearchRootProjectId_;
    std::vector<SearchResult> SearchResults_;
    int SearchSelected_ = 0;
    bool SearchFocus_ = false;
    bool SearchScrollToSelected_ = false;
    double GitHubLastCheckAt_ = -1.0e9;
    std::shared_ptr<UsageFetch> UsageFetch_;
    double UsageLastFetchAt_ = -1.0e9;
};
