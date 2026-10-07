#pragma once
#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "session.h"
#include "store.h"
#include "usage.h"

struct GLFWwindow;

struct InputEvent {
  enum Type { Char, Key } type = Char;
  uint32_t cp = 0;
  int key = 0, mods = 0, action = 0;
};

class App {
 public:
  App(GLFWwindow* win, Store& store, float uiScale);
  ~App();

  // Raw input from GLFW callbacks (routed to the terminal during frame()).
  void pushChar(uint32_t cp);
  void pushKey(int key, int action, int mods);
  void pushDrop(const std::vector<std::string>& paths);

  // Call before ImGui::NewFrame(); returns true when the font atlas changed (re-upload the texture).
  bool rebuildFontsIfNeeded();
  void frame(double now);
  bool wantsFastFrames() const;
  void shutdown();

 private:
  enum class Modal { None, AddProject, NewSession, Rename, Confirm, CloseSession, Settings, Usage };

  // ---- model ----
  Session* find(const std::string& id) const;
  Project* findProject(const std::string& id);
  Session* active() const;
  std::vector<std::string> visibleIds() const;
  void openSession(const std::string& id);
  void hideSession(const std::string& id);
  void focusSession(const std::string& id);
  void toggleMaximize(const std::string& id);
  Session* createSession(const std::string& projectId, const std::string& name, bool worktree, std::string& err);
  void closeSession(const std::string& id, int worktreeAction);
  void removeProject(const std::string& id);
  bool addProjectPath(const std::string& path, std::string& err, std::string* outId = nullptr);
  void onStatus(Session& s, Status oldS, Status newS, double busy);
  void markDirty() { dirty_ = true; }
  // Swaps two sessions' positions in the open-tile order (drag-and-drop pane reordering).
  void swapSessionOrder(const std::string& a, const std::string& b)
  {
    if (a == b)
    {
      return;
    }
    auto ai = std::find(s_.ui.openIds.begin(), s_.ui.openIds.end(), a);
    auto bi = std::find(s_.ui.openIds.begin(), s_.ui.openIds.end(), b);
    if (ai == s_.ui.openIds.end() || bi == s_.ui.openIds.end())
    {
      return;
    }
    std::swap(*ai, *bi);
    markDirty();
  }
  void saveNow();
  void toast(const std::string& text, bool err = false);
  Session::StatusFn statusCb();

  // ---- input ----
  void handleEvents();
  void handleKey(const InputEvent& e);
  void copySelection(Session& s);
  void paste(Session& s);
  void changeFontSize(int delta);

  // ---- ui ----
  void drawRoot();
  void drawSidebar(float width);
  void drawMain();
  void drawToolbar();
  void drawPane(Session& s, ImVec2 pos, ImVec2 size, int index);
  void drawTerminal(Session& s, ImVec2 size, bool isActive);
  void drawModals();
  void drawToasts();
  void openModal(Modal m) { modal_ = m; modalJustOpened_ = true; }
  void statusDot(ImDrawList* dl, ImVec2 center, float r, const Session& s) const;
  bool drawBoxChar(ImDrawList* dl, float x, float y, float w, float h, uint32_t cp, ImU32 col);
  void syncTheme();
  void startUsageFetch();

  // ---- state ----
  GLFWwindow* win_;
  Store& store_;
  State& s_;
  std::vector<std::unique_ptr<Session>> sessions_;
  std::deque<InputEvent> events_;
  std::string maximized_;
  double now_ = 0;
  bool focused_ = true, dirty_ = false;
  double lastSave_ = 0;

  // fonts
  float uiScale_;
  bool fontsDirty_ = true;
  ImFont *uiFont_ = nullptr, *mono_ = nullptr, *monoBold_ = nullptr;
  ImVec2 cell_{9, 18};
  float monoPx_ = 16;
  ImVector<ImWchar> monoRanges_;

  // modal state
  Modal modal_ = Modal::None;
  bool modalJustOpened_ = false;
  char pathBuf_[1024] = "";
  char nameBuf_[256] = "";
  std::string modalError_;
  std::string modalTarget_;  // project/session id the modal acts on
  bool renameIsProject_ = false;
  int projectCombo_ = 0;
  bool wantWorktree_ = false;
  struct GitCache {
    std::string path;
    bool isGit = false;
    std::string branch;
  } gitCache_;
  std::string confirmTitle_, confirmText_;
  std::function<void()> confirmYes_;
  int closeChoice_ = 0;
  bool closeDirty_ = false;
  char cmdBuf_[512] = "";
  int fontSizeEdit_ = 16;
  bool notifyEdit_ = true;
  ThemeColors themeEdit_;

  struct Toast {
    std::string text;
    double until;
    bool err;
  };
  std::vector<Toast> toasts_;

  // ---- theme cache (recomputed once per frame by syncTheme()) ----
  ImU32 accent_ = 0, busy_ = 0, waiting_ = 0, done_ = 0, idle_ = 0;
  ImU32 surfaceU_ = 0, paneBgU_ = 0, paneBorderU_ = 0, terminalBgU_ = 0, chipBgU_ = 0;
  ImU32 textU_ = 0, textDisabledU_ = 0;
  ImU32 selectionU_ = 0, cursorFillU_ = 0, cursorOutlineU_ = 0;
  ImVec4 bgV4_{}, surfaceV4_{}, paneBgV4_{}, paneBorderV4_{};
  ImVec4 accentV4_{}, accentHoverV4_{}, accentActiveV4_{}, waitingV4_{};
  ImVec4 textV4_{}, textDisabledV4_{};

  // ---- usage fetch ----
  struct UsageFetch {
    std::mutex mu;
    bool inFlight = false, done = false;
    UsageInfo result;
  };
  std::shared_ptr<UsageFetch> usageFetch_;
};
