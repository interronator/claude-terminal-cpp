#include "Store.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using nlohmann::json;

std::string Store::DefaultPath()
{
    if (const char* const OverridePath = std::getenv("CLAUDE_TERMINAL_DATA"))
    {
        return (fs::path(OverridePath) / "state.json").string();
    }
#ifdef _WIN32
    const char* const Base = std::getenv("APPDATA");
    fs::path const Dir = Base != nullptr ? fs::path(Base) : fs::path(".");
    return (Dir / "ClaudeTerminal" / "state.json").string();
#else
    const char* xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    fs::path dir = xdgConfigHome != nullptr ? fs::path(xdgConfigHome)
                                             : (home != nullptr ? fs::path(home) / ".config" : fs::path("."));
    return (dir / "claude-terminal" / "state.json").string();
#endif
}

Store::Store(std::string Path) : Path_(std::move(Path)) {}

void Store::Load()
{
    std::ifstream In(Path_);
    if (In.good() == false)
    {
        return;
    }
    json const Root = json::parse(In, nullptr, /*allow_exceptions=*/false);
    if (Root.is_discarded() == true || Root.is_object() == false)
    {
        return;
    }

    try
    {
        AppState = ParseState(Root);
    }
    catch (const json::exception&)
    {
    }
}

State Store::ParseState(const json& Root)
{
    State Loaded;
    for (const json& ProjectJson : Root.value("projects", json::array()))
    {
        Loaded.Projects.push_back({ProjectJson.value("id", ""), ProjectJson.value("name", ""), ProjectJson.value("path", "")});
    }

    for (const json& SessionJson : Root.value("sessions", json::array()))
    {
        SessionRec Rec = ReadSessionRec(SessionJson);
        if (Rec.Id.empty() == false)
        {
            Loaded.Sessions.push_back(std::move(Rec));
        }
    }

    for (const json& ArchiveJson : Root.value("archive", json::array()))
    {
        ArchivedSession Archived{ArchiveJson.value("id", ""), ArchiveJson.value("name", ""), ArchiveJson.value("projectName", ""), {}};
        if (ArchiveJson.contains("session") == true && ArchiveJson["session"].is_object() == true)
        {
            Archived.Rec = ReadSessionRec(ArchiveJson["session"]);
        }
        Loaded.Archive.push_back(std::move(Archived));
    }

    if (Root.contains("ui") && Root["ui"].is_object())
    {
        const json& UiJson = Root["ui"];
        Loaded.Ui.OpenIds = UiJson.value("openIds", std::vector<std::string>{});
        Loaded.Ui.ActiveId = UiJson.value("activeId", "");
        Loaded.Ui.Mode = UiJson.value("mode", "tile");
        Loaded.Ui.SidebarWidth = UiJson.value("sidebarWidth", 280.0f);
        Loaded.Ui.SidebarCollapsed = UiJson.value("sidebarCollapsed", false);
        Loaded.Ui.ScreenshotPanelWidth = UiJson.value("screenshotPanelWidth", 340.0f);
        Loaded.Ui.ScreenshotPanelOpen = UiJson.value("screenshotPanelOpen", true);
        Loaded.Ui.HiddenScreenshots = UiJson.value("hiddenScreenshots", std::set<std::string>{});
        Loaded.Ui.CloseWithoutConfirm = UiJson.value("closeWithoutConfirm", false);
        Loaded.Ui.ArchiveOpen = UiJson.value("archiveOpen", false);
        Loaded.Ui.Collapsed = UiJson.value("collapsed", std::map<std::string, bool>{});
        Loaded.Ui.TabFilterProjectId = UiJson.value("tabFilterProjectId", std::string());
        Loaded.Ui.ActionFilter = UiJson.value("actionFilter", std::string("all"));
        Loaded.Ui.FocusProjectOnClick = UiJson.value("focusProjectOnClick", false);
    }
    if (Root.contains("settings") && Root["settings"].is_object())
    {
        const json& SettingsJson = Root["settings"];
        Loaded.AppSettings.ClaudeCommand = SettingsJson.value("claudeCommand", "claude");
        Loaded.AppSettings.Notifications = SettingsJson.value("notifications", true);
        Loaded.AppSettings.AutoStartWindow = SettingsJson.value("autoStartWindow", false);
        Loaded.AppSettings.GitHubEnabled = SettingsJson.value("githubEnabled", false);
        Loaded.AppSettings.FontSize = SettingsJson.value("fontSize", 16);
        Loaded.AppSettings.PaneTitleScale = std::clamp(SettingsJson.value("paneTitleScale", 100), 70, 200);
        if (SettingsJson.contains("theme") && SettingsJson["theme"].is_object())
        {
            const json& ThemeJson = SettingsJson["theme"];
            const ThemeColors Defaults;
            const std::function<ThemeColor(const char*, const ThemeColor&)> ReadColor = [&](const char* Key, const ThemeColor& Fallback)
            {
                if (ThemeJson.contains(Key) == false || ThemeJson[Key].is_object() == false)
                {
                    return Fallback;
                }
                const json& ColorJson = ThemeJson[Key];
                return ThemeColor{ColorJson.value("r", Fallback.Red), ColorJson.value("g", Fallback.Green),
                                   ColorJson.value("b", Fallback.Blue), ColorJson.value("a", Fallback.Alpha)};
            };
            Loaded.AppSettings.Theme.Background = ReadColor("background", Defaults.Background);
            Loaded.AppSettings.Theme.Surface = ReadColor("surface", Defaults.Surface);
            Loaded.AppSettings.Theme.PaneBg = ReadColor("paneBg", Defaults.PaneBg);
            Loaded.AppSettings.Theme.PaneBorder = ReadColor("paneBorder", Defaults.PaneBorder);
            Loaded.AppSettings.Theme.Accent = ReadColor("accent", Defaults.Accent);
            Loaded.AppSettings.Theme.Text = ReadColor("text", Defaults.Text);
            Loaded.AppSettings.Theme.TextDisabled = ReadColor("textDisabled", Defaults.TextDisabled);
            Loaded.AppSettings.Theme.StatusBusy = ReadColor("statusBusy", Defaults.StatusBusy);
            Loaded.AppSettings.Theme.StatusWaiting = ReadColor("statusWaiting", Defaults.StatusWaiting);
            Loaded.AppSettings.Theme.StatusDone = ReadColor("statusDone", Defaults.StatusDone);
            Loaded.AppSettings.Theme.StatusIdle = ReadColor("statusIdle", Defaults.StatusIdle);
            Loaded.AppSettings.Theme.UsageWarning = ReadColor("usageWarning", Defaults.UsageWarning);
            Loaded.AppSettings.Theme.UsageFull = ReadColor("usageFull", Defaults.UsageFull);
            Loaded.AppSettings.Theme.TerminalBg = ReadColor("terminalBg", Defaults.TerminalBg);
            Loaded.AppSettings.Theme.Selection = ReadColor("selection", Defaults.Selection);
            Loaded.AppSettings.Theme.Cursor = ReadColor("cursor", Defaults.Cursor);
            Loaded.AppSettings.Theme.TitlebarBg = ReadColor("titlebarBg", Defaults.TitlebarBg);
            Loaded.AppSettings.Theme.TitlebarText = ReadColor("titlebarText", Defaults.TitlebarText);
        }
        if (SettingsJson.contains("keybindings") && SettingsJson["keybindings"].is_object())
        {
            const json& KeysJson = SettingsJson["keybindings"];
            for (int Index = 0; Index < KeyBindings::ACTION_COUNT; Index++)
            {
                const KeyAction Action = KeyBindings::ActionAt(Index);
                const char* const Id = KeyBindings::ActionId(Action);
                if (KeysJson.contains(Id) == false || KeysJson[Id].is_object() == false)
                {
                    continue;
                }
                const json& ChordJson = KeysJson[Id];
                Loaded.AppSettings.Keys.Set(Action, {ChordJson.value("key", 0), ChordJson.value("mods", 0) & KeyBindings::MOD_MASK});
            }
        }
    }
    return Loaded;
}

SessionRec Store::ReadSessionRec(const json& SessionJson)
{
    SessionRec Rec;
    Rec.Id = SessionJson.value("id", "");
    Rec.ProjectId = SessionJson.value("projectId", "");
    Rec.Name = SessionJson.value("name", "");
    Rec.Cwd = SessionJson.value("cwd", "");
    Rec.ClaudeSessionId = SessionJson.value("claudeSessionId", "");
    Rec.Started = SessionJson.value("started", false);
    Rec.WasRunning = SessionJson.value("wasRunning", false);
    Rec.CreatedAt = SessionJson.value("createdAt", 0LL);
    if (SessionJson.contains("worktree") == true && SessionJson["worktree"].is_object() == true)
    {
        const json& WorktreeJson = SessionJson["worktree"];
        Rec.WorktreeInfo = Worktree{WorktreeJson.value("repoRoot", ""), WorktreeJson.value("branch", ""), WorktreeJson.value("path", "")};
    }
    return Rec;
}

json Store::SessionRecToJson(const SessionRec& Rec)
{
    json SessionJson = {{"id", Rec.Id},
                        {"projectId", Rec.ProjectId},
                        {"name", Rec.Name},
                        {"cwd", Rec.Cwd},
                        {"claudeSessionId", Rec.ClaudeSessionId},
                        {"started", Rec.Started},
                        {"wasRunning", Rec.WasRunning},
                        {"createdAt", Rec.CreatedAt}};
    if (Rec.WorktreeInfo)
    {
        SessionJson["worktree"] = {
            {"repoRoot", Rec.WorktreeInfo->RepoRoot}, {"branch", Rec.WorktreeInfo->Branch}, {"path", Rec.WorktreeInfo->Path}};
    }
    return SessionJson;
}

bool Store::Save() const
{
    json Root;
    json KeysJson = json::object();
    for (int Index = 0; Index < KeyBindings::ACTION_COUNT; Index++)
    {
        const KeyAction Action = KeyBindings::ActionAt(Index);
        const KeyChord& Chord = AppState.AppSettings.Keys.Get(Action);
        KeysJson[KeyBindings::ActionId(Action)] = {{"key", Chord.Key}, {"mods", Chord.Mods}};
    }
    Root["projects"] = json::array();
    for (const Project& CurrentProject : AppState.Projects)
    {
        Root["projects"].push_back({{"id", CurrentProject.Id}, {"name", CurrentProject.Name}, {"path", CurrentProject.Path}});
    }
    Root["sessions"] = json::array();
    for (const SessionRec& Rec : AppState.Sessions)
    {
        Root["sessions"].push_back(SessionRecToJson(Rec));
    }
    Root["archive"] = json::array();
    for (const ArchivedSession& Archived : AppState.Archive)
    {
        Root["archive"].push_back(
            {{"id", Archived.Id}, {"name", Archived.Name}, {"projectName", Archived.ProjectName}, {"session", SessionRecToJson(Archived.Rec)}});
    }
    Root["ui"] = {{"openIds", AppState.Ui.OpenIds},
                  {"activeId", AppState.Ui.ActiveId},
                  {"mode", AppState.Ui.Mode},
                  {"sidebarWidth", AppState.Ui.SidebarWidth},
                  {"sidebarCollapsed", AppState.Ui.SidebarCollapsed},
                  {"screenshotPanelWidth", AppState.Ui.ScreenshotPanelWidth},
                  {"screenshotPanelOpen", AppState.Ui.ScreenshotPanelOpen},
                  {"hiddenScreenshots", AppState.Ui.HiddenScreenshots},
                  {"closeWithoutConfirm", AppState.Ui.CloseWithoutConfirm},
                  {"archiveOpen", AppState.Ui.ArchiveOpen},
                  {"collapsed", AppState.Ui.Collapsed},
                  {"tabFilterProjectId", AppState.Ui.TabFilterProjectId},
                  {"actionFilter", AppState.Ui.ActionFilter},
                  {"focusProjectOnClick", AppState.Ui.FocusProjectOnClick}};
    const std::function<json(const ThemeColor&)> ColorJson = [](const ThemeColor& Color)
    { return json{{"r", Color.Red}, {"g", Color.Green}, {"b", Color.Blue}, {"a", Color.Alpha}}; };
    const ThemeColors& Theme = AppState.AppSettings.Theme;
    Root["settings"] = {{"claudeCommand", AppState.AppSettings.ClaudeCommand},
                        {"notifications", AppState.AppSettings.Notifications},
                        {"autoStartWindow", AppState.AppSettings.AutoStartWindow},
                        {"githubEnabled", AppState.AppSettings.GitHubEnabled},
                        {"fontSize", AppState.AppSettings.FontSize},
                        {"paneTitleScale", AppState.AppSettings.PaneTitleScale},
                        {"keybindings", KeysJson},
                        {"theme",
                         {{"background", ColorJson(Theme.Background)},
                          {"surface", ColorJson(Theme.Surface)},
                          {"paneBg", ColorJson(Theme.PaneBg)},
                          {"paneBorder", ColorJson(Theme.PaneBorder)},
                          {"accent", ColorJson(Theme.Accent)},
                          {"text", ColorJson(Theme.Text)},
                          {"textDisabled", ColorJson(Theme.TextDisabled)},
                          {"statusBusy", ColorJson(Theme.StatusBusy)},
                          {"statusWaiting", ColorJson(Theme.StatusWaiting)},
                          {"statusDone", ColorJson(Theme.StatusDone)},
                          {"statusIdle", ColorJson(Theme.StatusIdle)},
                          {"usageWarning", ColorJson(Theme.UsageWarning)},
                          {"usageFull", ColorJson(Theme.UsageFull)},
                          {"terminalBg", ColorJson(Theme.TerminalBg)},
                          {"selection", ColorJson(Theme.Selection)},
                          {"cursor", ColorJson(Theme.Cursor)},
                          {"titlebarBg", ColorJson(Theme.TitlebarBg)},
                          {"titlebarText", ColorJson(Theme.TitlebarText)}}}};

    std::error_code ErrorCode;
    fs::create_directories(fs::path(Path_).parent_path(), ErrorCode);
    std::string const TempPath = Path_ + ".tmp";
    {
        std::ofstream Out(TempPath, std::ios::trunc);
        if (Out.good() == false)
        {
            return false;
        }
        Out << Root.dump(2);
    }
    fs::rename(TempPath, Path_, ErrorCode);
    return ErrorCode.operator bool() == false;
}
