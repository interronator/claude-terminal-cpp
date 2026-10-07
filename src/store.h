#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

struct Worktree {
  std::string repoRoot, branch, path;
};

struct SessionRec {
  std::string id, projectId, name, cwd, claudeSessionId;
  bool started = false;
  std::optional<Worktree> worktree;
  long long createdAt = 0;
};

struct Project {
  std::string id, name, path;
};

struct ThemeColor {
  float r = 0, g = 0, b = 0, a = 1;
};

struct ThemeColors {
  ThemeColor background{0.059f, 0.067f, 0.082f, 1};
  ThemeColor surface{0.082f, 0.094f, 0.125f, 1};  // sidebar/toolbar/pane-header bg
  ThemeColor paneBg{0.043f, 0.051f, 0.067f, 1};
  ThemeColor paneBorder{0.15f, 0.17f, 0.21f, 1};
  ThemeColor accent{0.851f, 0.467f, 0.341f, 1};
  ThemeColor text{0.90f, 0.91f, 0.93f, 1};
  ThemeColor textDisabled{0.54f, 0.57f, 0.64f, 1};
  ThemeColor statusBusy{0.902f, 0.635f, 0.235f, 1};
  ThemeColor statusWaiting{0.937f, 0.357f, 0.357f, 1};
  ThemeColor statusDone{0.298f, 0.765f, 0.541f, 1};
  ThemeColor statusIdle{0.357f, 0.396f, 0.467f, 1};
  ThemeColor terminalBg{0.043f, 0.051f, 0.067f, 1};
  ThemeColor selection{0.851f, 0.467f, 0.341f, 0.43f};
  ThemeColor cursor{0.851f, 0.467f, 0.341f, 0.67f};
  ThemeColor titlebarBg{0.082f, 0.094f, 0.125f, 1};
  ThemeColor titlebarText{0.90f, 0.91f, 0.93f, 1};
};

struct Settings {
  std::string claudeCommand = "claude";
  bool notifications = true;
  int fontSize = 16;  // terminal font, in points at 100% scale
  ThemeColors theme;
};

struct UiState {
  std::vector<std::string> openIds;
  std::string activeId;
  std::string mode = "tile";  // "tile" | "tabs"
  float sidebarWidth = 280;
  std::map<std::string, bool> collapsed;
};

struct State {
  std::vector<Project> projects;
  std::vector<SessionRec> sessions;
  UiState ui;
  Settings settings;
};

class Store {
 public:
  explicit Store(std::string path);
  static std::string defaultPath();
  State state;
  void load();
  bool save() const;  // atomic write

 private:
  std::string path_;
};
