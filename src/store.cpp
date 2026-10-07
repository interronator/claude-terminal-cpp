#include "store.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using nlohmann::json;

std::string Store::defaultPath() {
  if (const char* o = std::getenv("CLAUDE_TERMINAL_DATA")) return (fs::path(o) / "state.json").string();
#ifdef _WIN32
  const char* base = std::getenv("APPDATA");
  fs::path dir = base ? fs::path(base) : fs::path(".");
  return (dir / "ClaudeTerminal" / "state.json").string();
#else
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  const char* home = std::getenv("HOME");
  fs::path dir = xdg ? fs::path(xdg) : (home ? fs::path(home) / ".config" : fs::path("."));
  return (dir / "claude-terminal" / "state.json").string();
#endif
}

Store::Store(std::string path) : path_(std::move(path)) {}

void Store::load() {
  std::ifstream in(path_);
  if (!in) return;
  json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) return;

  State s;
  for (auto& p : j.value("projects", json::array()))
    s.projects.push_back({p.value("id", ""), p.value("name", ""), p.value("path", "")});

  for (auto& e : j.value("sessions", json::array())) {
    SessionRec r;
    r.id = e.value("id", "");
    r.projectId = e.value("projectId", "");
    r.name = e.value("name", "");
    r.cwd = e.value("cwd", "");
    r.claudeSessionId = e.value("claudeSessionId", "");
    r.started = e.value("started", false);
    r.createdAt = e.value("createdAt", 0LL);
    if (e.contains("worktree") && e["worktree"].is_object()) {
      auto& w = e["worktree"];
      r.worktree = Worktree{w.value("repoRoot", ""), w.value("branch", ""), w.value("path", "")};
    }
    if (!r.id.empty()) s.sessions.push_back(std::move(r));
  }

  if (j.contains("ui") && j["ui"].is_object()) {
    auto& u = j["ui"];
    s.ui.openIds = u.value("openIds", std::vector<std::string>{});
    s.ui.activeId = u.value("activeId", "");
    s.ui.mode = u.value("mode", "tile");
    s.ui.sidebarWidth = u.value("sidebarWidth", 280.0f);
    s.ui.collapsed = u.value("collapsed", std::map<std::string, bool>{});
  }
  if (j.contains("settings") && j["settings"].is_object()) {
    auto& t = j["settings"];
    s.settings.claudeCommand = t.value("claudeCommand", "claude");
    s.settings.notifications = t.value("notifications", true);
    s.settings.fontSize = t.value("fontSize", 16);
    if (t.contains("theme") && t["theme"].is_object()) {
      auto& th = t["theme"];
      const ThemeColors d;  // defaults
      auto readColor = [&](const char* key, const ThemeColor& def) {
        if (!th.contains(key) || !th[key].is_object()) return def;
        auto& c = th[key];
        return ThemeColor{c.value("r", def.r), c.value("g", def.g), c.value("b", def.b), c.value("a", def.a)};
      };
      s.settings.theme.background = readColor("background", d.background);
      s.settings.theme.surface = readColor("surface", d.surface);
      s.settings.theme.paneBg = readColor("paneBg", d.paneBg);
      s.settings.theme.paneBorder = readColor("paneBorder", d.paneBorder);
      s.settings.theme.accent = readColor("accent", d.accent);
      s.settings.theme.text = readColor("text", d.text);
      s.settings.theme.textDisabled = readColor("textDisabled", d.textDisabled);
      s.settings.theme.statusBusy = readColor("statusBusy", d.statusBusy);
      s.settings.theme.statusWaiting = readColor("statusWaiting", d.statusWaiting);
      s.settings.theme.statusDone = readColor("statusDone", d.statusDone);
      s.settings.theme.statusIdle = readColor("statusIdle", d.statusIdle);
      s.settings.theme.terminalBg = readColor("terminalBg", d.terminalBg);
      s.settings.theme.selection = readColor("selection", d.selection);
      s.settings.theme.cursor = readColor("cursor", d.cursor);
      s.settings.theme.titlebarBg = readColor("titlebarBg", d.titlebarBg);
      s.settings.theme.titlebarText = readColor("titlebarText", d.titlebarText);
    }
  }
  state = std::move(s);
}

bool Store::save() const {
  json j;
  j["projects"] = json::array();
  for (auto& p : state.projects) j["projects"].push_back({{"id", p.id}, {"name", p.name}, {"path", p.path}});
  j["sessions"] = json::array();
  for (auto& r : state.sessions) {
    json e = {{"id", r.id},           {"projectId", r.projectId},         {"name", r.name},
              {"cwd", r.cwd},         {"claudeSessionId", r.claudeSessionId},
              {"started", r.started}, {"createdAt", r.createdAt}};
    if (r.worktree)
      e["worktree"] = {{"repoRoot", r.worktree->repoRoot}, {"branch", r.worktree->branch}, {"path", r.worktree->path}};
    j["sessions"].push_back(e);
  }
  j["ui"] = {{"openIds", state.ui.openIds},
             {"activeId", state.ui.activeId},
             {"mode", state.ui.mode},
             {"sidebarWidth", state.ui.sidebarWidth},
             {"collapsed", state.ui.collapsed}};
  auto colorJson = [](const ThemeColor& c) { return json{{"r", c.r}, {"g", c.g}, {"b", c.b}, {"a", c.a}}; };
  const ThemeColors& th = state.settings.theme;
  j["settings"] = {{"claudeCommand", state.settings.claudeCommand},
                   {"notifications", state.settings.notifications},
                   {"fontSize", state.settings.fontSize},
                   {"theme",
                    {{"background", colorJson(th.background)},
                     {"surface", colorJson(th.surface)},
                     {"paneBg", colorJson(th.paneBg)},
                     {"paneBorder", colorJson(th.paneBorder)},
                     {"accent", colorJson(th.accent)},
                     {"text", colorJson(th.text)},
                     {"textDisabled", colorJson(th.textDisabled)},
                     {"statusBusy", colorJson(th.statusBusy)},
                     {"statusWaiting", colorJson(th.statusWaiting)},
                     {"statusDone", colorJson(th.statusDone)},
                     {"statusIdle", colorJson(th.statusIdle)},
                     {"terminalBg", colorJson(th.terminalBg)},
                     {"selection", colorJson(th.selection)},
                     {"cursor", colorJson(th.cursor)},
                     {"titlebarBg", colorJson(th.titlebarBg)},
                     {"titlebarText", colorJson(th.titlebarText)}}}};

  std::error_code ec;
  fs::create_directories(fs::path(path_).parent_path(), ec);
  std::string tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << j.dump(2);
  }
  fs::rename(tmp, path_, ec);
  return !ec;
}
