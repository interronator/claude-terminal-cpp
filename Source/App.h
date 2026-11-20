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

#include "BackgroundWatch.h"
#include "BlenderLive.h"
#include "BlenderMarkup.h"
#include "BlenderReviewBatch.h"
#include "BlenderReviewHistory.h"
#include "BlenderViewControl.h"
#include "MeshGenerator.h"
#include "ChildProgress.h"
#include "CommandTiming.h"
#include "FileIndex.h"
#include "Git.h"
#include "GitReview.h"
#include "GitHubLogin.h"
#include "imgui.h"
#include "LayoutDebug.h"
#include "LiveView.h"
#include "ModelCatalog.h"
#include "NightlyBuild.h"
#include "PaneGrid.h"
#include "PeerLinks.h"
#include "ProjectLocks.h"
#include "PeerVeins.h"
#include "ProjectLog.h"
#include "ProjectLogWindow.h"
#include "ProjectSettingsWindow.h"
#include "RamLimit.h"
#include "ScreenshotFeed.h"
#include "Session.h"
#include "PeerNames.h"
#include "SessionRequests.h"
#include "Store.h"
#include "SessionTitler.h"
#include "ScannerWorker.h"
#include "ShellHistory.h"
#include "ShellOutputTail.h"
#include "ShellRow.h"
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
    void ConfirmSubmitted(Session& Target);
    // ---- sidebar: sessions nested under the session that started them ----
    struct SidebarRow
    {
        Session* Target = nullptr;
        int Depth = 0;
        bool HasChildren = false;
        int HiddenChildren = 0;        // children folded away under this row
        bool HiddenChildBusy = false;  // one of them is working or waiting
    };
    std::vector<SidebarRow> BuildSidebarRows(const std::string& ProjectId);
    void AppendSidebarRow(Session& Current, const int Depth, const std::vector<Session*>& InProject, std::set<std::string>& Placed, std::vector<SidebarRow>& Rows);
    void DrawSidebarSessionRow(const SidebarRow& Row, std::string& DragSourceId, std::string& DragTargetId);
    void DrawReview();
    void PollScreenshots(Session& CurrentSession);
    // ---- session-to-session messages, shown as veins along the pane borders ----
    void NotePeerMessages(const Session& CurrentSession, TranscriptScanner& Scanner);
    void ApplyScanResults();
    void ApplyScanResult(Session& CurrentSession, TranscriptScanner& Scanner, const std::vector<TranscriptScanner::Shot>& Shots);
    std::string PeerDisplayName(const std::string& Id) const;
    bool IsPeerBusy(const std::string& Id) const;
    struct ReplyTrack
    {
        std::time_t MessageAt = 0;
        bool ReceiverWorked = false;
        bool ReceiverSettled = false;
        double LastSeenAt = 0.0;
    };
    std::map<std::string, ReplyTrack> ReplyTracks_;
    bool ReplyHandled(const PeerLinks::View& Link);
    void DrawPeerOverlay(const ImVec2 PanesMin, const ImVec2 PanesMax, const float Gap);
    std::string FindSessionMention(const Session& Self, const Terminal& Term, int AbsRow, int Col) const;
    void ApplyMentionFocus();
    void DrawWaitingPills(ImDrawList* const DrawList, const PeerVeins::Box& Pane, const std::vector<std::pair<std::string, ImU32>>& Labels);
    void RequestDeleteAllArchived();
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
        if (Target.CurrentStatus == Status::Waiting || Target.UserIsTyping(Now_) == true)
        {
            Target.PendingPrompt = Text;
            return;
        }
        SendPrompt(Target, Text);
    }
    bool AutoActiveFor(const Session& Target) const
    {
        return AutoMode_ == true || AutoProjects_.count(Target.Rec.ProjectId) > 0 || AutoSessions_.count(Target.Rec.Id) > 0;
    }
    bool SessionIsAuto(const std::string& SessionId) const
    {
        return AutoSessions_.count(SessionId) > 0;
    }
    void SetSessionAutoMode(Session& Target, const bool Enabled)
    {
        if (Enabled == true)
        {
            AutoSessions_.insert(Target.Rec.Id);
        }
        else
        {
            AutoSessions_.erase(Target.Rec.Id);
        }
        const bool CoveredElsewhere = AutoMode_ == true || AutoProjects_.count(Target.Rec.ProjectId) > 0;
        if (CoveredElsewhere == false)
        {
            ApplyAutoChange(Target, Enabled);
        }
        ShowToast(Enabled == true ? "Auto on for this session" : "Auto off for this session");
    }
    bool ProjectIsAuto(const std::string& ProjectId) const
    {
        return AutoProjects_.count(ProjectId) > 0;
    }
    // Returns true when the session was told about the change.
    bool ApplyAutoChange(Session& Candidate, const bool Enabled)
    {
        AutoState& State = AutoStates_[Candidate.Rec.Id];
        const bool WasInformed = State.Informed;
        State = AutoState{};
        if (Candidate.IsRunning() == false || Candidate.CurrentStatus == Status::Starting)
        {
            return false;
        }
        if (Enabled == true)
        {
            SendWhenSafe(Candidate, AUTO_ON_PROMPT);
            State.Informed = true;
            return true;
        }
        if (WasInformed == true)
        {
            SendWhenSafe(Candidate, AUTO_OFF_PROMPT);
            return true;
        }
        return false;
    }
    void SetAutoMode(const bool Enabled)
    {
        AutoMode_ = Enabled;
        AutoSessions_.clear();
        int Told = 0;
        for (const std::unique_ptr<Session>& Candidate : Sessions_)
        {
            if (Enabled == false && AutoProjects_.count(Candidate->Rec.ProjectId) > 0)
            {
                continue;
            }
            Told += ApplyAutoChange(*Candidate, Enabled) == true ? 1 : 0;
        }
        ShowToast(std::string(Enabled == true ? "Auto mode on: told " : "Auto mode off: told ") + std::to_string(Told) + " session" + (Told == 1 ? "" : "s"));
    }
    void SetProjectAutoMode(const std::string& ProjectId, const bool Enabled)
    {
        if (Enabled == true)
        {
            AutoProjects_.insert(ProjectId);
        }
        else
        {
            AutoProjects_.erase(ProjectId);
        }
        int Told = 0;
        for (const std::unique_ptr<Session>& Candidate : Sessions_)
        {
            if (Candidate->Rec.ProjectId != ProjectId || (Enabled == false && AutoMode_ == true))
            {
                continue;
            }
            Told += ApplyAutoChange(*Candidate, Enabled) == true ? 1 : 0;
        }
        ShowToast(std::string(Enabled == true ? "Project auto mode on: told " : "Project auto mode off: told ") + std::to_string(Told) + " session" + (Told == 1 ? "" : "s"));
    }
    // Called every tick: a session that finished a turn gets a short "keep going" nudge after a few seconds, unless it said
    // it is done, is waiting on background tasks, or has already been nudged several times without doing real work.
    void AutoContinueTick(Session& Target)
    {
        constexpr double NUDGE_DELAY_SECONDS = 6.0;
        constexpr int MAX_NUDGES_IN_A_ROW = 4;
        if (AutoActiveFor(Target) == false || Target.IsRunning() == false || Target.CurrentStatus != Status::Idle || Target.InBackground() == true ||
            Target.PendingPrompt.empty() == false || Target.UserIsTyping(Now_) == true)
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
        const std::map<std::string, std::shared_ptr<TranscriptScanner>>::const_iterator Scanner = TranscriptScanners_.find(Target.Rec.Id);
        const std::string LastReply = Scanner != TranscriptScanners_.end() ? Scanner->second->LastAssistantText() : std::string();
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
        std::string FreshId;  // the session already started to continue the work
        std::string FreshName;
    };
    static std::string HandoffPrompt(const std::string& Path, const std::string& PeerName, const std::string& Tag)
    {
        return "Write a handoff summary so this work can be continued by a fresh Claude session that has none of this conversation's context. "
               "Save it as Markdown to " + Path + " (create the folder if needed). Include: the goal and what the user asked for; what has been "
               "done so far (files changed and why); the current state (what works, what is untested or broken, anything still running such as "
               "background tasks, editors or games); decisions made and the user's preferences and constraints from this conversation; open "
               "questions; and the exact next steps, in order. Be specific: name files, functions, commands and paths. Do not continue the work "
               "itself. A fresh session named '" + PeerName + "' (tag " + Tag + ") has already been started to take over. Once the file is written, "
               "send the full summary to it with SendMessage (find the ListAgents row whose name ends with the tag; if it is not listed yet, retry every "
               "few seconds), worded as: \"You are continuing work another session handed off. Here is its handoff summary (also saved at " + Path +
               "): <summary>. Check the current state of the files it names, then carry on with its next steps. Ask the user if anything is unclear.\" "
               "Then reply with one short line saying it was sent.";
    }
    void RequestHandoff(Session& CurrentSession)
    {
        if (Handoffs_.count(CurrentSession.Rec.Id) > 0)
        {
            return;
        }
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
        Handoffs_[CurrentSession.Rec.Id] = {Path.string(), Now, 0, false, std::string(), std::string()};
    }
    // Runs once per frame, outside any pane drawing, because starting a session changes the session list.
    void ProcessHandoffs()
    {
        std::vector<std::string> SourceIds;
        for (const std::pair<const std::string, Handoff>& Entry : Handoffs_)
        {
            SourceIds.push_back(Entry.first);
        }
        for (const std::string& SourceId : SourceIds)
        {
            const Session* const Source = FindSession(SourceId);
            const std::map<std::string, Handoff>::iterator Found = Handoffs_.find(SourceId);
            if (Found == Handoffs_.end())
            {
                continue;
            }
            if (Source == nullptr)
            {
                Handoffs_.erase(Found);
                continue;
            }
            if (Found->second.FreshId.empty() == true)
            {
                StartHandoffSession(*Source);
                continue;
            }
            const Handoff* const Polled = PollHandoff(*Source);
            if (Polled == nullptr || Polled->Ready == false)
            {
                continue;
            }
            ShowToast("Handed off to " + Polled->FreshName + ". You can close " + Source->Rec.Name + " when you're happy.");
            Handoffs_.erase(SourceId);
        }
    }
    void StartHandoffSession(const Session& Source)
    {
        const std::map<std::string, Handoff>::iterator Found = Handoffs_.find(Source.Rec.Id);
        if (Found == Handoffs_.end())
        {
            return;
        }
        const std::string SourceId = Source.Rec.Id;
        const std::string SourceName = Source.Rec.Name;
        const std::string SourceCwd = Source.Rec.Cwd;
        const std::string ProjectId = Source.Rec.ProjectId;
        std::string Name = SourceName;
        const std::string Suffix = " (cont.)";
        if (Name.size() < Suffix.size() || Name.compare(Name.size() - Suffix.size(), Suffix.size(), Suffix) != 0)
        {
            Name += Suffix;
        }
        std::string Err;
        Session* const Fresh = CreateSession(ProjectId, Name, false, false, Err);
        if (Fresh == nullptr)
        {
            ShowToast("Could not start a fresh session: " + Err, true);
            Handoffs_.erase(SourceId);
            return;
        }
        // Work in the same folder; a worktree stays owned by the original session, so closing this one never removes it.
        Fresh->Rec.Cwd = SourceCwd;
        const std::string FreshId = Fresh->Rec.Id;
        const std::string PeerName = PeerNames::For(Fresh->Rec.Name, FreshId);
        const std::string Tag = ScreenshotFinder::SessionTag(FreshId);
        OpenSession(FreshId);
        Session* const Writer = FindSession(SourceId);
        const std::map<std::string, Handoff>::iterator Again = Handoffs_.find(SourceId);
        if (Writer == nullptr || Again == Handoffs_.end())
        {
            return;
        }
        Again->second.FreshId = FreshId;
        Again->second.FreshName = Name;
        Again->second.RequestedAt = std::time(nullptr);
        SendPrompt(*Writer, HandoffPrompt(Again->second.Path, PeerName, Tag));
        ShowToast("Started " + Name + ". " + SourceName + " is writing it a handoff summary.");
    }
    // Fallback if the message between sessions fails: a short prompt telling the fresh session to read the file itself.
    void TellFreshToReadHandoff(const Session& Source)
    {
        const std::map<std::string, Handoff>::const_iterator Found = Handoffs_.find(Source.Rec.Id);
        if (Found == Handoffs_.end() || Found->second.FreshId.empty() == true)
        {
            return;
        }
        Session* const Fresh = FindSession(Found->second.FreshId);
        if (Fresh == nullptr)
        {
            return;
        }
        SendWhenSafe(*Fresh, "You are continuing work another session handed off. Read the handoff summary at " + Found->second.Path +
                                 ", check the current state of the files it names, then carry on with its next steps. Ask me if anything is unclear.");
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
            Current.Ready = Current.FreshId.empty() == false && Exists == true && Written + CLOCK_SLACK_SECONDS >= Current.RequestedAt && CurrentSession.CurrentStatus != Status::Busy;
        }
        return &Current;
    }
    static std::time_t FileTimeToTimeT(const std::filesystem::file_time_type FileTime)
    {
        const std::chrono::system_clock::time_point System =
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(FileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        return std::chrono::system_clock::to_time_t(System);
    }

    // Launches Claude with the project-log and screenshot-tagging instructions for the session's project.
    bool StartSession(Session& Target, const int Cols, const int Rows, std::string& Err)
    {
        RamLimit_.Refresh(State_.AppSettings, Sessions_, Now_);
        if (RamLimit_.IsOver() == true)
        {
            Err = RamLimit_.BlockedMessage();
            return false;
        }
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
            if (IsBlenderSession(Target) == true)
            {
                BlenderLive::EnsureScript(ProjectDir);
                Target.ExtraGuidance += " " + BlenderLive::Guidance(ProjectDir, ScreenshotFinder::SessionShotDirectory(ProjectDir, Tag).string(), Tag);
            }
        }
        SessionRequests::EnsureInbox();
        ChildProgress::EnsureDir();
        ProjectLocks::EnsureScript();
        Target.ExtraGuidance += (Target.ExtraGuidance.empty() == true ? "" : " ") + ProjectLocks::Guidance(Tag);
        Target.ExtraGuidance += " " + BackgroundWatch::Guidance();
        if (Target.Rec.ParentId.empty() == false)
        {
            Target.ExtraGuidance += (Target.ExtraGuidance.empty() == true ? "" : " ") + ChildProgress::Guidance(Tag);
        }
        Target.LaunchModel = Target.Rec.Model.empty() == false ? Target.Rec.Model : State_.AppSettings.DefaultModel;
        Target.LaunchPermissionMode = State_.AppSettings.DefaultPermissionMode;
        Target.ExtraGuidance += (Target.ExtraGuidance.empty() == true ? "" : " ") + SessionRequests::Guidance(Tag, PeerNames::For(Target.Rec.Name, Target.Rec.Id));
        AutoState& Auto = AutoStates_[Target.Rec.Id];
        Auto = AutoState{};
        if (AutoActiveFor(Target) == true)
        {
            Target.ExtraGuidance += std::string(" ") + AUTO_ON_PROMPT;
            Auto.Informed = true;
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
    bool IsBlenderSession(const Session& Target)
    {
        const std::map<std::string, bool>::const_iterator Known = BlenderProjects_.find(Target.Rec.ProjectId);
        if (Known != BlenderProjects_.end())
        {
            return Known->second;
        }
        const bool IsBlender = BlenderLive::IsBlenderProject(SessionProjectDir(Target));
        BlenderProjects_[Target.Rec.ProjectId] = IsBlender;
        return IsBlender;
    }
    bool DoesMeshWork(const Session& Target);
    Session* ReviewTargetFor(const Session& Root);
    bool HasFreshBlenderFrame(const Session& Target);
    void ToggleBlenderReview(const std::string& SessionId);
    void OpenBlenderReview(const std::string& RequestedId);
    void DrawBlenderReview();
    void TouchBlenderLive(Session& Target);
    void CloseBlenderReview();
    std::string BlenderReviewerName(const Session& Mesh) const;
    bool IsBlenderReviewer(const Session& Candidate) const;
    Session* FindBlenderReviewer(const Session& Mesh) const;
    bool InBlenderReview(const std::string& SessionId) const;
    std::string BlenderReviewerBrief(const Session& Mesh, const Session& Reviewer);
    Session* EnsureBlenderReviewer(Session& Mesh);
    void SendToBlenderReviewer(Session& Reviewer, const std::string& Text);
    void DrawBlenderRightHeader(Session& Mesh, Session* const Reviewer);
    void DrawBlenderReviewLeft(Session& Target, const float Width, const float Height);
    void DrawBlenderLeftTabs(Session& Target, const float Width, const float Height);
    void DrawMeshGeneratorTab(Session& Target);
    void DrawMeshGeneratorStatus();
    void DrawMeshGeneratorResult(Session& Target);
    void DrawMeshGeneratorAdvice();
    void DrawMeshGeneratorPathField(const char* const Label, std::string& Value);
    void AdvanceMeshGeneratorQueue();
    void PollMeshGenerator();
    void RefreshMeshGeneratorCache();
    long long GeneratorFileKey(const std::string& Path) const;
    void StartMeshGeneratorStage(const MeshGenerator::Stage Kind, const bool RunAll);
    void SendMeshGeneratorToReviewer(Session& Target);
    void DrawBlenderLiveToolbar(Session& Target, const bool LiveFresh, const bool HasPicture);
    void DrawBlenderExistingShotPicker(Session& Target, const std::string& OwnDirectory);
    void UseBlenderExistingShot(const Session& Target, const std::string& SourcePath);
    void DrawBlenderMarkToolbar();
    void DrawBlenderSnapshotStrip(Session& Target);
    void HandleBlenderViewInput(const ImVec2 Min, const ImVec2 Max, const bool Enabled);
    void BeginBlenderMarking(const Session& Target, const BlenderMarkup::Tool Kind);
    void PollBlenderCapture(const Session& Target);
    void FreezeBlenderFromLive(const Session& Target, const BlenderMarkup::Tool Kind, const std::string& Id);
    void StartBlenderMarking(const BlenderMarkup::Tool Kind);
    void FinishBlenderSnapshot();
    void SendBlenderReview(Session& Target, const bool ToReviewer, const std::string& GeneratorNote = std::string());
    std::string BlenderOwnDirectory(const Session& Target);
    float DrawLiveView(Session& CurrentSession, const ImVec2 PaneTopLeft, const ImVec2 PaneSize, const float HeaderHeight);
    float DrawChildBars(const Session& CurrentSession, const ImVec2 PaneTopLeft, const ImVec2 PaneSize, const float HeaderHeight, const float TopY);
    void PollChildProgress();
    void WatchBackgroundTasks(Session& CurrentSession);
    void DrawPaneModelPicker(Session& CurrentSession, const float Width);
    float PaneModelPickerWidth(const Session& CurrentSession) const;
    void DrawContextMeter(const Session& CurrentSession, const float BarWidth, const float HeaderHeight);
    std::int64_t ContextWindowOf(const TranscriptScanner& Scanner) const;
    std::string ModelLabelFor(const Session& CurrentSession) const;
    static std::string FormatTokens(const std::int64_t Tokens);
    bool CanAskClaudeToUseGit(const Session& CurrentSession) const
    {
        return State_.AppSettings.GitHubEnabled == true && ReadGitHubStatus().Value == Git::GitHubStatus::State::LoggedIn && CurrentSession.IsRunning() == true;
    }
    // Shell commands, background shells and monitors the session is running, with elapsed time and a rough guess of
    // when each will finish, drawn over the top-left of its terminal.
    void DrawShellActivity(const Session& CurrentSession, const ImVec2 PaneTopLeft, const ImVec2 PaneSize, const float HeaderHeight, const float TopY);
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
    static constexpr const char* REVIEW_BUTTON_LABEL = "   Review";
    static constexpr const char* BLENDER_REVIEWER_MODEL = "opus";
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
    void DrawNightlyBuildNow(const Project& Owner);
    void DrawNightlyBuildFields(const Project& Owner, const std::string& JobName, const float LabelWidth, const float FieldWidth);
    NightlyBuild::Job NightlyBuildDraftJob() const;
    void DrawNightlyBuildResult(const NightlyBuild::Job& Item);
    void DrawNightlyBuildFileButtons(const Project& Owner, const NightlyBuild::Job& Item);
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
    void BeginNewSession(const std::string& ProjectId);
    void StartSessionWithOptions(const std::string& ProjectId, bool Worktree, bool ContinueLast);
    void DrawNewSessionChoices(const Project& CurrentProject);
    void AutoTitleTick(Session& CurrentSession);
    void ApplyAutoTitles();
    void PollSessionRequests();
    void WatchRequestedStart(Session& Target);
    static constexpr double NO_TASK_WARN_SECONDS = 60.0;
    std::string RequestProjectId(const Session* const Sender, const std::string& Cwd, std::string& Err);
    void OnStatus(Session& CurrentSession, Status OldStatus, Status NewStatus, double BusySeconds);
    void MarkDirty() { Dirty_ = true; }
    void FocusFamily(const std::string& ParentId);
    std::vector<std::string> GroupOrder(const std::vector<std::string>& Ids) const;
    std::string FamilyRootId(const Session& Target, const std::vector<const Session*>& Among) const;
    bool HasChildSessions(const std::string& ParentId) const;
    void SetTabFilter(const std::string& ProjectId)
    {
        State_.Ui.TabFilterProjectId = ProjectId;
        FilterExtraIds_.clear();
        FamilyFocusIds_.clear();
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
    void DrawEmptyMain(const ImVec2& Avail);
    void DrawMainAndPanel();
    void DrawMainColumn(const float Width);
    void DrawFeedbackPanel(const float Width);
    float FeedbackPanelHeight();
    void StartFeedbackSessions();
    void DrawPauseButton();
    float PauseButtonWidth() const;
    float ViewArrowWidth() const;
    void DrawViewMenu(const std::string& GitHubText);
    // Sends "Pause" to every session that is busy working and remembers them so they can be told to continue.
    void PauseWorkingSessions();
    void ContinuePausedSessions();
    // Splits clipboard text into one item per numbered/bulleted entry; empty unless it holds at least two entries.
    static std::vector<std::string> ParseFeedbackList(const std::string& Text);
    bool PasteFeedbackList(std::vector<std::array<char, FEEDBACK_LINE_CAPACITY>>& Lines, const size_t Index);
    // The right side panel: a tab strip over the Screenshots and Shells tabs.
    void DrawRightPanel(const float Width);
    void DrawScreenshotsTab();
    void DrawShellsTab();
    void DrawShellCard(const TranscriptScanner::ActiveCommand& Command, const float Width, const bool Finished, const std::time_t EndedAt);
    ShellRow BuildShellRow(const TranscriptScanner::ActiveCommand& Command, const std::time_t Now) const;
    std::set<std::string> SelectedSessionFamily() const;
    std::vector<const Session*> ShellPanelSessions() const;
    int RunningShellCount() const;
    ShellHistory ShellHistory_;
    ShellOutputTail ShellOutputTail_;
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
    void DrawStatusDot(ImDrawList* DrawList, ImVec2 Center, float Radius, const Session& CurrentSession, const bool GreyWhenIdle = false) const;
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
    std::set<std::string> FamilyFocusIds_;     // when not empty, only these sessions (a parent and its descendants) are shown
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
    int RamLimitModeEdit_ = 0;
    int RamLimitPercentEdit_ = 50;
    int RamLimitGbEdit_ = 16;
    RamLimit RamLimit_;
    bool NotifyEdit_ = true;
    bool AskSessionNameEdit_ = false;
    SessionTitler Titler_;
    bool AutoStartEdit_ = false;
    std::string ModelEdit_;
    ModelCatalog ModelCatalog_{std::filesystem::path(Store::DefaultPath()).parent_path() / "Models.json"};
    std::string PermissionModeEdit_;
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
    std::map<std::string, std::shared_ptr<TranscriptScanner>> TranscriptScanners_;
    ScannerWorker ScannerWorker_;
    std::map<std::string, double> ScreenshotPollAt_;
    int UnseenScreenshots_ = 0;

    std::map<std::string, Handoff> Handoffs_;  // session id -> requested handoff summary
    bool AutoMode_ = false;
    std::set<std::string> AutoProjects_;
    std::set<std::string> AutoSessions_;
    std::map<std::string, AutoState> AutoStates_;
    CommandTiming CommandTiming_;
    std::map<std::string, std::vector<TranscriptScanner::ActiveCommand>> RunningCommands_;  // session id -> commands
    struct OutputTailState
    {
        double CheckAt = 0;
        std::string Line;
    };
    std::map<std::string, OutputTailState> OutputTails_;  // background output file -> its last line

    std::map<std::string, bool> BlenderProjects_;  // project id -> has a .blend file
    std::string BlenderReviewId_;  // session shown in the Blender review window; empty when closed
    bool BlenderReviewFocus_ = false;
    double BlenderTouchAt_ = 0;
    float BlenderSplitFraction_ = 0.62f;
    std::string BlenderFrozenPath_;  // the picture being marked up; empty while the live view is shown
    BlenderMarkup BlenderMarkup_;
    BlenderReviewBatch BlenderBatch_;
    std::string BlenderBatchOwnerId_;  // session the unsent snapshots belong to
    int BlenderShownSnapshot_ = -1;    // snapshot shown large in the picture area; -1 shows the live view
    bool BlenderRightShowsMesh_ = false;  // review window's right side: the model's session instead of its reviewer
    BlenderReviewHistory BlenderHistory_;
    MeshGenerator Generator_;
    MeshGenerator::Progress GeneratorProgress_;
    MeshGenerator::Report GeneratorReport_;
    double GeneratorPolledAt_ = 0;
    double GeneratorReportAt_ = 0;
    bool GeneratorHandled_ = true;
    std::vector<std::string> GeneratorConcepts_;
    std::map<std::string, long long> GeneratorFileKeys_;
    bool GeneratorHasMesh_ = false;
    bool BlenderSaveBlend_ = true;                 // ask Blender for a .blend copy with each snapshot
    std::string BlenderPendingId_;                 // snapshot id Blender is still capturing; empty when none
    double BlenderPendingAt_ = 0;
    BlenderMarkup::Tool BlenderPendingTool_ = BlenderMarkup::Tool::Rectangle;
    std::string BlenderFrozenId_;                  // names the files of the picture being marked
    std::string BlenderFrozenStatePath_;           // <tag>_snap_<id>.json for it, when Blender wrote one
    std::string BlenderFrozenBlendPath_;           // .blend copy taken with it, when there is one
    BlenderViewControl BlenderView_;
    std::map<std::string, bool> UnrealProjects_;  // project id -> has a .uproject
    std::set<std::string> LiveViewIds_;           // sessions showing their live view
    std::map<std::string, double> LiveFrameCheckAt_;
    std::map<std::string, LiveView::Frame> LiveFrames_;

    PeerLinks PeerLinks_;
    SessionRequests SessionRequests_;
    ChildProgress ChildProgress_;
    BackgroundWatch BackgroundWatch_;
    std::map<std::string, std::vector<TranscriptScanner::ActiveCommand>> OverdueShells_;  // session id -> probably hung commands
    std::map<std::string, double> BackgroundCheckAt_;
    PeerVeins PeerVeins_;
    std::string MentionHoverFromId_, MentionHoverToId_;  // set while the mouse is over a session mentioned in a terminal
    std::string PendingMentionFocusId_;
    PaneGrid PaneGrid_;

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
