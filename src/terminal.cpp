#include "terminal.h"

#include <algorithm>
#include <cstring>

#include "util.h"

namespace {
inline uint32_t packRgb(const VTermColor& c) {
  return ((uint32_t)c.rgb.red << 16) | ((uint32_t)c.rgb.green << 8) | c.rgb.blue;
}
inline void setRgb(VTermColor& c, uint32_t v) { vterm_color_rgb(&c, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

constexpr uint32_t kDefaultFg = 0xE6E8EE, kDefaultBg = 0x0B0D11;
const uint32_t kPalette[16] = {0x1B1F2A, 0xEF5B5B, 0x4CC38A, 0xE6A23C, 0x6AA3FF, 0xC58AF9, 0x5CCFE6, 0xC9CFDB,
                               0x5B6577, 0xFF7B7B, 0x6FDCA8, 0xF3C36B, 0x8CB9FF, 0xD8A8FF, 0x86E0F0, 0xFFFFFF};
}  // namespace

Terminal::Terminal(int rows, int cols, OutputFn out) : out_(std::move(out)), rows_(rows), cols_(cols) {
  vt_ = vterm_new(rows, cols);
  vterm_set_utf8(vt_, 1);
  vterm_output_set_callback(vt_, &Terminal::outCb, this);

  screen_ = vterm_obtain_screen(vt_);
  static const VTermScreenCallbacks callbacks = {
      nullptr,           // damage (we read cells directly every frame)
      nullptr,           // moverect
      nullptr,           // movecursor
      &Terminal::setProp,
      nullptr,           // bell
      &Terminal::onResize,
      &Terminal::sbPush,
      &Terminal::sbPop,
      &Terminal::sbClear,
      nullptr,           // sb_pushline4
  };
  vterm_screen_set_callbacks(screen_, &callbacks, this);
  vterm_screen_enable_altscreen(screen_, 1);

  VTermState* state = vterm_obtain_state(vt_);
  for (int i = 0; i < 16; i++) {
    VTermColor c;
    setRgb(c, kPalette[i]);
    vterm_state_set_palette_color(state, i, &c);
  }
  VTermColor fg, bg;
  setRgb(fg, kDefaultFg);
  setRgb(bg, kDefaultBg);
  vterm_screen_set_default_colors(screen_, &fg, &bg);
  vterm_screen_reset(screen_, 1);
}

Terminal::~Terminal() { vterm_free(vt_); }

void Terminal::feed(const char* data, size_t n) { vterm_input_write(vt_, data, n); }

void Terminal::resize(int rows, int cols) {
  if (rows < 1 || cols < 2 || (rows == rows_ && cols == cols_)) return;
  rows_ = rows;
  cols_ = cols;
  vterm_set_size(vt_, rows, cols);
}

Cell Terminal::convert(const VTermScreenCell& vc) const {
  Cell c;
  c.ch = vc.chars[0] == (uint32_t)-1 ? Cell::kContinuation : vc.chars[0];
  c.width = vc.width ? (uint8_t)vc.width : 1;
  VTermColor fg = vc.fg, bg = vc.bg;
  bool defBg = VTERM_COLOR_IS_DEFAULT_BG(&bg);
  vterm_screen_convert_color_to_rgb(screen_, &fg);
  vterm_screen_convert_color_to_rgb(screen_, &bg);
  c.fg = packRgb(fg);
  c.bg = packRgb(bg);
  c.defBg = defBg;
  if (vc.attrs.reverse) {
    std::swap(c.fg, c.bg);
    c.defBg = false;
  }
  c.bold = vc.attrs.bold;
  c.italic = vc.attrs.italic;
  c.underline = vc.attrs.underline != 0;
  c.strike = vc.attrs.strike;
  if (vc.attrs.conceal) c.ch = 0;
  return c;
}

Cell Terminal::cell(int absRow, int col) const {
  if (absRow < 0 || col < 0) return Cell{};
  if (absRow < (int)sb_.size()) {
    const auto& line = sb_[(size_t)absRow];
    return col < (int)line.size() ? line[(size_t)col] : Cell{};
  }
  int r = absRow - (int)sb_.size();
  if (r >= rows_ || col >= cols_) return Cell{};
  VTermScreenCell vc;
  if (!vterm_screen_get_cell(screen_, VTermPos{r, col}, &vc)) return Cell{};
  return convert(vc);
}

void Terminal::cursor(int& row, int& col, bool& visible) const {
  VTermPos p;
  vterm_state_get_cursorpos(vterm_obtain_state(vt_), &p);
  row = p.row;
  col = p.col;
  visible = cursorVisible_;
}

std::string Terminal::text(int row0, int col0, int row1, int col1) const {
  std::string out;
  for (int r = row0; r <= row1; r++) {
    int c0 = r == row0 ? col0 : 0;
    int c1 = r == row1 ? col1 : cols_ - 1;
    std::string line;
    for (int c = c0; c <= c1; c++) {
      Cell cl = cell(r, c);
      if (cl.ch == Cell::kContinuation) continue;
      line += cl.ch ? util::utf8(cl.ch) : " ";
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    out += line;
    if (r != row1) out += '\n';
  }
  return out;
}

std::string Terminal::screenText() const {
  int base = (int)sb_.size();
  return text(base, 0, base + rows_ - 1, cols_ - 1);
}

// ---- libvterm callbacks ------------------------------------------------------

void Terminal::outCb(const char* s, size_t len, void* user) {
  auto* t = static_cast<Terminal*>(user);
  if (t->out_) t->out_(s, len);
}

int Terminal::sbPush(int cols, const VTermScreenCell* cells, void* user) {
  auto* t = static_cast<Terminal*>(user);
  std::vector<Cell> line((size_t)cols);
  for (int i = 0; i < cols; i++) line[(size_t)i] = t->convert(cells[i]);
  t->sb_.push_back(std::move(line));
  t->pushed_++;
  if (t->sb_.size() > kMaxScrollback) {
    t->sb_.pop_front();
    t->dropped_++;
  }
  return 1;
}

int Terminal::sbPop(int cols, VTermScreenCell* cells, void* user) {
  auto* t = static_cast<Terminal*>(user);
  if (t->sb_.empty()) return 0;
  std::vector<Cell> line = std::move(t->sb_.back());
  t->sb_.pop_back();
  t->pushed_--;
  for (int i = 0; i < cols; i++) {
    VTermScreenCell& vc = cells[i];
    std::memset(&vc, 0, sizeof vc);
    Cell c = i < (int)line.size() ? line[(size_t)i] : Cell{};
    if (c.ch == Cell::kContinuation) vc.chars[0] = (uint32_t)-1;
    else vc.chars[0] = c.ch;
    vc.width = (char)c.width;
    setRgb(vc.fg, c.fg);
    setRgb(vc.bg, c.bg);
    if (c.defBg) vc.bg.type |= VTERM_COLOR_DEFAULT_BG;
    vc.attrs.bold = c.bold;
    vc.attrs.italic = c.italic;
    vc.attrs.underline = c.underline ? 1 : 0;
    vc.attrs.strike = c.strike;
  }
  return 1;
}

int Terminal::sbClear(void* user) {
  auto* t = static_cast<Terminal*>(user);
  t->dropped_ += (long long)t->sb_.size();
  t->sb_.clear();
  return 1;
}

int Terminal::setProp(VTermProp prop, VTermValue* val, void* user) {
  auto* t = static_cast<Terminal*>(user);
  switch (prop) {
    case VTERM_PROP_CURSORVISIBLE: t->cursorVisible_ = val->boolean != 0; break;
    case VTERM_PROP_ALTSCREEN: t->alt_ = val->boolean != 0; break;
    case VTERM_PROP_MOUSE: t->mouseMode_ = val->number; break;
    default: break;
  }
  return 1;
}

int Terminal::onResize(int, int, void*) { return 1; }
