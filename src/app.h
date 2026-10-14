#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "file_index.h"
#include "git.h"
#include "git_review.h"
#include "github_login.h"
#include "imgui.h"
#include "screenshot_feed.h"
#include "session.h"
#include "store.h"
#include "transcript_scanner.h"
#include "usage.h"

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
    void RequestDeleteAllArchived();
    void BuildAndRun(Session& CurrentSession);
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
    void DrawMain(float Width);
    void DrawMainAndPanel();
    void DrawScreenshotPanel(const float Width);
    bool DrawScreenshotCard(const ScreenshotFeed::Entry& Item, const float Width);
    void DrawToolbar();
    void DrawPane(Session& CurrentSession, ImVec2 Pos, ImVec2 Size, int Index);
    void DrawTerminal(Session& CurrentSession, ImVec2 Size, bool IsActive);
    void DrawFindBar(Session& Target, ImVec2 PanelPos, ImVec2 PanelSize);
    void RunFind(Session& Target, bool Forward, bool KeepCurrent);
    void DrawModals();
    void DrawToasts();
    void OpenModal(Modal CurrentModal) { Modal_ = CurrentModal; ModalJustOpened_ = true; }
    void DrawStatusDot(ImDrawList* DrawList, ImVec2 Center, float Radius, const Session& CurrentSession) const;
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

    ScreenshotFeed ScreenshotFeed_;
    std::string ProjectScopeId_;
    std::map<std::string, float> ShotAspects_;
    std::unique_ptr<ImageCache> Images_;
    std::map<std::string, TranscriptScanner> TranscriptScanners_;
    std::map<std::string, double> ScreenshotPollAt_;
    int UnseenScreenshots_ = 0;

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
