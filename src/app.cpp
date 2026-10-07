#include "app.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>

#include "git.h"
#include "platform.h"
#include "style.h"
#include "util.h"

namespace fs = std::filesystem;

namespace {

constexpr ImU32 rgbCol(uint32_t v, int a = 255) {
  return IM_COL32((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, a);
}

constexpr ImU32 withAlpha(ImU32 col, int alpha) { return (col & 0x00FFFFFF) | ((ImU32)alpha << 24); }

bool fileExists(const std::string& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

std::string firstExisting(std::initializer_list<std::string> paths) {
  for (auto& p : paths)
    if (!p.empty() && fileExists(p)) return p;
  return {};
}

std::string envOr(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

// Arm weights for light/heavy box-drawing characters: {left, right, up, down}; 0 none, 1 light, 2 heavy.
bool boxArms(uint32_t cp, int& l, int& r, int& u, int& d) {
  l = r = u = d = 0;
  switch (cp) {
    case 0x2500: l = r = 1; return true;
    case 0x2501: l = r = 2; return true;
    case 0x2502: u = d = 1; return true;
    case 0x2503: u = d = 2; return true;
    case 0x250C: case 0x256D: r = d = 1; return true;
    case 0x250F: r = d = 2; return true;
    case 0x2510: case 0x256E: l = d = 1; return true;
    case 0x2513: l = d = 2; return true;
    case 0x2514: case 0x2570: r = u = 1; return true;
    case 0x2517: r = u = 2; return true;
    case 0x2518: case 0x256F: l = u = 1; return true;
    case 0x251B: l = u = 2; return true;
    case 0x251C: u = d = r = 1; return true;
    case 0x2523: u = d = r = 2; return true;
    case 0x2524: u = d = l = 1; return true;
    case 0x252B: u = d = l = 2; return true;
    case 0x252C: l = r = d = 1; return true;
    case 0x2533: l = r = d = 2; return true;
    case 0x2534: l = r = u = 1; return true;
    case 0x253B: l = r = u = 2; return true;
    case 0x253C: l = r = u = d = 1; return true;
    case 0x254B: l = r = u = d = 2; return true;
    case 0x2550: l = r = 2; return true;  // double lines drawn heavy
    case 0x2551: u = d = 2; return true;
    case 0x2554: r = d = 2; return true;
    case 0x2557: l = d = 2; return true;
    case 0x255A: r = u = 2; return true;
    case 0x255D: l = u = 2; return true;
    case 0x2574: l = 1; return true;
    case 0x2575: u = 1; return true;
    case 0x2576: r = 1; return true;
    case 0x2577: d = 1; return true;
    default: return false;
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// construction, fonts
// ---------------------------------------------------------------------------------------------

App::App(GLFWwindow* win, Store& store, float uiScale)
    : win_(win), store_(store), s_(store.state), uiScale_(uiScale) {
  for (auto& rec : s_.sessions) sessions_.push_back(std::make_unique<Session>(rec));
  auto& ui = s_.ui;
  ui.openIds.erase(std::remove_if(ui.openIds.begin(), ui.openIds.end(), [&](const std::string& id) { return !find(id); }),
                   ui.openIds.end());
  if (!find(ui.activeId)) ui.activeId = ui.openIds.empty() ? "" : ui.openIds.front();
  if (ui.mode != "tile" && ui.mode != "tabs") ui.mode = "tile";
  Session::wake = [] { glfwPostEmptyEvent(); };
}

App::~App() { Session::wake = nullptr; }

bool App::rebuildFontsIfNeeded() {
  if (!fontsDirty_) return false;
  fontsDirty_ = false;
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();

  const std::string win = envOr("WINDIR").empty() ? "C:\\Windows" : envOr("WINDIR");
  const std::string uiPath = firstExisting({envOr("CT_FONT_UI"), win + "\\Fonts\\segoeui.ttf",
                                            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                            "/System/Library/Fonts/Helvetica.ttc"});
  const std::string monoPath = firstExisting({envOr("CT_FONT_MONO"), win + "\\Fonts\\CascadiaMono.ttf",
                                              win + "\\Fonts\\consola.ttf",
                                              "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                              "/System/Library/Fonts/Menlo.ttc"});
  const std::string boldPath = firstExisting({envOr("CT_FONT_MONO_BOLD"), win + "\\Fonts\\consolab.ttf",
                                              "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"});
  const std::string symPath = firstExisting({win + "\\Fonts\\seguisym.ttf"});

  // Glyphs Claude Code uses: Latin, Greek/Cyrillic, punctuation, arrows, box drawing, blocks,
  // geometric shapes, dingbats (spinner stars, prompt chevron), braille, powerline.
  ImFontGlyphRangesBuilder b;
  b.AddRanges(io.Fonts->GetGlyphRangesDefault());
  static const ImWchar extra[] = {0x0100, 0x024F, 0x0370, 0x03FF, 0x0400, 0x04FF, 0x2000, 0x2BFF, 0xE0A0, 0xE0D7, 0};
  b.AddRanges(extra);
  monoRanges_.clear();
  b.BuildRanges(&monoRanges_);

  const float uiPx = std::round(15.0f * uiScale_);
  monoPx_ = std::round((float)s_.settings.fontSize * uiScale_);

  ImFontConfig uiCfg;
  uiCfg.OversampleH = 2;
  uiFont_ = uiPath.empty() ? io.Fonts->AddFontDefault() : io.Fonts->AddFontFromFileTTF(uiPath.c_str(), uiPx, &uiCfg);

  ImFontConfig monoCfg;
  monoCfg.OversampleH = 1;
  monoCfg.OversampleV = 1;
  monoCfg.PixelSnapH = true;
  if (monoPath.empty()) {
    mono_ = io.Fonts->AddFontDefault();
  } else {
    mono_ = io.Fonts->AddFontFromFileTTF(monoPath.c_str(), monoPx_, &monoCfg, monoRanges_.Data);
    if (!symPath.empty()) {
      ImFontConfig m = monoCfg;
      m.MergeMode = true;
      static const ImWchar symRanges[] = {0x2190, 0x21FF, 0x2300, 0x23FF, 0x25A0, 0x25FF, 0x2600, 0x27BF, 0x2B00, 0x2BFF, 0};
      io.Fonts->AddFontFromFileTTF(symPath.c_str(), monoPx_, &m, symRanges);
    }
  }
  monoBold_ = nullptr;
  if (!boldPath.empty()) {
    static const ImWchar boldRanges[] = {0x20, 0xFF, 0};
    monoBold_ = io.Fonts->AddFontFromFileTTF(boldPath.c_str(), monoPx_, &monoCfg, boldRanges);
  }
  io.Fonts->Build();

  ImVec2 m = mono_->CalcTextSizeA(mono_->FontSize, FLT_MAX, 0.0f, "M");
  cell_.x = std::max(1.0f, std::round(m.x));
  cell_.y = std::ceil(mono_->FontSize * 1.18f);
  return true;
}

void App::syncTheme() {
  const ThemeColors& t = s_.settings.theme;
  auto u = [](const ThemeColor& c) { return ImGui::ColorConvertFloat4ToU32(toImVec4(c)); };
  accent_ = u(t.accent);
  busy_ = u(t.statusBusy);
  waiting_ = u(t.statusWaiting);
  done_ = u(t.statusDone);
  idle_ = u(t.statusIdle);
  surfaceU_ = u(t.surface);
  paneBgU_ = u(t.paneBg);
  paneBorderU_ = u(t.paneBorder);
  terminalBgU_ = u(t.terminalBg);
  chipBgU_ = ImGui::ColorConvertFloat4ToU32(shadeColor(t.surface, 0.06f));
  textU_ = u(t.text);
  textDisabledU_ = u(t.textDisabled);
  selectionU_ = u(t.selection);
  cursorFillU_ = u(t.cursor);
  cursorOutlineU_ = ImGui::ColorConvertFloat4ToU32(
      ImVec4(t.cursor.r, t.cursor.g, t.cursor.b, std::clamp(t.cursor.a + 0.12f, 0.0f, 1.0f)));

  bgV4_ = toImVec4(t.background);
  surfaceV4_ = toImVec4(t.surface);
  paneBgV4_ = toImVec4(t.paneBg);
  paneBorderV4_ = toImVec4(t.paneBorder);
  accentV4_ = toImVec4(t.accent);
  accentHoverV4_ = shadeColor(t.accent, 0.05f);
  accentActiveV4_ = shadeColor(t.accent, -0.07f);
  waitingV4_ = toImVec4(t.statusWaiting);
  textV4_ = toImVec4(t.text);
  textDisabledV4_ = toImVec4(t.textDisabled);
}

void App::startUsageFetch() {
  auto fetch = std::make_shared<UsageFetch>();
  fetch->inFlight = true;
  usageFetch_ = fetch;
  std::thread([fetch] {
    UsageInfo r = fetchClaudeUsage();
    std::lock_guard<std::mutex> g(fetch->mu);
    fetch->result = std::move(r);
    fetch->inFlight = false;
    fetch->done = true;
    if (Session::wake) Session::wake();
  }).detach();
}

void App::changeFontSize(int delta) {
  int n = delta == 0 ? 16 : std::clamp(s_.settings.fontSize + delta, 8, 36);
  if (n == s_.settings.fontSize) return;
  s_.settings.fontSize = n;
  fontsDirty_ = true;
  markDirty();
}

// ---------------------------------------------------------------------------------------------
// model
// ---------------------------------------------------------------------------------------------

Session* App::find(const std::string& id) const {
  for (auto& s : sessions_)
    if (s->rec.id == id) return s.get();
  return nullptr;
}

Project* App::findProject(const std::string& id) {
  for (auto& p : s_.projects)
    if (p.id == id) return &p;
  return nullptr;
}

Session* App::active() const { return find(s_.ui.activeId); }

std::vector<std::string> App::visibleIds() const {
  std::vector<std::string> open;
  for (auto& id : s_.ui.openIds)
    if (find(id)) open.push_back(id);
  if (s_.ui.mode == "tabs") {
    if (open.empty()) return {};
    bool has = std::find(open.begin(), open.end(), s_.ui.activeId) != open.end();
    return {has ? s_.ui.activeId : open.front()};
  }
  if (!maximized_.empty() && std::find(open.begin(), open.end(), maximized_) != open.end()) return {maximized_};
  return open;
}

void App::focusSession(const std::string& id) {
  if (std::find(s_.ui.openIds.begin(), s_.ui.openIds.end(), id) == s_.ui.openIds.end()) return;
  Session* s = find(id);
  if (!s) return;
  if (s_.ui.activeId != id) {
    s_.ui.activeId = id;
    markDirty();
  }
  s->unseen = false;
}

void App::openSession(const std::string& id) {
  if (!find(id)) return;
  auto& open = s_.ui.openIds;
  if (std::find(open.begin(), open.end(), id) == open.end()) open.push_back(id);
  if (!maximized_.empty() && maximized_ != id) maximized_.clear();
  focusSession(id);
  markDirty();
}

void App::hideSession(const std::string& id) {
  auto& open = s_.ui.openIds;
  open.erase(std::remove(open.begin(), open.end(), id), open.end());
  if (s_.ui.activeId == id) s_.ui.activeId = open.empty() ? "" : open.back();
  if (maximized_ == id) maximized_.clear();
  if (Session* s = find(s_.ui.activeId)) s->unseen = false;
  markDirty();
}

void App::toggleMaximize(const std::string& id) {
  if (s_.ui.mode == "tabs" || id.empty()) return;
  maximized_ = maximized_ == id ? "" : id;
}

Session* App::createSession(const std::string& projectId, const std::string& name, bool worktree, std::string& err) {
  Project* p = findProject(projectId);
  if (!p) {
    err = "Project not found";
    return nullptr;
  }
  if (!fileExists(p->path)) {
    err = "Folder no longer exists: " + p->path;
    return nullptr;
  }
  int count = 0;
  for (auto& s : sessions_)
    if (s->rec.projectId == projectId) count++;
  std::string finalName = util::trim(name);
  if (finalName.empty()) finalName = "Session " + std::to_string(count + 1);

  SessionRec rec;
  rec.id = util::uuid4();
  rec.projectId = projectId;
  rec.name = finalName;
  rec.cwd = p->path;
  rec.claudeSessionId = util::uuid4();
  rec.createdAt = (long long)std::time(nullptr);
  if (worktree) {
    git::Worktree wt;
    if (!git::createWorktree(p->path, finalName, wt, err)) return nullptr;
    rec.worktree = Worktree{wt.repoRoot, wt.branch, wt.path};
    rec.cwd = wt.cwd;
  }
  sessions_.push_back(std::make_unique<Session>(rec));
  markDirty();
  return sessions_.back().get();
}

void App::closeSession(const std::string& id, int worktreeAction) {
  Session* s = find(id);
  if (!s) return;
  s->kill(now_, nullptr);
  std::string warning;
  if (s->rec.worktree && worktreeAction > 0) {
    bool branchKept = false;
    std::string err;
    if (git::removeWorktree(s->rec.worktree->repoRoot, s->rec.worktree->path, s->rec.worktree->branch,
                            worktreeAction == 2, branchKept, err)) {
      if (branchKept) warning = "Worktree removed. Branch " + s->rec.worktree->branch + " was kept (unmerged commits).";
    } else {
      warning = "Could not remove the worktree (" + err + "). It was left at " + s->rec.worktree->path;
    }
  }
  hideSession(id);
  sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [&](auto& p) { return p->rec.id == id; }),
                  sessions_.end());
  markDirty();
  if (!warning.empty()) toast(warning);
}

void App::removeProject(const std::string& id) {
  std::vector<std::string> ids;
  for (auto& s : sessions_)
    if (s->rec.projectId == id) ids.push_back(s->rec.id);
  for (auto& sid : ids) closeSession(sid, 0);
  s_.projects.erase(std::remove_if(s_.projects.begin(), s_.projects.end(), [&](auto& p) { return p.id == id; }),
                    s_.projects.end());
  markDirty();
}

bool App::addProjectPath(const std::string& rawPath, std::string& err, std::string* outId) {
  std::string path = util::trim(rawPath);
  if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
  std::error_code ec;
  if (path.empty() || !fs::is_directory(path, ec)) {
    err = "That folder doesn't exist.";
    return false;
  }
  std::string norm = fs::path(path).lexically_normal().make_preferred().string();
  while (norm.size() > 1 && (norm.back() == '/' || norm.back() == '\\')) norm.pop_back();
  for (auto& p : s_.projects) {
    if (p.path == norm) {
      if (outId) *outId = p.id;
      return true;
    }
  }
  Project p{util::uuid4(), util::baseName(norm), norm};
  s_.projects.push_back(p);
  if (outId) *outId = p.id;
  markDirty();
  return true;
}

Session::StatusFn App::statusCb() {
  return [this](Session& s, Status o, Status n, double busy) { onStatus(s, o, n, busy); };
}

void App::onStatus(Session& s, Status, Status n, double busy) {
  bool visible = false;
  for (auto& id : visibleIds())
    if (id == s.rec.id) visible = true;
  bool watching = focused_ && s_.ui.activeId == s.rec.id && visible;
  bool waiting = n == Status::Waiting;
  bool done = n == Status::Idle && busy >= 4.0;
  if ((waiting || done) && !watching) {
    s.unseen = true;
    if (!focused_) {
      Project* p = findProject(s.rec.projectId);
      if (s_.settings.notifications)
        platform::notify(win_, (p ? p->name : std::string("Claude")) + " · " + s.rec.name,
                         waiting ? "Waiting for your input or approval" : "Finished — ready for your next prompt");
      platform::flash(win_, true);
    }
  }
  if (n == Status::Busy || n == Status::Exited) s.unseen = false;
}

void App::saveNow() {
  s_.sessions.clear();
  for (auto& s : sessions_) s_.sessions.push_back(s->rec);
  store_.save();
  dirty_ = false;
  lastSave_ = now_;
}

void App::toast(const std::string& text, bool err) { toasts_.push_back({text, now_ + (err ? 9.0 : 6.0), err}); }

void App::shutdown() {
  for (auto& s : sessions_) s->kill(now_, nullptr);
  saveNow();
  platform::shutdown(win_);
}

bool App::wantsFastFrames() const {
  if (!toasts_.empty() || modal_ != Modal::None) return true;
  for (auto& s : sessions_)
    if (s->status == Status::Busy || s->status == Status::Starting) return true;
  return false;
}

// ---------------------------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------------------------

void App::pushChar(uint32_t cp) {
  bool ctrl = glfwGetKey(win_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(win_, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
  bool alt = glfwGetKey(win_, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(win_, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
  if ((ctrl || alt) && !(ctrl && alt)) return;  // handled from the key event (AltGr = ctrl+alt delivers chars)
  InputEvent e;
  e.type = InputEvent::Char;
  e.cp = cp;
  events_.push_back(e);
}

void App::pushKey(int key, int action, int mods) {
  if (action == GLFW_RELEASE) return;
  InputEvent e;
  e.type = InputEvent::Key;
  e.key = key;
  e.mods = mods;
  e.action = action;
  events_.push_back(e);
}

void App::pushDrop(const std::vector<std::string>& paths) {
  for (auto& p : paths) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) continue;
    std::string err, id;
    if (addProjectPath(p, err, &id)) {
      s_.ui.collapsed[id] = false;
      toast("Added " + util::baseName(p));
    }
  }
}

void App::handleEvents() {
  ImGuiIO& io = ImGui::GetIO();
  bool blocked = modal_ != Modal::None || io.WantTextInput;
  while (!events_.empty()) {
    InputEvent e = events_.front();
    events_.pop_front();
    if (blocked) continue;
    if (e.type == InputEvent::Key) {
      handleKey(e);
    } else if (e.cp >= 32 && e.cp != 127) {
      Session* s = active();
      if (s && s->running()) {
        s->scrollOffset = 0;
        s->sendText(util::utf8(e.cp), now_, statusCb());
      }
    }
  }
}

void App::copySelection(Session& s) {
  if (!s.term || !s.hasSel) return;
  int ra = s.term->absRow(s.selA_row), rb = s.term->absRow(s.selB_row);
  int ca = s.selA_col, cb = s.selB_col;
  if (ra > rb || (ra == rb && ca > cb)) {
    std::swap(ra, rb);
    std::swap(ca, cb);
  }
  std::string text = s.term->text(ra, ca, rb, cb);
  if (!text.empty()) glfwSetClipboardString(win_, text.c_str());
  s.hasSel = false;
}

void App::paste(Session& s)
{
  if (!s.running())
  {
    return;
  }

  const char* clip = glfwGetClipboardString(win_);
  const bool hasText = clip && *clip;
  if (!hasText)
  {
    if (!platform::clipboardHasImage())
    {
      return;
    }
    // Claude Code reads the OS clipboard itself when it sees a raw Alt+V keystroke (its image-paste
    // trigger on Windows) rather than receiving image bytes over the pty, so deliver that same byte
    // sequence here instead of the clipboard text path below.
    s.sendText("\x1bv", now_, statusCb());
    return;
  }

  std::string text;
  for (const char* p = clip; *p; p++)
  {
    if (*p == '\x1b')
    {
      continue;  // never let pasted text inject escape sequences
    }
    if (*p == '\r')
    {
      text += '\r';
      if (p[1] == '\n')
      {
        p++;
      }
    }
    else if (*p == '\n')
    {
      text += '\r';
    }
    else
    {
      text += *p;
    }
  }
  auto cb = statusCb();
  double now = now_;
  s.typing(now, cb, [&](Terminal& t) {
    t.startPaste();
    s.sendText(text, now, cb);
    t.endPaste();
  });
}

void App::handleKey(const InputEvent& e) {
  const bool ctrl = e.mods & GLFW_MOD_CONTROL, alt = e.mods & GLFW_MOD_ALT, shift = e.mods & GLFW_MOD_SHIFT;
  const int k = e.key;
  Session* s = active();

  // ---- app shortcuts (never reach Claude) ----
  if (alt && !ctrl && !shift && k >= GLFW_KEY_1 && k <= GLFW_KEY_9) {
    auto v = visibleIds();
    size_t i = (size_t)(k - GLFW_KEY_1);
    if (i < v.size()) focusSession(v[i]);
    return;
  }
  if (ctrl && !alt && (k == GLFW_KEY_EQUAL || k == GLFW_KEY_KP_ADD)) return changeFontSize(+1);
  if (ctrl && !alt && !shift && (k == GLFW_KEY_MINUS || k == GLFW_KEY_KP_SUBTRACT)) return changeFontSize(-1);
  if (ctrl && !alt && !shift && (k == GLFW_KEY_0 || k == GLFW_KEY_KP_0)) return changeFontSize(0);
  if (ctrl && shift && !alt) {
    switch (k) {
      case GLFW_KEY_N:
        if (s_.projects.empty()) openModal(Modal::AddProject);
        else {
          modalTarget_ = s ? s->rec.projectId : s_.projects.front().id;
          openModal(Modal::NewSession);
        }
        return;
      case GLFW_KEY_M: toggleMaximize(s_.ui.activeId); return;
      case GLFW_KEY_W: hideSession(s_.ui.activeId); return;
      case GLFW_KEY_C: if (s) copySelection(*s); return;
      case GLFW_KEY_V: if (s) paste(*s); return;
      default: break;
    }
  }

  if (!s || !s->running()) return;
  auto cb = statusCb();
  auto send = [&](const std::string& bytes) {
    s->scrollOffset = 0;
    s->sendText(bytes, now_, cb);
  };

  if (ctrl && !shift && !alt && k == GLFW_KEY_C && s->hasSel) return copySelection(*s);  // otherwise: interrupt
  if (ctrl && !alt && k == GLFW_KEY_V) return paste(*s);
  if (ctrl && alt) return;  // AltGr combinations arrive as characters

  // Local scrollback navigation
  if (shift && !ctrl && !alt && s->term && (k == GLFW_KEY_PAGE_UP || k == GLFW_KEY_PAGE_DOWN)) {
    int page = std::max(1, s->term->rows() - 1);
    s->scrollOffset = std::clamp(s->scrollOffset + (k == GLFW_KEY_PAGE_UP ? page : -page), 0, s->term->scrollbackSize());
    return;
  }

  VTermModifier vm = (VTermModifier)((shift ? VTERM_MOD_SHIFT : 0) | (alt ? VTERM_MOD_ALT : 0) | (ctrl ? VTERM_MOD_CTRL : 0));
  VTermKey vk = VTERM_KEY_NONE;
  switch (k) {
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:
      if (shift && !ctrl && !alt) return send("\x1b\r");  // newline in the prompt
      vk = VTERM_KEY_ENTER;
      break;
    case GLFW_KEY_TAB: vk = VTERM_KEY_TAB; break;
    case GLFW_KEY_BACKSPACE: vk = VTERM_KEY_BACKSPACE; break;
    case GLFW_KEY_ESCAPE: vk = VTERM_KEY_ESCAPE; break;
    case GLFW_KEY_UP: vk = VTERM_KEY_UP; break;
    case GLFW_KEY_DOWN: vk = VTERM_KEY_DOWN; break;
    case GLFW_KEY_LEFT: vk = VTERM_KEY_LEFT; break;
    case GLFW_KEY_RIGHT: vk = VTERM_KEY_RIGHT; break;
    case GLFW_KEY_INSERT: vk = VTERM_KEY_INS; break;
    case GLFW_KEY_DELETE: vk = VTERM_KEY_DEL; break;
    case GLFW_KEY_HOME: vk = VTERM_KEY_HOME; break;
    case GLFW_KEY_END: vk = VTERM_KEY_END; break;
    case GLFW_KEY_PAGE_UP: vk = VTERM_KEY_PAGEUP; break;
    case GLFW_KEY_PAGE_DOWN: vk = VTERM_KEY_PAGEDOWN; break;
    default:
      if (k >= GLFW_KEY_F1 && k <= GLFW_KEY_F12) vk = (VTermKey)(VTERM_KEY_FUNCTION_0 + (k - GLFW_KEY_F1 + 1));
      break;
  }
  if (vk != VTERM_KEY_NONE) {
    s->typing(now_, cb, [&](Terminal& t) { t.key(vk, vm); });
    return;
  }

  // Ctrl/Alt + letter: send control bytes ourselves (libvterm would use CSI-u for some of them).
  if ((ctrl || alt) && k >= GLFW_KEY_A && k <= GLFW_KEY_Z) {
    char c = (char)('a' + (k - GLFW_KEY_A));
    std::string out;
    if (alt) out += '\x1b';
    if (ctrl) out += (char)(c - 'a' + 1);
    else out += shift ? (char)std::toupper(c) : c;
    return send(out);
  }
  if (ctrl && !alt) {
    switch (k) {
      case GLFW_KEY_SPACE: return send(std::string(1, '\0'));
      case GLFW_KEY_LEFT_BRACKET: return send("\x1b");
      case GLFW_KEY_BACKSLASH: return send("\x1c");
      case GLFW_KEY_RIGHT_BRACKET: return send("\x1d");
      case GLFW_KEY_SLASH: return send("\x1f");
      default: break;
    }
  }
  if (alt && !ctrl && k >= 32 && k <= 96) {  // Alt+digit / punctuation
    return send(std::string("\x1b") + (char)k);
  }
}

// ---------------------------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------------------------

void App::frame(double now) {
  now_ = now;
  syncTheme();
  bool f = glfwGetWindowAttrib(win_, GLFW_FOCUSED) != 0;
  if (f && !focused_) {
    platform::flash(win_, false);
    if (Session* a = active()) a->unseen = false;
  }
  focused_ = f;

  auto cb = statusCb();
  for (auto& s : sessions_) s->tick(now_, cb);

  handleEvents();
  drawRoot();
  drawModals();
  drawToasts();

  if (dirty_ && now_ - lastSave_ > 0.5) saveNow();
}

void App::statusDot(ImDrawList* dl, ImVec2 c, float r, const Session& s) const {
  switch (s.status) {
    case Status::Busy: {
      float pulse = 0.55f + 0.45f * std::sin((float)now_ * 6.0f);
      dl->AddCircleFilled(c, r * (0.8f + 0.2f * pulse), withAlpha(busy_, (int)(110 + 145 * pulse)));
      break;
    }
    case Status::Starting: dl->AddCircleFilled(c, r, withAlpha(busy_, 150)); break;
    case Status::Waiting:
      dl->AddCircleFilled(c, r + 3, withAlpha(waiting_, 60));
      dl->AddCircleFilled(c, r, waiting_);
      break;
    case Status::Idle:
      if (s.unseen) {
        dl->AddCircleFilled(c, r + 3, withAlpha(done_, 60));
        dl->AddCircleFilled(c, r, done_);
      } else {
        dl->AddCircleFilled(c, r, withAlpha(done_, 130));
      }
      break;
    case Status::Exited: dl->AddCircle(c, r - 0.5f, idle_, 0, 1.5f); break;
  }
}

void App::drawRoot() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, bgV4_);
  ImGui::Begin("##root", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar(3);

  float sw = std::clamp(s_.ui.sidebarWidth, 200.0f * uiScale_, 520.0f * uiScale_);
  drawSidebar(sw);
  ImGui::SameLine(0, 0);

  ImGui::PushStyleColor(ImGuiCol_Button, paneBorderV4_);
  ImGui::InvisibleButton("##splitter", ImVec2(5.0f * uiScale_, -1));
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (ImGui::IsItemActive()) {
    s_.ui.sidebarWidth = std::clamp(sw + ImGui::GetIO().MouseDelta.x, 200.0f * uiScale_, 520.0f * uiScale_);
    markDirty();
  }
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImGui::GetWindowDrawList()->AddRectFilled(a, b, ImGui::IsItemActive() ? accent_ : paneBorderU_);
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 0);

  drawMain();
  ImGui::End();
  ImGui::PopStyleColor();
}

void App::drawSidebar(float width) {
  const float u = uiScale_;

  // A row icon button (the "+"/"x" on project and session rows): transparent at rest so it reads
  // as part of the row, and on hover/press it fills with the exact same flat rectangle the row
  // itself uses for its own hover/selected highlight, instead of looking like a separate pill button.
  auto iconButton = [&](const char* label) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    bool clicked = ImGui::SmallButton(label);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    return clicked;
  };

  ImGui::PushStyleColor(ImGuiCol_ChildBg, surfaceV4_);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * u, 10 * u));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * u, 6 * u));
  ImGui::BeginChild("##side", ImVec2(width, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

  ImGui::PushStyleColor(ImGuiCol_Text, accentV4_);
  ImGui::TextUnformatted("*");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextUnformatted("Claude Terminal");
  ImGui::Spacing();

  ImGui::PushStyleColor(ImGuiCol_Button, accentV4_);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accentHoverV4_);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, accentActiveV4_);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.06f, 0.04f, 1));
  if (ImGui::Button("+ Add project folder", ImVec2(-1, 0))) {
    pathBuf_[0] = 0;
    modalError_.clear();
    openModal(Modal::AddProject);
  }
  ImGui::PopStyleColor(4);
  ImGui::Spacing();

  const float footer = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
  ImGui::BeginChild("##tree", ImVec2(0, -footer), ImGuiChildFlags_None);
  if (s_.projects.empty()) ImGui::TextDisabled("No projects yet.");

  for (size_t pi = 0; pi < s_.projects.size(); pi++) {
    Project& p = s_.projects[pi];
    ImGui::PushID(p.id.c_str());
    bool collapsed = s_.ui.collapsed[p.id];

    // attention summary for the header dot
    const Session* worst = nullptr;
    for (auto& s : sessions_) {
      if (s->rec.projectId != p.id) continue;
      if (s->status == Status::Waiting) { worst = s.get(); break; }
      if (s->unseen) worst = s.get();
      else if (!worst && s->status == Status::Busy) worst = s.get();
    }

    ImGui::SetNextItemOpen(!collapsed, ImGuiCond_Always);
    bool open = ImGui::TreeNodeEx(p.name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding |
                                                       ImGuiTreeNodeFlags_AllowOverlap);
    if (ImGui::IsItemToggledOpen()) {
      s_.ui.collapsed[p.id] = !open;
      markDirty();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.path.c_str());
    if (ImGui::BeginPopupContextItem("##projmenu")) {
      if (ImGui::MenuItem("New session")) {
        modalTarget_ = p.id;
        openModal(Modal::NewSession);
      }
      if (ImGui::MenuItem("Rename project")) {
        modalTarget_ = p.id;
        renameIsProject_ = true;
        std::snprintf(nameBuf_, sizeof nameBuf_, "%s", p.name.c_str());
        openModal(Modal::Rename);
      }
      if (ImGui::MenuItem("Reveal folder")) platform::reveal(p.path);
      ImGui::Separator();
      if (ImGui::MenuItem("Remove project")) {
        std::string id = p.id, nm = p.name;
        confirmTitle_ = "Remove \"" + nm + "\"?";
        confirmText_ = "This stops its sessions and removes the project from the list.\nFiles and git worktrees on disk are left untouched.";
        confirmYes_ = [this, id] { removeProject(id); };
        openModal(Modal::Confirm);
      }
      ImGui::EndPopup();
    }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 24 * u);
    if (iconButton("+")) {
      modalTarget_ = p.id;
      openModal(Modal::NewSession);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New session");
    if (worst) {
      ImVec2 c(ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x - 38 * u,
               ImGui::GetItemRectMin().y + ImGui::GetItemRectSize().y * 0.5f);
      statusDot(ImGui::GetWindowDrawList(), c, 4.5f * u, *worst);
    }

    if (open) {
      bool any = false;
      for (auto& sp : sessions_) {
        Session& s = *sp;
        if (s.rec.projectId != p.id) continue;
        any = true;
        ImGui::PushID(s.rec.id.c_str());
        bool isOpen = std::find(s_.ui.openIds.begin(), s_.ui.openIds.end(), s.rec.id) != s_.ui.openIds.end();
        bool isActive = s_.ui.activeId == s.rec.id;
        float h = ImGui::GetFrameHeight();
        float w = ImGui::GetContentRegionAvail().x - 26 * u;
        ImGui::Indent(12 * u);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##s", isActive, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(w - 12 * u, h))) {
          openSession(s.rec.id);
          if (ImGui::IsMouseDoubleClicked(0)) {
            modalTarget_ = s.rec.id;
            renameIsProject_ = false;
            std::snprintf(nameBuf_, sizeof nameBuf_, "%s", s.rec.name.c_str());
            openModal(Modal::Rename);
          }
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (isOpen && !isActive) dl->AddRectFilled(ImVec2(p0.x - 4 * u, p0.y + 3), ImVec2(p0.x - 2 * u, p0.y + h - 3), idle_);
        statusDot(dl, ImVec2(p0.x + 9 * u, p0.y + h * 0.5f), 4.5f * u, s);
        dl->AddText(ImVec2(p0.x + 22 * u, p0.y + (h - ImGui::GetFontSize()) * 0.5f), textU_,
                    s.rec.name.c_str());
        if (s.rec.worktree) {
          const char* tag = "wt";
          ImVec2 ts = ImGui::CalcTextSize(tag);
          dl->AddText(ImVec2(p0.x + w - 12 * u - ts.x - 6 * u, p0.y + (h - ts.y) * 0.5f), textDisabledU_, tag);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s%s", s.rec.cwd.c_str(), s.rec.worktree ? "\nbranch: " : "",
                                                      s.rec.worktree ? s.rec.worktree->branch.c_str() : "");
        if (ImGui::BeginPopupContextItem("##sessmenu")) {
          if (ImGui::MenuItem("Open")) openSession(s.rec.id);
          if (ImGui::MenuItem("Rename")) {
            modalTarget_ = s.rec.id;
            renameIsProject_ = false;
            std::snprintf(nameBuf_, sizeof nameBuf_, "%s", s.rec.name.c_str());
            openModal(Modal::Rename);
          }
          if (isOpen && ImGui::MenuItem("Hide pane")) hideSession(s.rec.id);
          ImGui::Separator();
          if (ImGui::MenuItem("Close session...")) {
            modalTarget_ = s.rec.id;
            if (s.rec.worktree) {
              closeChoice_ = 0;
              closeDirty_ = git::isDirty(s.rec.worktree->path);
              openModal(Modal::CloseSession);
            } else {
              std::string id = s.rec.id;
              confirmTitle_ = "Close \"" + s.rec.name + "\"?";
              confirmText_ = "This stops Claude in this session. The conversation stays in Claude's own history.";
              confirmYes_ = [this, id] { closeSession(id, 0); };
              openModal(Modal::Confirm);
            }
          }
          ImGui::EndPopup();
        }
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2 * u);
        if (iconButton("x")) {
          modalTarget_ = s.rec.id;
          if (s.rec.worktree) {
            closeChoice_ = 0;
            closeDirty_ = git::isDirty(s.rec.worktree->path);
            openModal(Modal::CloseSession);
          } else {
            std::string id = s.rec.id;
            confirmTitle_ = "Close \"" + s.rec.name + "\"?";
            confirmText_ = "This stops Claude in this session. The conversation stays in Claude's own history.";
            confirmYes_ = [this, id] { closeSession(id, 0); };
            openModal(Modal::Confirm);
          }
        }
        ImGui::Unindent(12 * u);
        ImGui::PopID();
      }
      if (!any) {
        ImGui::Indent(12 * u);
        ImGui::TextDisabled("No sessions yet");
        ImGui::Unindent(12 * u);
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  ImGui::EndChild();

  ImGui::Separator();
  ImGui::TextDisabled("Alt+1-9  focus pane");
  ImGui::TextDisabled("Ctrl+Shift+N  new session");
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

void App::drawToolbar() {
  const float u = uiScale_;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * u, 6 * u));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * u, 6 * u));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, surfaceV4_);
  float h = ImGui::GetFrameHeight() + 12 * u;
  ImGui::BeginChild("##toolbar", ImVec2(0, h), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // right-aligned controls first so chips know how much room is left
  float rightW = 0;
  const char* showRunning = "Show running";
  const char* tileLbl = "Tiles";
  const char* tabLbl = "Tabs";
  float padX = ImGui::GetStyle().FramePadding.x * 2;
  rightW = ImGui::CalcTextSize(showRunning).x + ImGui::CalcTextSize(tileLbl).x + ImGui::CalcTextSize(tabLbl).x +
           ImGui::CalcTextSize("Settings").x + ImGui::CalcTextSize("Usage").x + padX * 5 + 42 * u;
  float avail = ImGui::GetContentRegionAvail().x;

  bool anyChip = false;
  for (auto& id : s_.ui.openIds) {
    Session* s = find(id);
    if (!s) continue;
    anyChip = true;
    if (ImGui::GetCursorPosX() + 90 * u > avail - rightW) break;
    bool act = id == s_.ui.activeId;
    ImGui::PushID(id.c_str());
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(s->rec.name.c_str());
    float w = ts.x + 40 * u;
    if (ImGui::InvisibleButton("##chip", ImVec2(w, ImGui::GetFrameHeight()))) focusSession(id);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + ImGui::GetFrameHeight()), chipBgU_, 6);
    if (act) dl->AddRect(p0, ImVec2(p0.x + w, p0.y + ImGui::GetFrameHeight()), accent_, 6);
    statusDot(dl, ImVec2(p0.x + 12 * u, p0.y + ImGui::GetFrameHeight() * 0.5f), 4.0f * u, *s);
    dl->AddText(ImVec2(p0.x + 24 * u, p0.y + (ImGui::GetFrameHeight() - ts.y) * 0.5f), textU_, s->rec.name.c_str());
    ImGui::SameLine();
    ImGui::PopID();
  }
  if (!anyChip) ImGui::TextDisabled("No open sessions");

  ImGui::SameLine(avail - rightW + 8 * u);
  if (ImGui::Button(showRunning)) {
    std::vector<std::string> running;
    for (auto& s : sessions_)
      if (s->status != Status::Exited) running.push_back(s->rec.id);
    if (running.empty()) {
      toast("No sessions are running right now.");
    } else {
      std::vector<std::string> open;
      for (auto& id : s_.ui.openIds)
        if (std::find(running.begin(), running.end(), id) != running.end()) open.push_back(id);
      for (auto& id : running)
        if (std::find(open.begin(), open.end(), id) == open.end()) open.push_back(id);
      s_.ui.openIds = open;
      maximized_.clear();
      markDirty();
    }
  }
  ImGui::SameLine();
  bool tile = s_.ui.mode == "tile";
  ImGui::PushStyleColor(ImGuiCol_Button, tile ? ImGui::ColorConvertU32ToFloat4(withAlpha(accent_, 90)) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
  if (ImGui::Button(tileLbl)) {
    s_.ui.mode = "tile";
    maximized_.clear();
    markDirty();
  }
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 2);
  ImGui::PushStyleColor(ImGuiCol_Button, !tile ? ImGui::ColorConvertU32ToFloat4(withAlpha(accent_, 90)) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
  if (ImGui::Button(tabLbl)) {
    s_.ui.mode = "tabs";
    maximized_.clear();
    markDirty();
  }
  ImGui::PopStyleColor();
  ImGui::SameLine();
  if (ImGui::Button("Usage")) openModal(Modal::Usage);
  ImGui::SameLine();
  if (ImGui::Button("Settings")) {
    std::snprintf(cmdBuf_, sizeof cmdBuf_, "%s", s_.settings.claudeCommand.c_str());
    fontSizeEdit_ = s_.settings.fontSize;
    notifyEdit_ = s_.settings.notifications;
    themeEdit_ = s_.settings.theme;
    openModal(Modal::Settings);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

void App::drawMain() {
  const float u = uiScale_;
  ImGui::BeginChild("##main", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  drawToolbar();

  auto ids = visibleIds();
  ImVec2 avail = ImGui::GetContentRegionAvail();
  if (ids.empty()) {
    const char* msg = s_.projects.empty() ? "Add a project folder, then start a Claude session in it."
                                          : "Pick a session in the sidebar, or press + next to a project to start one.";
    ImVec2 ts = ImGui::CalcTextSize(msg);
    ImGui::SetCursorPos(ImVec2((avail.x - ts.x) * 0.5f, ImGui::GetCursorPosY() + avail.y * 0.4f));
    ImGui::TextDisabled("%s", msg);
    ImGui::EndChild();
    return;
  }

  const float gap = 6 * u;
  int n = (int)ids.size();
  int cols = n <= 2 ? n : (int)std::ceil(std::sqrt((double)n));
  int rows = (n + cols - 1) / cols;
  ImVec2 origin = ImGui::GetCursorPos();
  origin.x += gap;
  origin.y += gap;
  float cw = (avail.x - gap * (cols + 1)) / cols;
  float ch = (avail.y - gap * (rows + 1)) / rows;

  for (int i = 0; i < n; i++) {
    Session* s = find(ids[(size_t)i]);
    if (!s) continue;
    int r = i / cols, c = i % cols;
    float w = cw;
    if (i == n - 1 && n % cols != 0) w = cw * (cols - (n % cols) + 1) + gap * (cols - (n % cols));
    ImVec2 pos(origin.x + c * (cw + gap), origin.y + r * (ch + gap));
    drawPane(*s, pos, ImVec2(w, ch), i);
  }
  ImGui::EndChild();
}

void App::drawPane(Session& s, ImVec2 pos, ImVec2 size, int index) {
  const float u = uiScale_;
  bool isActive = s_.ui.activeId == s.rec.id;
  ImGui::SetCursorPos(pos);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, paneBgV4_);
  ImGui::PushStyleColor(ImGuiCol_Border, s.status == Status::Waiting ? waitingV4_
                                         : isActive                    ? accentV4_
                                                                       : paneBorderV4_);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6 * u);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * u, 0));
  ImGui::BeginChild(("##pane" + s.rec.id).c_str(), size, ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
      (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2)))
    focusSession(s.rec.id);

  // ---- header ----
  float hh = ImGui::GetFrameHeight() + 4 * u;
  ImVec2 hp = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  dl->AddRectFilled(hp, ImVec2(hp.x + size.x, hp.y + hh), surfaceU_);
  dl->AddLine(ImVec2(hp.x, hp.y + hh), ImVec2(hp.x + size.x, hp.y + hh), paneBorderU_);
  statusDot(dl, ImVec2(hp.x + 14 * u, hp.y + hh * 0.5f), 4.5f * u, s);
  Project* proj = findProject(s.rec.projectId);
  std::string sub = (proj ? proj->name : std::string()) + (s.rec.worktree ? "  [" + s.rec.worktree->branch + "]" : "");

  ImGui::SetCursorPos(ImVec2(28 * u, (hh - ImGui::GetTextLineHeight()) * 0.5f));
  ImGui::TextUnformatted(s.rec.name.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("%s", sub.c_str());

  const char* maxLbl = maximized_ == s.rec.id ? "Restore" : "Max";
  float bPad = ImGui::GetStyle().FramePadding.x * 2 + 6 * u;
  float bw = ImGui::CalcTextSize("Restart").x + ImGui::CalcTextSize(maxLbl).x + ImGui::CalcTextSize("Hide").x + bPad * 3;
  std::string num = "Alt+" + std::to_string(index + 1);
  float nw = ImGui::CalcTextSize(num.c_str()).x + 10 * u;
  ImGui::SetCursorPos(ImVec2(size.x - bw - nw - 6 * u, (hh - ImGui::GetFrameHeight()) * 0.5f));
  ImGui::TextDisabled("%s", num.c_str());
  ImGui::SameLine();
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5 * u, 1 * u));
  ImGui::PushID("hdr");
  if (ImGui::SmallButton("Restart")) {
    std::string id = s.rec.id;
    confirmTitle_ = "Restart session?";
    confirmText_ = s.status == Status::Busy || s.status == Status::Waiting
                       ? "Claude is active right now. Restarting stops it and resumes the conversation from its last saved state."
                       : "Restarting stops Claude and resumes the conversation from its last saved state.";
    confirmYes_ = [this, id] {
      if (Session* t = find(id)) {
        t->kill(now_, statusCb());
        t->attempted = false;
      }
    };
    openModal(Modal::Confirm);
  }
  ImGui::SameLine();
  if (s_.ui.mode == "tile" && ImGui::SmallButton(maxLbl)) toggleMaximize(s.rec.id);
  if (s_.ui.mode == "tile") ImGui::SameLine();
  if (ImGui::SmallButton("Hide")) hideSession(s.rec.id);
  ImGui::PopID();
  ImGui::PopStyleVar();

  // Drag handle spans the whole header but is submitted last, after the Restart/Max/Hide buttons
  // above: on a mouse-press frame, whichever widget is submitted first at a given position claims
  // the click, so those buttons (submitted earlier) keep working over the overlap; this invisible
  // button only ever "wins" the press in the header's empty space.
  ImGui::SetCursorScreenPos(hp);
  ImGui::InvisibleButton("##paneDragHandle", ImVec2(size.x, hh));
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("CT_PANE_SESSION_ID", s.rec.id.c_str(), s.rec.id.size() + 1);
    const ImVec2 cardSize(150 * u, 56 * u);
    ImGui::Dummy(cardSize);
    ImVec2 cp0 = ImGui::GetItemRectMin(), cp1 = ImGui::GetItemRectMax();
    ImDrawList* pdl = ImGui::GetWindowDrawList();
    pdl->AddRectFilled(cp0, cp1, surfaceU_, 6 * u);
    pdl->AddRect(cp0, cp1, accent_, 6 * u, 0, 2.0f);
    statusDot(pdl, ImVec2(cp0.x + 14 * u, cp0.y + cardSize.y * 0.5f), 4.5f * u, s);
    pdl->AddText(ImVec2(cp0.x + 24 * u, cp0.y + (cardSize.y - ImGui::GetFontSize()) * 0.5f), textU_, s.rec.name.c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CT_PANE_SESSION_ID", ImGuiDragDropFlags_AcceptBeforeDelivery);
    if (payload) {
      dl->AddRect(ImVec2(hp.x + 1, hp.y + 1), ImVec2(hp.x + size.x - 1, hp.y + size.y - 1), accent_, 6 * u, 0, 3.0f);
      if (payload->Delivery) {
        std::string draggedId(static_cast<const char*>(payload->Data), (size_t)payload->DataSize - 1);
        swapSessionOrder(draggedId, s.rec.id);
      }
    }
    ImGui::EndDragDropTarget();
  }

  ImGui::SetCursorPos(ImVec2(0, hh + 1));
  drawTerminal(s, ImVec2(size.x, ImGui::GetContentRegionAvail().y), isActive);

  ImGui::EndChild();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
}

bool App::drawBoxChar(ImDrawList* dl, float x, float y, float w, float h, uint32_t cp, ImU32 col) {
  // blocks and shades
  if (cp >= 0x2580 && cp <= 0x259F) {
    auto rect = [&](float x0, float y0, float x1, float y1, int alpha = 255) {
      ImU32 c = (col & 0x00FFFFFF) | ((ImU32)alpha << 24);
      dl->AddRectFilled(ImVec2(x + x0 * w, y + y0 * h), ImVec2(x + x1 * w, y + y1 * h), c);
    };
    if (cp == 0x2580) rect(0, 0, 1, 0.5f);
    else if (cp >= 0x2581 && cp <= 0x2587) rect(0, 1.0f - (float)(cp - 0x2580) / 8.0f, 1, 1);
    else if (cp == 0x2588) rect(0, 0, 1, 1);
    else if (cp >= 0x2589 && cp <= 0x258F) rect(0, 0, (float)(0x2590 - cp) / 8.0f, 1);
    else if (cp == 0x2590) rect(0.5f, 0, 1, 1);
    else if (cp == 0x2591) rect(0, 0, 1, 1, 64);
    else if (cp == 0x2592) rect(0, 0, 1, 1, 128);
    else if (cp == 0x2593) rect(0, 0, 1, 1, 192);
    else if (cp == 0x2594) rect(0, 0, 1, 0.125f);
    else if (cp == 0x2595) rect(0.875f, 0, 1, 1);
    else return false;  // quadrants fall back to the font
    return true;
  }

  int l, r, u, d;
  if (cp < 0x2500 || cp > 0x2577 || !boxArms(cp, l, r, u, d)) return false;
  float t = std::max(1.0f, std::round(monoPx_ / 14.0f));
  float cx = std::floor(x + w * 0.5f), cy = std::floor(y + h * 0.5f);
  auto thick = [&](int wgt) { return wgt == 2 ? t * 2 : t; };

  bool rounded = cp >= 0x256D && cp <= 0x2570;
  if (rounded) {
    float rad = std::min(w, h) * 0.5f;
    dl->PathClear();
    const float PI = 3.14159265f;
    switch (cp) {
      case 0x256D:  // down + right
        dl->PathLineTo(ImVec2(cx, y + h));
        dl->PathLineTo(ImVec2(cx, cy + rad));
        dl->PathArcTo(ImVec2(cx + rad, cy + rad), rad, PI, 1.5f * PI, 8);
        dl->PathLineTo(ImVec2(x + w, cy));
        break;
      case 0x256E:  // left + down
        dl->PathLineTo(ImVec2(x, cy));
        dl->PathLineTo(ImVec2(cx - rad, cy));
        dl->PathArcTo(ImVec2(cx - rad, cy + rad), rad, 1.5f * PI, 2.0f * PI, 8);
        dl->PathLineTo(ImVec2(cx, y + h));
        break;
      case 0x256F:  // up + left
        dl->PathLineTo(ImVec2(cx, y));
        dl->PathLineTo(ImVec2(cx, cy - rad));
        dl->PathArcTo(ImVec2(cx - rad, cy - rad), rad, 0.0f, 0.5f * PI, 8);
        dl->PathLineTo(ImVec2(x, cy));
        break;
      default:  // 0x2570: right + up
        dl->PathLineTo(ImVec2(x + w, cy));
        dl->PathLineTo(ImVec2(cx + rad, cy));
        dl->PathArcTo(ImVec2(cx + rad, cy - rad), rad, 0.5f * PI, PI, 8);
        dl->PathLineTo(ImVec2(cx, y));
        break;
    }
    dl->PathStroke(col, 0, t);
    return true;
  }

  auto hbar = [&](float x0, float x1, int wgt) {
    float th = thick(wgt);
    dl->AddRectFilled(ImVec2(x0, cy - std::floor(th * 0.5f)), ImVec2(x1, cy - std::floor(th * 0.5f) + th), col);
  };
  auto vbar = [&](float y0, float y1, int wgt) {
    float th = thick(wgt);
    dl->AddRectFilled(ImVec2(cx - std::floor(th * 0.5f), y0), ImVec2(cx - std::floor(th * 0.5f) + th, y1), col);
  };
  float jx = std::ceil(thick(std::max(u, d)) * 0.5f), jy = std::ceil(thick(std::max(l, r)) * 0.5f);
  if (l) hbar(x, cx + jx, l);
  if (r) hbar(cx - jx, x + w, r);
  if (u) vbar(y, cy + jy, u);
  if (d) vbar(cy - jy, y + h, d);
  return true;
}

void App::drawTerminal(Session& s, ImVec2 size, bool isActive) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float pad = 6 * uiScale_;
  const int cols = std::max(10, (int)((size.x - pad * 2) / cell_.x));
  const int rows = std::max(3, (int)((size.y - 2) / cell_.y));

  if (!s.attempted) {
    s.attempted = true;
    std::string err;
    if (!s.start(s_.settings.claudeCommand, cols, rows, now_, err)) toast("Could not launch Claude: " + err, true);
    markDirty();
  }
  if (s.term) s.resize(cols, rows);

  dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), terminalBgU_);

  ImGui::InvisibleButton("##term", size,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();

  if (!s.term) {
    dl->AddText(ImVec2(p0.x + pad, p0.y + pad), textDisabledU_, "Session not started.");
  } else {
    Terminal& t = *s.term;
    const int sb = t.scrollbackSize();
    // keep the view anchored while output scrolls into history
    long long delta = t.pushedTotal() - s.lastPushed;
    s.lastPushed = t.pushedTotal();
    if (s.scrollOffset > 0 && delta > 0) s.scrollOffset += (int)delta;
    s.scrollOffset = std::clamp(s.scrollOffset, 0, sb);
    const int viewTop = sb - s.scrollOffset;
    const bool reporting = t.mouseMode() > 0 && !io.KeyShift;

    auto cellAt = [&](ImVec2 m, int& row, int& col) {
      col = std::clamp((int)std::floor((m.x - (p0.x + pad)) / cell_.x), 0, t.cols() - 1);
      row = std::clamp((int)std::floor((m.y - p0.y) / cell_.y), 0, t.rows() - 1);
    };

    // ---- mouse ----
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      int r, c;
      cellAt(io.MousePos, r, c);
      if (reporting) {
        s.typing(now_, statusCb(), [&](Terminal& tm) {
          tm.mouseMove(r, c, VTERM_MOD_NONE);
          tm.mouseButton(1, true, VTERM_MOD_NONE);
        });
      } else {
        s.selecting = true;
        s.hasSel = false;
        s.selA_row = s.selB_row = t.stableRow(viewTop + r);
        s.selA_col = s.selB_col = c;
      }
    }
    if (s.selecting && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
      int r, c;
      cellAt(io.MousePos, r, c);
      s.selB_row = t.stableRow(viewTop + r);
      s.selB_col = c;
      s.hasSel = !(s.selA_row == s.selB_row && s.selA_col == s.selB_col);
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      if (s.selecting) s.selecting = false;
      else if (reporting && hovered) {
        int r, c;
        cellAt(io.MousePos, r, c);
        s.typing(now_, statusCb(), [&](Terminal& tm) {
          tm.mouseMove(r, c, VTERM_MOD_NONE);
          tm.mouseButton(1, false, VTERM_MOD_NONE);
        });
      }
    }
    if (hovered && !reporting && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      if (s.hasSel) copySelection(s);
      else paste(s);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) paste(s);
    if (hovered && io.MouseWheel != 0.0f && !io.KeyCtrl) {
      if (reporting) {
        int r, c;
        cellAt(io.MousePos, r, c);
        int steps = std::max(1, (int)std::abs(io.MouseWheel * 3));
        s.typing(now_, statusCb(), [&](Terminal& tm) {
          tm.mouseMove(r, c, VTERM_MOD_NONE);
          for (int i = 0; i < steps; i++) tm.mouseButton(io.MouseWheel > 0 ? 4 : 5, true, VTERM_MOD_NONE);
        });
      } else if (!t.altScreen()) {
        s.scrollOffset = std::clamp(s.scrollOffset + (int)std::round(io.MouseWheel * 3), 0, sb);
      }
    }
    if (hovered) ImGui::SetMouseCursor(reporting ? ImGuiMouseCursor_Arrow : ImGuiMouseCursor_TextInput);

    // ---- selection range (absolute rows) ----
    int sra = 0, sca = 0, srb = -1, scb = 0;
    if (s.hasSel) {
      sra = t.absRow(s.selA_row);
      srb = t.absRow(s.selB_row);
      sca = s.selA_col;
      scb = s.selB_col;
      if (sra > srb || (sra == srb && sca > scb)) {
        std::swap(sra, srb);
        std::swap(sca, scb);
      }
    }

    // ---- cells ----
    const float fs = mono_->FontSize;
    const float ox = p0.x + pad;
    const float textDy = (cell_.y - fs) * 0.5f;
    int crow = 0, ccol = 0;
    bool cvis = false;
    t.cursor(crow, ccol, cvis);
    std::vector<Cell> row((size_t)t.cols());
    std::string run;

    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    for (int r = 0; r < t.rows(); r++) {
      const int abs = viewTop + r;
      const float oy = p0.y + r * cell_.y;
      if (oy > p0.y + size.y) break;
      for (int c = 0; c < t.cols(); c++) row[(size_t)c] = t.cell(abs, c);

      // backgrounds (+ selection)
      for (int c = 0; c < t.cols();) {
        const Cell& cl = row[(size_t)c];
        int span = cl.width > 1 ? cl.width : 1;
        bool sel = s.hasSel && ((abs > sra && abs < srb) || (abs == sra && abs == srb && c >= sca && c <= scb) ||
                                (abs == sra && abs != srb && c >= sca) || (abs == srb && abs != sra && c <= scb));
        if (sel) {
          dl->AddRectFilled(ImVec2(ox + c * cell_.x, oy), ImVec2(ox + (c + span) * cell_.x, oy + cell_.y), selectionU_);
        } else if (!cl.defBg) {
          dl->AddRectFilled(ImVec2(ox + c * cell_.x, oy), ImVec2(ox + (c + span) * cell_.x, oy + cell_.y), rgbCol(cl.bg));
        }
        c += span;
      }

      // cursor block
      if (cvis && s.scrollOffset == 0 && r == crow && s.running()) {
        ImVec2 a(ox + ccol * cell_.x, oy), b(ox + (ccol + 1) * cell_.x, oy + cell_.y);
        if (isActive && focused_) dl->AddRectFilled(a, b, cursorFillU_);
        else dl->AddRect(a, b, cursorOutlineU_);
      }

      // text
      auto flush = [&](int startCol, uint32_t fg, bool bold) {
        while (!run.empty() && run.back() == ' ') run.pop_back();
        if (run.empty()) return;
        ImFont* f = (bold && monoBold_) ? monoBold_ : mono_;
        dl->AddText(f, fs, ImVec2(ox + startCol * cell_.x, oy + textDy), rgbCol(fg), run.c_str(), run.c_str() + run.size());
        run.clear();
      };
      int runStart = 0;
      uint32_t runFg = 0;
      bool runBold = false;
      for (int c = 0; c < t.cols(); c++) {
        const Cell& cl = row[(size_t)c];
        if (cl.ch == Cell::kContinuation) continue;
        const bool blank = cl.ch == 0 || cl.ch == ' ';
        if (cl.underline || cl.strike) {
          float x0 = ox + c * cell_.x, x1 = x0 + cell_.x * (cl.width > 1 ? cl.width : 1);
          if (cl.underline) dl->AddLine(ImVec2(x0, oy + cell_.y - 2), ImVec2(x1, oy + cell_.y - 2), rgbCol(cl.fg), 1.0f);
          if (cl.strike) dl->AddLine(ImVec2(x0, oy + cell_.y * 0.5f), ImVec2(x1, oy + cell_.y * 0.5f), rgbCol(cl.fg), 1.0f);
        }
        if (blank) {
          if (!run.empty()) run += ' ';
          continue;
        }
        if (cl.ch < 0x80) {
          if (!run.empty() && (cl.fg != runFg || cl.bold != runBold)) flush(runStart, runFg, runBold);
          if (run.empty()) {
            runStart = c;
            runFg = cl.fg;
            runBold = cl.bold;
          }
          run += (char)cl.ch;
          continue;
        }
        flush(runStart, runFg, runBold);
        float gx = ox + c * cell_.x;
        if (drawBoxChar(dl, gx, oy, cell_.x * (cl.width > 1 ? cl.width : 1), cell_.y, cl.ch, rgbCol(cl.fg))) continue;
        if (cl.ch > 0xFFFF) continue;  // outside the atlas (emoji)
        std::string g = util::utf8(cl.ch);
        dl->AddText((cl.bold && monoBold_) ? monoBold_ : mono_, fs, ImVec2(gx, oy + textDy), rgbCol(cl.fg), g.c_str(), g.c_str() + g.size());
      }
      flush(runStart, runFg, runBold);
    }

    // scrollback indicator
    if (s.scrollOffset > 0 && sb > 0) {
      float frac = (float)viewTop / (float)(sb + t.rows());
      float barH = std::max(20.0f, size.y * t.rows() / (float)(sb + t.rows()));
      float y0 = p0.y + frac * size.y;
      dl->AddRectFilled(ImVec2(p0.x + size.x - 5, y0), ImVec2(p0.x + size.x - 2, std::min(y0 + barH, p0.y + size.y)), withAlpha(textU_, 70), 2);
      const char* hint = "scrolled back - type to return";
      ImVec2 ts = ImGui::CalcTextSize(hint);
      dl->AddRectFilled(ImVec2(p0.x + size.x - ts.x - 22, p0.y + size.y - ts.y - 12), ImVec2(p0.x + size.x - 10, p0.y + size.y - 6), IM_COL32(0, 0, 0, 170), 4);
      dl->AddText(ImVec2(p0.x + size.x - ts.x - 16, p0.y + size.y - ts.y - 9), IM_COL32(0xC9, 0xCF, 0xDB, 255), hint);
    }
    dl->PopClipRect();
  }

  // ---- "not running" overlay ----
  if (s.status == Status::Exited && s.attempted) {
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(11, 13, 17, 215));
    const char* msg = "This session is not running.";
    ImVec2 ts = ImGui::CalcTextSize(msg);
    float bw = 170 * uiScale_;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + size.y * 0.5f - ts.y - 10));
    ImGui::TextDisabled("%s", msg);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + (size.x - bw) * 0.5f, p0.y + size.y * 0.5f + 4));
    ImGui::PushStyleColor(ImGuiCol_Button, accentV4_);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.06f, 0.04f, 1));
    ImGui::PushID(("resume" + s.rec.id).c_str());
    if (ImGui::Button("Resume session", ImVec2(bw, 0))) {
      std::string err;
      if (!s.start(s_.settings.claudeCommand, cols, rows, now_, err)) toast("Could not launch Claude: " + err, true);
      markDirty();
    }
    ImGui::PopID();
    ImGui::PopStyleColor(2);
  }
}

// ---------------------------------------------------------------------------------------------
// modals + toasts
// ---------------------------------------------------------------------------------------------

void App::drawModals() {
  if (modal_ == Modal::None) return;

  const char* title = "";
  switch (modal_) {
    case Modal::AddProject: title = "Add project"; break;
    case Modal::NewSession: title = "New Claude session"; break;
    case Modal::Rename: title = "Rename"; break;
    case Modal::Confirm: title = confirmTitle_.c_str(); break;
    case Modal::CloseSession: title = "Close session"; break;
    case Modal::Settings: title = "Settings"; break;
    case Modal::Usage: title = "Usage"; break;
    default: break;
  }
  std::string popupId = std::string(title) + "###modal";
  if (modalJustOpened_) {
    ImGui::OpenPopup(popupId.c_str());
    modalJustOpened_ = false;
    modalError_.clear();
    if (modal_ == Modal::NewSession) {
      nameBuf_[0] = 0;
      wantWorktree_ = false;
      projectCombo_ = 0;
      for (size_t i = 0; i < s_.projects.size(); i++)
        if (s_.projects[i].id == modalTarget_) projectCombo_ = (int)i;
      gitCache_ = GitCache{};
    }
    if (modal_ == Modal::Usage) startUsageFetch();
  }

  const float u = uiScale_;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(460 * u, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * u, 14 * u));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * u, 9 * u));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * u);
  bool open = true;
  if (ImGui::BeginPopupModal(popupId.c_str(), &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
    auto closeModal = [&] {
      ImGui::CloseCurrentPopup();
      modal_ = Modal::None;
    };
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) closeModal();
    const float full = -1;
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);

    switch (modal_) {
      case Modal::AddProject: {
        ImGui::TextDisabled("Project folder");
        ImGui::SetNextItemWidth(platform::canPickFolder() ? -80 * u : full);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##path", pathBuf_, sizeof pathBuf_);
        if (platform::canPickFolder()) {
          ImGui::SameLine();
          if (ImGui::Button("Browse...")) {
            std::string p = platform::pickFolder(win_);
            if (!p.empty()) std::snprintf(pathBuf_, sizeof pathBuf_, "%s", p.c_str());
          }
        }
        ImGui::TextDisabled("You can also drag a folder onto the window.");
        if (!modalError_.empty()) ImGui::TextColored(ImVec4(0.94f, 0.36f, 0.36f, 1), "%s", modalError_.c_str());
        if (ImGui::Button("Cancel")) closeModal();
        ImGui::SameLine();
        if (ImGui::Button("Add project") || enter) {
          std::string id;
          if (addProjectPath(pathBuf_, modalError_, &id)) {
            s_.ui.collapsed[id] = false;
            modalTarget_ = id;
            openModal(Modal::NewSession);
            ImGui::CloseCurrentPopup();
          }
        }
        break;
      }
      case Modal::NewSession: {
        if (s_.projects.empty()) {
          closeModal();
          break;
        }
        projectCombo_ = std::clamp(projectCombo_, 0, (int)s_.projects.size() - 1);
        ImGui::TextDisabled("Project");
        ImGui::SetNextItemWidth(full);
        if (ImGui::BeginCombo("##proj", s_.projects[(size_t)projectCombo_].name.c_str())) {
          for (int i = 0; i < (int)s_.projects.size(); i++)
            if (ImGui::Selectable(s_.projects[(size_t)i].name.c_str(), i == projectCombo_)) projectCombo_ = i;
          ImGui::EndCombo();
        }
        const Project& p = s_.projects[(size_t)projectCombo_];
        if (gitCache_.path != p.path) {
          git::Info gi = git::info(p.path);
          gitCache_ = {p.path, gi.isGit, gi.branch};
          if (!gi.isGit) wantWorktree_ = false;
        }
        ImGui::TextDisabled("Session name (optional)");
        ImGui::SetNextItemWidth(full);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##name", "e.g. Fix login bug", nameBuf_, sizeof nameBuf_);
        ImGui::BeginDisabled(!gitCache_.isGit);
        ImGui::Checkbox("Isolate in a new git worktree", &wantWorktree_);
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", gitCache_.isGit
                                      ? ("Branches from " + (gitCache_.branch.empty() ? std::string("HEAD") : gitCache_.branch) +
                                         " into .claude/worktrees/, so parallel sessions never touch each other's files.")
                                            .c_str()
                                      : "Not a git repository - worktrees unavailable.");
        ImGui::PopTextWrapPos();
        if (!modalError_.empty()) ImGui::TextColored(ImVec4(0.94f, 0.36f, 0.36f, 1), "%s", modalError_.c_str());
        if (ImGui::Button("Cancel")) closeModal();
        ImGui::SameLine();
        if (ImGui::Button("Start session") || enter) {
          std::string err;
          Session* ns = createSession(p.id, nameBuf_, wantWorktree_ && gitCache_.isGit, err);
          if (ns) {
            s_.ui.collapsed[p.id] = false;
            openSession(ns->rec.id);
            closeModal();
          } else {
            modalError_ = err;
          }
        }
        break;
      }
      case Modal::Rename: {
        ImGui::TextDisabled(renameIsProject_ ? "Project name" : "Session name");
        ImGui::SetNextItemWidth(full);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##rename", nameBuf_, sizeof nameBuf_);
        if (ImGui::Button("Cancel")) closeModal();
        ImGui::SameLine();
        if (ImGui::Button("Save") || enter) {
          std::string nm = util::trim(nameBuf_);
          if (!nm.empty()) {
            if (renameIsProject_) {
              if (Project* p = findProject(modalTarget_)) p->name = nm;
            } else if (Session* s = find(modalTarget_)) {
              s->rec.name = nm;
            }
            markDirty();
          }
          closeModal();
        }
        break;
      }
      case Modal::Confirm: {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(confirmText_.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Cancel")) closeModal();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.24f, 0.24f, 1));
        if (ImGui::Button("Confirm")) {
          auto yes = confirmYes_;
          closeModal();
          if (yes) yes();
        }
        ImGui::PopStyleColor();
        break;
      }
      case Modal::CloseSession: {
        Session* s = find(modalTarget_);
        if (!s || !s->rec.worktree) {
          closeModal();
          break;
        }
        ImGui::PushTextWrapPos(0);
        ImGui::Text("\"%s\" works in its own git worktree on branch %s.", s->rec.name.c_str(), s->rec.worktree->branch.c_str());
        if (closeDirty_) ImGui::TextColored(ImVec4(0.9f, 0.64f, 0.24f, 1), "The worktree has uncommitted changes.");
        ImGui::PopTextWrapPos();
        ImGui::RadioButton("Keep the worktree and branch", &closeChoice_, 0);
        ImGui::BeginDisabled(closeDirty_);
        ImGui::RadioButton("Remove the worktree (branch kept if it has unmerged commits)", &closeChoice_, 1);
        ImGui::EndDisabled();
        ImGui::RadioButton("Remove and discard all changes", &closeChoice_, 2);
        if (closeDirty_ && closeChoice_ == 1) closeChoice_ = 0;
        if (ImGui::Button("Cancel")) closeModal();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.24f, 0.24f, 1));
        if (ImGui::Button("Close session")) {
          std::string id = s->rec.id;
          int choice = closeChoice_;
          closeModal();
          closeSession(id, choice);
        }
        ImGui::PopStyleColor();
        break;
      }
      case Modal::Settings: {
        ImGui::TextDisabled("Claude command (add flags, e.g.  claude --model opus)");
        ImGui::SetNextItemWidth(full);
        ImGui::InputText("##cmd", cmdBuf_, sizeof cmdBuf_);
        ImGui::TextDisabled("Terminal font size");
        ImGui::SetNextItemWidth(full);
        ImGui::SliderInt("##fs", &fontSizeEdit_, 8, 32);
        ImGui::Checkbox("Desktop notifications when a background session needs you", &notifyEdit_);
        ImGui::TextDisabled("Command changes apply the next time a session starts.");

        ImGui::Separator();
        ImGui::TextUnformatted("Theme");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset to defaults")) themeEdit_ = ThemeColors{};
        ImGui::BeginChild("##themeColors", ImVec2(full, 260 * u), ImGuiChildFlags_Borders);
        auto row = [&](const char* label, ThemeColor& tc) {
          ImGui::ColorEdit4(label, &tc.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs);
        };
        row("Background", themeEdit_.background);
        row("Surface (sidebar/toolbar)", themeEdit_.surface);
        row("Pane background", themeEdit_.paneBg);
        row("Pane border", themeEdit_.paneBorder);
        row("Accent", themeEdit_.accent);
        row("Text", themeEdit_.text);
        row("Text (disabled)", themeEdit_.textDisabled);
        row("Status: busy", themeEdit_.statusBusy);
        row("Status: waiting", themeEdit_.statusWaiting);
        row("Status: done", themeEdit_.statusDone);
        row("Status: idle", themeEdit_.statusIdle);
        row("Terminal background", themeEdit_.terminalBg);
        row("Selection highlight", themeEdit_.selection);
        row("Cursor", themeEdit_.cursor);
        row("Title bar background", themeEdit_.titlebarBg);
        row("Title bar text", themeEdit_.titlebarText);
        ImGui::EndChild();

        if (ImGui::Button("Cancel")) closeModal();
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
          std::string cmd = util::trim(cmdBuf_);
          s_.settings.claudeCommand = cmd.empty() ? "claude" : cmd;
          s_.settings.notifications = notifyEdit_;
          if (fontSizeEdit_ != s_.settings.fontSize) {
            s_.settings.fontSize = fontSizeEdit_;
            fontsDirty_ = true;
          }
          s_.settings.theme = themeEdit_;
          applyStyle(uiScale_, s_.settings.theme);
          platform::applyTitlebarColors(win_, toRgbColor(s_.settings.theme.titlebarBg),
                                        toRgbColor(s_.settings.theme.titlebarText));
          markDirty();
          closeModal();
        }
        break;
      }
      case Modal::Usage: {
        bool inFlight = false, done = false;
        UsageInfo info;
        if (usageFetch_) {
          std::lock_guard<std::mutex> g(usageFetch_->mu);
          inFlight = usageFetch_->inFlight;
          done = usageFetch_->done;
          info = usageFetch_->result;
        }
        if (inFlight) {
          ImGui::TextDisabled("Fetching usage...");
        } else if (!done || !info.ok) {
          ImGui::PushTextWrapPos(0);
          ImGui::TextColored(waitingV4_, "%s", info.error.empty() ? "Could not fetch usage." : info.error.c_str());
          ImGui::PopTextWrapPos();
        } else {
          if (info.hasSession) {
            ImGui::Text("Session usage: %.0f%%", info.sessionPct);
            ImGui::ProgressBar((float)(info.sessionPct / 100.0), ImVec2(full, 0));
            if (!info.sessionResetsAt.empty()) ImGui::TextDisabled("Resets %s", info.sessionResetsAt.c_str());
            ImGui::Spacing();
          }
          if (info.hasWeekly) {
            ImGui::Text("Weekly usage: %.0f%%", info.weeklyPct);
            ImGui::ProgressBar((float)(info.weeklyPct / 100.0), ImVec2(full, 0));
            if (!info.weeklyResetsAt.empty()) ImGui::TextDisabled("Resets %s", info.weeklyResetsAt.c_str());
          }
          if (!info.hasSession && !info.hasWeekly) ImGui::TextDisabled("No usage data returned.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Refresh")) startUsageFetch();
        ImGui::SameLine();
        if (ImGui::Button("Close")) closeModal();
        break;
      }
      default: break;
    }
    ImGui::EndPopup();
  } else if (!open) {
    modal_ = Modal::None;
  }
  ImGui::PopStyleVar(3);
}

void App::drawToasts() {
  toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return t.until < now_; }), toasts_.end());
  if (toasts_.empty()) return;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  float y = vp->WorkPos.y + vp->WorkSize.y - 16 * uiScale_;
  int i = 0;
  for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it, ++i) {
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 16 * uiScale_, y), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowSize(ImVec2(380 * uiScale_, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, it->err ? waitingV4_ : accentV4_);
    ImGui::Begin(("##toast" + std::to_string(i)).c_str(), nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(it->text.c_str());
    ImGui::PopTextWrapPos();
    y -= ImGui::GetWindowHeight() + 8 * uiScale_;
    ImGui::End();
    ImGui::PopStyleColor();
  }
}
