#pragma once
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "keybindings.h"

struct Worktree
{
    std::string RepoRoot;
    std::string Branch;
    std::string Path;
};

struct SessionRec
{
    std::string Id;
    std::string ProjectId;
    std::string Name;
    std::string Cwd;
    std::string ClaudeSessionId;
    bool Started = false;
    bool WasRunning = false;
    std::optional<Worktree> WorktreeInfo;
    long long CreatedAt = 0;
};

struct ArchivedSession
{
    std::string Id;
    std::string Name;
    std::string ProjectName;
    SessionRec Rec;  // empty ClaudeSessionId means a legacy entry that cannot be restored
};

struct Project
{
    std::string Id;
    std::string Name;
    std::string Path;
};

struct ThemeColor
{
    float Red = 0, Green = 0, Blue = 0, Alpha = 1;
};

struct ThemeColors
{
    ThemeColor Background{0.059f, 0.067f, 0.082f, 1};
    ThemeColor Surface{0.082f, 0.094f, 0.125f, 1};  // sidebar/toolbar/pane-header bg
    ThemeColor PaneBg{0.043f, 0.051f, 0.067f, 1};
    ThemeColor PaneBorder{0.15f, 0.17f, 0.21f, 1};
    ThemeColor Accent{0.851f, 0.467f, 0.341f, 1};
    ThemeColor Text{0.90f, 0.91f, 0.93f, 1};
    ThemeColor TextDisabled{0.54f, 0.57f, 0.64f, 1};
    ThemeColor StatusBusy{0.902f, 0.635f, 0.235f, 1};
    ThemeColor StatusWaiting{0.937f, 0.357f, 0.357f, 1};
    ThemeColor StatusDone{0.298f, 0.765f, 0.541f, 1};
    ThemeColor StatusIdle{0.357f, 0.396f, 0.467f, 1};
    ThemeColor UsageWarning{1.0f, 0.596f, 0.0f, 1};
    ThemeColor UsageFull{0.898f, 0.224f, 0.208f, 1};
    ThemeColor TerminalBg{0.043f, 0.051f, 0.067f, 1};
    ThemeColor Selection{0.851f, 0.467f, 0.341f, 0.43f};
    ThemeColor Cursor{0.851f, 0.467f, 0.341f, 0.67f};
    ThemeColor TitlebarBg{0.082f, 0.094f, 0.125f, 1};
    ThemeColor TitlebarText{0.90f, 0.91f, 0.93f, 1};

    static ThemeColors Dark() { return ThemeColors{}; }

    // Light chrome around a dark terminal: the terminal's ANSI palette is tuned for dark backgrounds.
    static ThemeColors Light()
    {
        ThemeColors Result;
        Result.Background = {0.91f, 0.92f, 0.94f, 1};
        Result.Surface = {0.96f, 0.97f, 0.98f, 1};
        Result.PaneBg = {1.0f, 1.0f, 1.0f, 1};
        Result.PaneBorder = {0.78f, 0.80f, 0.85f, 1};
        Result.Accent = {0.80f, 0.38f, 0.24f, 1};
        Result.Text = {0.12f, 0.14f, 0.18f, 1};
        Result.TextDisabled = {0.42f, 0.45f, 0.52f, 1};
        Result.StatusBusy = {0.85f, 0.52f, 0.08f, 1};
        Result.StatusWaiting = {0.85f, 0.22f, 0.22f, 1};
        Result.StatusDone = {0.13f, 0.58f, 0.36f, 1};
        Result.StatusIdle = {0.55f, 0.58f, 0.65f, 1};
        Result.Selection = {0.80f, 0.38f, 0.24f, 0.45f};
        Result.Cursor = {0.80f, 0.38f, 0.24f, 0.75f};
        Result.TitlebarBg = {0.96f, 0.97f, 0.98f, 1};
        Result.TitlebarText = {0.12f, 0.14f, 0.18f, 1};
        return Result;
    }

    static ThemeColors Midnight()
    {
        ThemeColors Result;
        Result.Background = {0.015f, 0.015f, 0.025f, 1};
        Result.Surface = {0.045f, 0.047f, 0.065f, 1};
        Result.PaneBg = {0.0f, 0.0f, 0.0f, 1};
        Result.PaneBorder = {0.14f, 0.15f, 0.20f, 1};
        Result.Accent = {0.42f, 0.60f, 1.0f, 1};
        Result.Text = {0.92f, 0.93f, 0.96f, 1};
        Result.TextDisabled = {0.50f, 0.53f, 0.62f, 1};
        Result.TerminalBg = {0.0f, 0.0f, 0.0f, 1};
        Result.Selection = {0.42f, 0.60f, 1.0f, 0.40f};
        Result.Cursor = {0.42f, 0.60f, 1.0f, 0.70f};
        Result.TitlebarBg = {0.045f, 0.047f, 0.065f, 1};
        Result.TitlebarText = {0.92f, 0.93f, 0.96f, 1};
        return Result;
    }
};

struct Settings
{
    std::string ClaudeCommand = "claude";
    bool Notifications = true;
    bool GitHubEnabled = false;
    bool AutoStartWindow = false;
    int FontSize = 16;  // terminal font, in points at 100% scale
    int PaneTitleScale = 100;  // session title text size in the pane header, percent of the normal UI text
    ThemeColors Theme;
    KeyBindings Keys;
};

struct UiState
{
    std::vector<std::string> OpenIds;
    std::string ActiveId;
    std::string Mode = "tile";  // "tile" | "tabs"
    float SidebarWidth = 280;
    bool SidebarCollapsed = false;
    float ScreenshotPanelWidth = 340;
    bool ScreenshotPanelOpen = true;
    std::set<std::string> HiddenScreenshots;  // file paths the user dismissed from the feed
    bool CloseWithoutConfirm = false;
    bool ArchiveOpen = false;
    std::map<std::string, bool> Collapsed;
    std::string TabFilterProjectId;    // empty = no project filter; else = Project::Id the tab bar is narrowed to
    std::string ActionFilter = "all";  // "all" | "running" | "action"
    bool FocusProjectOnClick = false;  // clicking a session in the sidebar narrows the panes to its project
};

struct State
{
    std::vector<Project> Projects;
    std::vector<SessionRec> Sessions;
    std::vector<ArchivedSession> Archive;
    UiState Ui;
    Settings AppSettings;
};

class Store
{
public:
    explicit Store(std::string Path);
    static std::string DefaultPath();
    State AppState;
    void Load();
    bool Save() const;  // atomic write

private:
    static State ParseState(const nlohmann::json& Root);
    static SessionRec ReadSessionRec(const nlohmann::json& SessionJson);
    static nlohmann::json SessionRecToJson(const SessionRec& Rec);
    std::string Path_;
};
