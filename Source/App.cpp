#include "App.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
#include <thread>

#include "Git.h"
#include "BuildScripts.h"
#include "ImageCache.h"
#include "Platform.h"
#include "Style.h"
#include "UiHelpers.h"
#include "Util.h"

namespace fs = std::filesystem;


// ---------------------------------------------------------------------------------------------
// construction, fonts
// ---------------------------------------------------------------------------------------------

App::App(GLFWwindow* Window, Store& Store, float UiScale)
    : Window_(Window), Store_(Store), State_(Store.AppState), UiScale_(UiScale), Images_(std::make_unique<ImageCache>())
{
    for (const SessionRec& Rec : State_.Sessions)
    {
        Sessions_.push_back(std::make_unique<Session>(Rec));
    }
    UiState& Ui = State_.Ui;
    Ui.OpenIds.erase(std::remove_if(Ui.OpenIds.begin(), Ui.OpenIds.end(),
                                    [&](const std::string& Id) { return FindSession(Id) == nullptr; }),
                     Ui.OpenIds.end());
    if (FindSession(Ui.ActiveId) == nullptr)
    {
        Ui.ActiveId = Ui.OpenIds.empty() ? "" : Ui.OpenIds.front();
    }
    if (Ui.Mode != "tile" && Ui.Mode != "tabs")
    {
        Ui.Mode = "tile";
    }
    Session::Wake = [] { glfwPostEmptyEvent(); };
    CommandTiming_.Start(std::filesystem::path(Store::DefaultPath()).parent_path() / "command_durations.json", TranscriptScanner::ProjectsRoot());
}

App::~App()
{
}

bool App::RebuildFontsIfNeeded()
{
    if (FontsDirty_ == false)
    {
        return false;
    }
    FontsDirty_ = false;
    ImGuiIO& Io = ImGui::GetIO();
    Io.Fonts->Clear();

    const std::string Windir = UiHelpers::EnvOr("WINDIR").empty() ? "C:\\Windows" : UiHelpers::EnvOr("WINDIR");
    const std::string UiPath = UiHelpers::FirstExisting({UiHelpers::EnvOr("CT_FONT_UI"), Windir + "\\Fonts\\segoeui.ttf",
                                              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                              "/System/Library/Fonts/Helvetica.ttc"});
    const std::string MonoPath = UiHelpers::FirstExisting({UiHelpers::EnvOr("CT_FONT_MONO"), Windir + "\\Fonts\\CascadiaMono.ttf",
                                                Windir + "\\Fonts\\consola.ttf",
                                                "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                                "/System/Library/Fonts/Menlo.ttc"});
    const std::string BoldPath = UiHelpers::FirstExisting({UiHelpers::EnvOr("CT_FONT_MONO_BOLD"), Windir + "\\Fonts\\consolab.ttf",
                                                "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"});
    const std::string SymbolPath = UiHelpers::FirstExisting({Windir + "\\Fonts\\seguisym.ttf"});

    // Glyphs Claude Code uses: Latin, Greek/Cyrillic, punctuation, arrows, box drawing, blocks,
    // geometric shapes, dingbats (spinner stars, prompt chevron), braille, powerline.
    ImFontGlyphRangesBuilder RangesBuilder;
    RangesBuilder.AddRanges(Io.Fonts->GetGlyphRangesDefault());
    static const ImWchar EXTRA_RANGES[] = {0x0100, 0x024F, 0x0370, 0x03FF, 0x0400, 0x04FF, 0x2000, 0x2BFF, 0xE0A0, 0xE0D7, 0};
    RangesBuilder.AddRanges(EXTRA_RANGES);
    MonoRanges_.clear();
    RangesBuilder.BuildRanges(&MonoRanges_);

    const float UiPx = std::round(15.0f * UiScale_);
    MonoFontPx_ = std::round((float)State_.AppSettings.FontSize * UiScale_);

    ImFontConfig UiConfig;
    UiConfig.OversampleH = 2;
    UiFont_ = UiPath.empty() ? Io.Fonts->AddFontDefault() : Io.Fonts->AddFontFromFileTTF(UiPath.c_str(), UiPx, &UiConfig);

    ImFontConfig MonoConfig;
    MonoConfig.OversampleH = 1;
    MonoConfig.OversampleV = 1;
    MonoConfig.PixelSnapH = true;
    if (MonoPath.empty())
    {
        MonoFont_ = Io.Fonts->AddFontDefault();
    }
    else
    {
        MonoFont_ = Io.Fonts->AddFontFromFileTTF(MonoPath.c_str(), MonoFontPx_, &MonoConfig, MonoRanges_.Data);
        if (SymbolPath.empty() == false)
        {
            ImFontConfig MergeConfig = MonoConfig;
            MergeConfig.MergeMode = true;
            static const ImWchar SYMBOL_RANGES[] = {0x2190, 0x21FF, 0x2300, 0x23FF, 0x25A0, 0x25FF,
                                                    0x2600, 0x27BF, 0x2B00, 0x2BFF, 0};
            Io.Fonts->AddFontFromFileTTF(SymbolPath.c_str(), MonoFontPx_, &MergeConfig, SYMBOL_RANGES);
        }
    }
    MonoBoldFont_ = nullptr;
    if (BoldPath.empty() == false)
    {
        static const ImWchar BOLD_RANGES[] = {0x20, 0xFF, 0};
        MonoBoldFont_ = Io.Fonts->AddFontFromFileTTF(BoldPath.c_str(), MonoFontPx_, &MonoConfig, BOLD_RANGES);
    }
    Io.Fonts->Build();

    ImVec2 const GlyphSize = MonoFont_->CalcTextSizeA(MonoFont_->FontSize, FLT_MAX, 0.0f, "M");
    CellSize_.x = std::max(1.0f, std::round(GlyphSize.x));
    CellSize_.y = std::ceil(MonoFont_->FontSize * 1.18f);
    return true;
}

void App::SyncTheme()
{
    const ThemeColors& Theme = State_.AppSettings.Theme;
    const std::function<ImU32(const ThemeColor&)> ToU32 = [](const ThemeColor& Color) { return ImGui::ColorConvertFloat4ToU32(Style::ToImVec4(Color)); };
    AccentColor_ = ToU32(Theme.Accent);
    BusyColor_ = ToU32(Theme.StatusBusy);
    WaitingColor_ = ToU32(Theme.StatusWaiting);
    DoneColor_ = ToU32(Theme.StatusDone);
    IdleColor_ = ToU32(Theme.StatusIdle);
    UsageWarningColor_ = ToU32(Theme.UsageWarning);
    UsageFullColor_ = ToU32(Theme.UsageFull);
    SurfaceColorU32_ = ToU32(Theme.Surface);
    PaneBgColorU32_ = ToU32(Theme.PaneBg);
    PaneBorderColorU32_ = ToU32(Theme.PaneBorder);
    TerminalBgColorU32_ = ToU32(Theme.TerminalBg);
    ChipBgColorU32_ = ImGui::ColorConvertFloat4ToU32(Style::ShadeColor(Theme.Surface, 0.06f));
    TextColorU32_ = ToU32(Theme.Text);
    TextDisabledColorU32_ = ToU32(Theme.TextDisabled);
    SelectionColorU32_ = ToU32(Theme.Selection);
    CursorFillColorU32_ = ToU32(Theme.Cursor);
    CursorOutlineColorU32_ = ImGui::ColorConvertFloat4ToU32(
        ImVec4(Theme.Cursor.Red, Theme.Cursor.Green, Theme.Cursor.Blue, std::clamp(Theme.Cursor.Alpha + 0.12f, 0.0f, 1.0f)));

    BackgroundColorV4_ = Style::ToImVec4(Theme.Background);
    SurfaceColorV4_ = Style::ToImVec4(Theme.Surface);
    PaneBgColorV4_ = Style::ToImVec4(Theme.PaneBg);
    PaneBorderColorV4_ = Style::ToImVec4(Theme.PaneBorder);
    AccentColorV4_ = Style::ToImVec4(Theme.Accent);
    AccentHoverColorV4_ = Style::ShadeColor(Theme.Accent, 0.05f);
    AccentActiveColorV4_ = Style::ShadeColor(Theme.Accent, -0.07f);
    WaitingColorV4_ = Style::ToImVec4(Theme.StatusWaiting);
    TextColorV4_ = Style::ToImVec4(Theme.Text);
    TextDisabledColorV4_ = Style::ToImVec4(Theme.TextDisabled);
}

void App::ChangeFontSize(int Delta)
{
    int const NewSize = Delta == 0 ? 16 : std::clamp(State_.AppSettings.FontSize + Delta, 8, 36);
    if (NewSize == State_.AppSettings.FontSize)
    {
        return;
    }
    State_.AppSettings.FontSize = NewSize;
    FontsDirty_ = true;
    MarkDirty();
}

// ---------------------------------------------------------------------------------------------
// model
// ---------------------------------------------------------------------------------------------

Session* App::FindSession(const std::string& Id) const
{
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Rec.Id == Id)
        {
            return CurrentSession.get();
        }
    }
    return nullptr;
}

Project* App::FindProject(const std::string& Id)
{
    for (Project& CurrentProject : State_.Projects)
    {
        if (CurrentProject.Id == Id)
        {
            return &CurrentProject;
        }
    }
    return nullptr;
}

Session* App::ActiveSession() const
{
    return FindSession(State_.Ui.ActiveId);
}

Session* App::InputSession() const
{
    Session* const Active = ActiveSession();
    if (Active == nullptr)
    {
        return nullptr;
    }
    const std::vector<std::string> Visible = VisibleIds();
    if (std::find(Visible.begin(), Visible.end(), Active->Rec.Id) == Visible.end())
    {
        return nullptr;
    }
    return Active;
}

void App::EnsureActiveVisible()
{
    const std::vector<std::string> Visible = VisibleIds();
    if (Visible.empty() == true || std::find(Visible.begin(), Visible.end(), State_.Ui.ActiveId) != Visible.end())
    {
        return;
    }
    State_.Ui.ActiveId = Visible.front();
    MarkDirty();
}

// Open sessions passing the Running/Requires-action filter only (project filter NOT applied) —
// this is what the toolbar's tab bar shows, so every project's group stays visible when one is selected.
std::vector<std::string> App::ActionFilteredIds() const
{
    std::vector<std::string> Result;
    for (const std::string& Id : State_.Ui.OpenIds)
    {
        Session* const SessionPtr = FindSession(Id);
        if (SessionPtr == nullptr)
        {
            continue;
        }
        // The active session and sessions not yet started always stay visible: a hidden pane is never
        // drawn, and drawing is what starts a session.
        const bool AlwaysShown = Id == State_.Ui.ActiveId || SessionPtr->Attempted == false;
        if (AlwaysShown == false && State_.Ui.ActionFilter == "running" && SessionPtr->IsRunning() == false)
        {
            continue;
        }
        if (AlwaysShown == false && State_.Ui.ActionFilter == "action" && SessionPtr->RequiresAction() == false)
        {
            continue;
        }
        Result.push_back(Id);
    }
    return Result;
}

// Same as ActionFilteredIds(), plus the project filter — this is what's actually rendered as panes/tiles.
std::vector<std::string> App::FilteredOpenIds() const
{
    std::vector<std::string> Result;
    for (const std::string& Id : ActionFilteredIds())
    {
        Session* const SessionPtr = FindSession(Id);
        if (SessionPtr == nullptr)
        {
            continue;
        }
        const bool InExtras = std::find(FilterExtraIds_.begin(), FilterExtraIds_.end(), Id) != FilterExtraIds_.end();
        if (State_.Ui.TabFilterProjectId.empty() == false && SessionPtr->Rec.ProjectId != State_.Ui.TabFilterProjectId && InExtras == false)
        {
            continue;
        }
        Result.push_back(Id);
    }
    return Result;
}

std::vector<std::string> App::VisibleIds() const
{
    std::vector<std::string> Open = FilteredOpenIds();
    if (State_.Ui.Mode == "tabs")
    {
        if (Open.empty())
        {
            return {};
        }
        bool const HasActive = std::find(Open.begin(), Open.end(), State_.Ui.ActiveId) != Open.end();
        return {HasActive ? State_.Ui.ActiveId : Open.front()};
    }
    if (MaximizedId_.empty() == false && std::find(Open.begin(), Open.end(), MaximizedId_) != Open.end())
    {
        return {MaximizedId_};
    }
    return Open;
}

void App::FocusSession(const std::string& Id)
{
    if (std::find(State_.Ui.OpenIds.begin(), State_.Ui.OpenIds.end(), Id) == State_.Ui.OpenIds.end())
    {
        return;
    }
    Session* const CurrentSession = FindSession(Id);
    if (CurrentSession == nullptr)
    {
        return;
    }
    ProjectScopeId_.clear();
    if (State_.Ui.ActiveId != Id)
    {
        State_.Ui.ActiveId = Id;
        MarkDirty();
    }
    CurrentSession->Unseen = false;
}

void App::OpenSession(const std::string& Id)
{
    Session* const CurrentSession = FindSession(Id);
    if (CurrentSession == nullptr)
    {
        return;
    }
    std::vector<std::string>& Open = State_.Ui.OpenIds;
    if (std::find(Open.begin(), Open.end(), Id) == Open.end())
    {
        Open.push_back(Id);
    }
    if (MaximizedId_.empty() == false && MaximizedId_ != Id)
    {
        MaximizedId_.clear();
    }
    const bool OutsideFilter = State_.Ui.TabFilterProjectId.empty() == false && State_.Ui.TabFilterProjectId != CurrentSession->Rec.ProjectId;
    if (OutsideFilter == true && std::find(FilterExtraIds_.begin(), FilterExtraIds_.end(), Id) == FilterExtraIds_.end())
    {
        FilterExtraIds_.push_back(Id);
    }
    FocusSession(Id);
    MarkDirty();
}

void App::HideSession(const std::string Id)
{
    std::vector<std::string>& Open = State_.Ui.OpenIds;
    Open.erase(std::remove(Open.begin(), Open.end(), Id), Open.end());
    FilterExtraIds_.erase(std::remove(FilterExtraIds_.begin(), FilterExtraIds_.end(), Id), FilterExtraIds_.end());
    if (State_.Ui.ActiveId == Id)
    {
        State_.Ui.ActiveId = Open.empty() ? "" : Open.back();
    }
    if (MaximizedId_ == Id)
    {
        MaximizedId_.clear();
    }
    if (Session* const CurrentSession = FindSession(State_.Ui.ActiveId))
    {
        CurrentSession->Unseen = false;
    }
    MarkDirty();
}

void App::ToggleMaximize(const std::string& Id)
{
    if (State_.Ui.Mode == "tabs" || Id.empty())
    {
        return;
    }
    MaximizedId_ = MaximizedId_ == Id ? "" : Id;
}

Session* App::CreateSession(const std::string& ProjectId, const std::string& Name, bool CurrentWorktree, bool ContinueLast, std::string& Err)
{
    Project* const CurrentProject = FindProject(ProjectId);
    if (CurrentProject == nullptr)
    {
        Err = "Project not found";
        return nullptr;
    }
    if (UiHelpers::FileExists(CurrentProject->Path) == false)
    {
        Err = "Folder no longer exists: " + CurrentProject->Path;
        return nullptr;
    }
    int Count = 0;
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Rec.ProjectId == ProjectId)
        {
            Count++;
        }
    }
    std::string FinalName = Util::Trim(Name);
    if (FinalName.empty())
    {
        FinalName = "Session " + std::to_string(Count + 1);
    }

    SessionRec CurrentRec;
    CurrentRec.Id = Util::Uuid4();
    CurrentRec.ProjectId = ProjectId;
    CurrentRec.Name = FinalName;
    CurrentRec.Cwd = CurrentProject->Path;
    CurrentRec.ClaudeSessionId = Util::Uuid4();
    CurrentRec.CreatedAt = (long long)std::time(nullptr);
    if (ContinueLast == true && CurrentWorktree == false)
    {
        std::string const PreviousId = Util::LatestClaudeSessionId(CurrentRec.Cwd);
        if (PreviousId.empty() == true)
        {
            Err = "No earlier conversation found for this folder";
            return nullptr;
        }
        CurrentRec.ClaudeSessionId = PreviousId;
        CurrentRec.Started = true;
    }
    if (CurrentWorktree)
    {
        Git::Worktree WorktreeInfo;
        if (Git::CreateWorktree(CurrentProject->Path, FinalName, WorktreeInfo, Err) == false)
        {
            return nullptr;
        }
        CurrentRec.WorktreeInfo = Worktree{WorktreeInfo.RepoRoot, WorktreeInfo.Branch, WorktreeInfo.Path};
        CurrentRec.Cwd = WorktreeInfo.Cwd;
    }
    Sessions_.push_back(std::make_unique<Session>(CurrentRec));
    MarkDirty();
    return Sessions_.back().get();
}

void App::CloseSession(const std::string& Id, int WorktreeAction)
{
    Session* CurrentSession = FindSession(Id);
    if (CurrentSession == nullptr)
    {
        return;
    }
    CurrentSession->Kill(Now_, nullptr);
    ScreenshotFeed_.RemoveSession(Id);
    TranscriptScanners_.erase(Id);
    ScreenshotPollAt_.erase(Id);
    const Project* const Owner = FindProject(CurrentSession->Rec.ProjectId);
    State_.Archive.push_back({CurrentSession->Rec.Id, CurrentSession->Rec.Name, Owner != nullptr ? Owner->Name : std::string(), CurrentSession->Rec});
    std::string Warning;
    if (CurrentSession->Rec.WorktreeInfo && WorktreeAction > 0)
    {
        bool BranchKept = false;
        std::string Err;
        if (Git::RemoveWorktree(CurrentSession->Rec.WorktreeInfo->RepoRoot, CurrentSession->Rec.WorktreeInfo->Path, CurrentSession->Rec.WorktreeInfo->Branch,
                                WorktreeAction == 2, BranchKept, Err))
        {
            if (BranchKept)
            {
                Warning = "Worktree removed. Branch " + CurrentSession->Rec.WorktreeInfo->Branch + " was kept (unmerged commits).";
            }
        }
        else
        {
            Warning = "Could not remove the worktree (" + Err + "). It was left at " + CurrentSession->Rec.WorktreeInfo->Path;
        }
    }
    HideSession(Id);
    Sessions_.erase(std::remove_if(Sessions_.begin(), Sessions_.end(),
                                   [&](const std::unique_ptr<Session>& CurrentSession) { return CurrentSession->Rec.Id == Id; }),
                    Sessions_.end());
    MarkDirty();
    if (Warning.empty() == false)
    {
        ShowToast(Warning);
    }
}

void App::RequestDeleteAllArchived()
{
    if (State_.Ui.CloseWithoutConfirm == true)
    {
        State_.Archive.clear();
        MarkDirty();
        return;
    }
    ConfirmTitle_ = "Delete all archived sessions?";
    ConfirmText_ = "This permanently removes every entry from the Archive.";
    ConfirmYesAction_ = [this]
    {
        State_.Archive.clear();
        MarkDirty();
    };
    OpenModal(Modal::Confirm);
}

void App::RestoreArchivedSession(const size_t ArchiveIndex)
{
    if (ArchiveIndex >= State_.Archive.size())
    {
        return;
    }
    const SessionRec Rec = State_.Archive[ArchiveIndex].Rec;
    if (Rec.ClaudeSessionId.empty() == true || FindProject(Rec.ProjectId) == nullptr)
    {
        ShowToast("This session can't be restored (its project or history was not saved).", true);
        return;
    }
    if (FindSession(Rec.Id) != nullptr || UiHelpers::FileExists(Rec.Cwd) == false)
    {
        ShowToast("This session can't be restored (its folder no longer exists).", true);
        return;
    }
    State_.Archive.erase(State_.Archive.begin() + static_cast<std::ptrdiff_t>(ArchiveIndex));
    Sessions_.push_back(std::make_unique<Session>(Rec));
    OpenSession(Rec.Id);
}

void App::RequestCloseSession(const std::string& Id)
{
    const Session* const Target = FindSession(Id);
    if (Target == nullptr)
    {
        return;
    }
    if (State_.Ui.CloseWithoutConfirm == true)
    {
        PendingCloseId_ = Id;
        return;
    }
    ModalTarget_ = Id;
    if (Target->Rec.WorktreeInfo)
    {
        CloseChoice_ = 0;
        CloseDirty_ = Git::IsDirty(Target->Rec.WorktreeInfo->Path);
        OpenModal(Modal::CloseSession);
        return;
    }
    ConfirmTitle_ = "Close \"" + Target->Rec.Name + "\"?";
    ConfirmText_ = "This stops Claude in this session and moves it to the Archive. The conversation stays in Claude's own history.";
    ConfirmYesAction_ = [this, Id] { CloseSession(Id, 0); };
    OpenModal(Modal::Confirm);
}

void App::CloseUnopenedSessions(const std::string& ProjectId)
{
    std::vector<std::string> IdsToClose;
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Rec.ProjectId != ProjectId)
        {
            continue;
        }
        const bool IsOpen = std::find(State_.Ui.OpenIds.begin(), State_.Ui.OpenIds.end(), CurrentSession->Rec.Id) != State_.Ui.OpenIds.end();
        if (IsOpen == true)
        {
            continue;
        }
        IdsToClose.push_back(CurrentSession->Rec.Id);
    }
    for (const std::string& SessionId : IdsToClose)
    {
        CloseSession(SessionId, 0);
    }
}

void App::RequestCloseUnopenedSessions(const std::string& ProjectId)
{
    int UnopenedCount = 0;
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Rec.ProjectId != ProjectId)
        {
            continue;
        }
        const bool IsOpen = std::find(State_.Ui.OpenIds.begin(), State_.Ui.OpenIds.end(), CurrentSession->Rec.Id) != State_.Ui.OpenIds.end();
        if (IsOpen == false)
        {
            UnopenedCount++;
        }
    }
    if (UnopenedCount == 0)
    {
        ShowToast("No unopened sessions in this project.");
        return;
    }
    if (State_.Ui.CloseWithoutConfirm == true)
    {
        CloseUnopenedSessions(ProjectId);
        return;
    }
    ConfirmTitle_ = "Close " + std::to_string(UnopenedCount) + " unopened session(s)?";
    ConfirmText_ = "This stops these sessions and moves them to the Archive. Sessions shown in a pane stay open.\nGit worktrees on disk are left untouched.";
    ConfirmYesAction_ = [this, ProjectId] { CloseUnopenedSessions(ProjectId); };
    OpenModal(Modal::Confirm);
}

void App::RemoveProject(const std::string& Id)
{
    std::vector<std::string> IdsToClose;
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Rec.ProjectId == Id)
        {
            IdsToClose.push_back(CurrentSession->Rec.Id);
        }
    }
    for (const std::string& SessionId : IdsToClose)
    {
        CloseSession(SessionId, 0);
    }
    State_.Projects.erase(
        std::remove_if(State_.Projects.begin(), State_.Projects.end(), [&](const Project& CurrentProject) { return CurrentProject.Id == Id; }),
        State_.Projects.end());
    MarkDirty();
}

bool App::AddProjectPath(const std::string& RawPath, std::string& Err, std::string* OutId)
{
    std::string Path = Util::Trim(RawPath);
    if (Path.size() >= 2 && Path.front() == '"' && Path.back() == '"')
    {
        Path = Path.substr(1, Path.size() - 2);
    }
    std::error_code ErrorCode;
    if (Path.empty() || fs::is_directory(Path, ErrorCode) == false)
    {
        Err = "That folder doesn't exist.";
        return false;
    }
    std::string Normalized = fs::path(Path).lexically_normal().make_preferred().string();
    while (Normalized.size() > 1 && (Normalized.back() == '/' || Normalized.back() == '\\'))
    {
        Normalized.pop_back();
    }
    for (const Project& CurrentProject : State_.Projects)
    {
        if (CurrentProject.Path == Normalized)
        {
            if (OutId != nullptr)
            {
                *OutId = CurrentProject.Id;
            }
            return true;
        }
    }
    Project const CurrentProject{Util::Uuid4(), Util::BaseName(Normalized), Normalized};
    State_.Projects.push_back(CurrentProject);
    if (OutId != nullptr)
    {
        *OutId = CurrentProject.Id;
    }
    MarkDirty();
    return true;
}

Session::StatusFn App::StatusCallback()
{
    return [this](Session& CurrentSession, Status OldStatus, Status NewStatus, double BusySeconds)
    { OnStatus(CurrentSession, OldStatus, NewStatus, BusySeconds); };
}

void App::OnStatus(Session& CurrentSession, Status, Status NewStatus, double BusySeconds)
{
    bool Visible = false;
    for (const std::string& Id : VisibleIds())
    {
        if (Id == CurrentSession.Rec.Id)
        {
            Visible = true;
        }
    }
    AutoOnStatus(CurrentSession, NewStatus, BusySeconds);
    bool const Watching = Focused_ && State_.Ui.ActiveId == CurrentSession.Rec.Id && Visible;
    bool const Waiting = NewStatus == Status::Waiting;
    // With background tasks still running, Claude comes back on its own when they report, so it isn't finished yet.
    bool const Done = NewStatus == Status::Idle && BusySeconds >= 4.0 && CurrentSession.BackgroundTasks == 0;
    if ((Waiting || Done) && Watching == false)
    {
        CurrentSession.Unseen = true;
        if (Focused_ == false)
        {
            Project* const CurrentProject = FindProject(CurrentSession.Rec.ProjectId);
            if (State_.AppSettings.Notifications)
            {
                Platform::Notify(Window_, (CurrentProject != nullptr ? CurrentProject->Name : std::string("Claude")) + " \xC2\xB7 " + CurrentSession.Rec.Name,
                                 Waiting ? "Waiting for your input or approval" : "Finished — ready for your next prompt");
            }
            Platform::Flash(Window_, true);
        }
    }
    if (NewStatus == Status::Busy || NewStatus == Status::Exited)
    {
        CurrentSession.Unseen = false;
    }
}

void App::SaveNow()
{
    State_.Sessions.clear();
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (ShuttingDown_ == false && CurrentSession->Attempted == true)
        {
            CurrentSession->Rec.WasRunning = CurrentSession->IsRunning();
        }
        State_.Sessions.push_back(CurrentSession->Rec);
    }
    Store_.Save();
    Dirty_ = false;
    LastSave_ = Now_;
}

void App::ShowToast(const std::string& Text, bool IsError)
{
    Toasts_.push_back({Text, Now_ + (IsError ? 9.0 : 6.0), IsError});
}

void App::Shutdown()
{
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Attempted == true)
        {
            CurrentSession->Rec.WasRunning = CurrentSession->IsRunning();
        }
    }
    ShuttingDown_ = true;
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        CurrentSession->Kill(Now_, nullptr);
    }
    SaveNow();
    Platform::Shutdown(Window_);
}

double App::FrameWaitSeconds() const
{
    constexpr double ANIMATION_WAIT_SECONDS = 1.0 / 30.0;
    constexpr double IDLE_WAIT_SECONDS = 0.1;
    if (Toasts_.empty() == false)
    {
        return ANIMATION_WAIT_SECONDS;
    }
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->CurrentStatus == Status::Busy || CurrentSession->InBackground() == true)
        {
            return ANIMATION_WAIT_SECONDS;
        }
    }
    return IDLE_WAIT_SECONDS;
}

// ---------------------------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------------------------

void App::TickSessions(double Now)
{
    Now_ = Now;
    Session::StatusFn const Callback = StatusCallback();
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Attempted == false && CurrentSession->Rec.WasRunning == true && IsSessionOpen(CurrentSession->Rec.Id) == false)
        {
            CurrentSession->Attempted = true;
            std::string Err;
            if (StartSession(*CurrentSession, BACKGROUND_COLS, BACKGROUND_ROWS, Err) == false)
            {
                ShowToast("Could not launch Claude: " + Err, true);
            }
            MarkDirty();
        }
        CurrentSession->Tick(Now_, Callback);
        if (CurrentSession->PendingPrompt.empty() == false && CurrentSession->CurrentStatus == Status::Idle)
        {
            const std::string Prompt = std::move(CurrentSession->PendingPrompt);
            CurrentSession->PendingPrompt.clear();
            SendPrompt(*CurrentSession, Prompt);
        }
        AutoContinueTick(*CurrentSession);
        PollScreenshots(*CurrentSession);
    }
}

void App::Frame(double Now)
{
    Now_ = Now;
    if (PendingCloseId_.empty() == false)
    {
        const std::string CloseId = PendingCloseId_;
        PendingCloseId_.clear();
        CloseSession(CloseId, 0);
    }
    SyncTheme();
    Images_->BeginFrame();
    bool const Focused = glfwGetWindowAttrib(Window_, GLFW_FOCUSED) != 0;
    if (Focused && Focused_ == false)
    {
        Platform::Flash(Window_, false);
        if (Session* const Active = ActiveSession())
        {
            Active->Unseen = false;
        }
    }
    Focused_ = Focused;

    TickSessions(Now);
    EnsureActiveVisible();

    constexpr double USAGE_REFRESH_INTERVAL_SECONDS = 300.0;
    bool UsageInFlight = false, UsageDone = false;
    UsageInfo CurrentUsage;
    ReadUsageFetch(UsageInFlight, UsageDone, CurrentUsage);
    if (UsageInFlight == false && Now_ - UsageLastFetchAt_ > USAGE_REFRESH_INTERVAL_SECONDS)
    {
        UsageLastFetchAt_ = Now_;
        StartUsageFetch();
    }

    constexpr double WINDOW_PING_RETRY_SECONDS = 120.0;
    bool const WindowIsFree = CurrentUsage.Ok == true &&
                              (CurrentUsage.SessionResetsAt.empty() == true || UiHelpers::SecondsUntil(CurrentUsage.SessionResetsAt) == 0);
    if (State_.AppSettings.AutoStartWindow == true && WindowIsFree == true && WindowPingBusy_->load() == false &&
        Now_ - WindowPingLastAt_ > WINDOW_PING_RETRY_SECONDS)
    {
        StartWindowPing();
        UsageLastFetchAt_ = Now_ - USAGE_REFRESH_INTERVAL_SECONDS + 20.0;
    }

    if (State_.AppSettings.GitHubEnabled == true)
    {
        const double GitHubInterval = Modal_ == Modal::GitHub ? 3.0 : 60.0;
        if (Now_ - GitHubLastCheckAt_ > GitHubInterval)
        {
            GitHubLastCheckAt_ = Now_;
            StartGitHubCheck();
        }
    }

    HandleEvents();
    DrawRoot();
    DrawProjectLog();
    DrawProjectSettings();
    DrawNightlyBuild();
    DrawModals();
    DrawToasts();

    if (Dirty_ && Now_ - LastSave_ > 0.5)
    {
        SaveNow();
    }
}

void App::DrawStatusDot(ImDrawList* DrawList, ImVec2 Center, float Radius, const Session& CurrentSession) const
{
    switch (CurrentSession.CurrentStatus)
    {
        case Status::Busy:
        {
            float const Pulse = 0.55f + 0.45f * std::sin((float)Now_ * 6.0f);
            DrawList->AddCircleFilled(Center, Radius * (0.8f + 0.2f * Pulse), UiHelpers::WithAlpha(BusyColor_, (int)(110 + 145 * Pulse)));
            break;
        }
        case Status::Starting: DrawList->AddCircleFilled(Center, Radius, UiHelpers::WithAlpha(BusyColor_, 150)); break;
        case Status::Waiting:
            DrawList->AddCircleFilled(Center, Radius + 3, UiHelpers::WithAlpha(WaitingColor_, 60));
            DrawList->AddCircleFilled(Center, Radius, WaitingColor_);
            break;
        case Status::Idle:
            if (CurrentSession.InBackground() == true)
            {
                // Slower pulse and a ring, so "working in the background" reads differently from "busy right now".
                float const Pulse = 0.5f + 0.5f * std::sin((float)Now_ * 2.5f);
                DrawList->AddCircle(Center, Radius + 1.5f, UiHelpers::WithAlpha(BusyColor_, (int)(90 + 165 * Pulse)), 0, 1.5f);
                DrawList->AddCircleFilled(Center, Radius * 0.55f, UiHelpers::WithAlpha(BusyColor_, 200));
            }
            else if (CurrentSession.Unseen)
            {
                DrawList->AddCircleFilled(Center, Radius + 3, UiHelpers::WithAlpha(DoneColor_, 60));
                DrawList->AddCircleFilled(Center, Radius, DoneColor_);
            }
            else
            {
                DrawList->AddCircleFilled(Center, Radius, UiHelpers::WithAlpha(DoneColor_, 130));
            }
            break;
        case Status::Exited: DrawList->AddCircle(Center, Radius - 0.5f, IdleColor_, 0, 1.5f); break;
    }
}

void App::DrawRoot()
{
    ImGuiViewport* const Viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(Viewport->WorkPos);
    ImGui::SetNextWindowSize(Viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, BackgroundColorV4_);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    LayoutDebug_.Sample("root.width", ImGui::GetWindowWidth());
    LayoutDebug_.Sample("root.height", ImGui::GetWindowHeight());
    LayoutDebug_.Sample("root.scrollMaxY", ImGui::GetScrollMaxY());

    if (State_.Ui.SidebarCollapsed == true)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, SurfaceColorV4_);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * UiScale_, 10 * UiScale_));
        ImGui::BeginChild("##sideCollapsed", ImVec2(28.0f * UiScale_, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::Button(">", ImVec2(-1, 0)) == true)
        {
            State_.Ui.SidebarCollapsed = false;
            MarkDirty();
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Expand sidebar");
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 0);
        DrawMainAndPanel();
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }

    float const SidebarWidth = std::clamp(State_.Ui.SidebarWidth, 200.0f * UiScale_, 520.0f * UiScale_);
    DrawSidebar(SidebarWidth);
    ImGui::SameLine(0, 0);

    ImGui::PushStyleColor(ImGuiCol_Button, PaneBorderColorV4_);
    ImGui::InvisibleButton("##splitter", ImVec2(5.0f * UiScale_, -1));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActive())
    {
        State_.Ui.SidebarWidth = std::clamp(SidebarWidth + ImGui::GetIO().MouseDelta.x, 200.0f * UiScale_, 520.0f * UiScale_);
        MarkDirty();
    }
    ImVec2 SplitterMin = ImGui::GetItemRectMin(), SplitterMax = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(SplitterMin, SplitterMax, ImGui::IsItemActive() ? AccentColor_ : PaneBorderColorU32_);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 0);

    DrawMainAndPanel();
    ImGui::End();
    ImGui::PopStyleColor();
}
