#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <vterm.h>

// One screen cell with colors already resolved to RGB (0xRRGGBB).
struct Cell {
  uint32_t ch = 0;  // 0 = empty, kContinuation = right half of a wide glyph
  uint32_t fg = 0xE6E8EE, bg = 0x0B0D11;
  bool bold = false, italic = false, underline = false, strike = false;
  bool defBg = true;  // background is the terminal default (no need to paint it)
  uint8_t width = 1;
  static constexpr uint32_t kContinuation = 0xFFFFFFFFu;
};

// libvterm-backed terminal: parses output, holds the screen and a scrollback buffer.
// Not thread-safe; use from the UI thread only.
class Terminal {
 public:
  using OutputFn = std::function<void(const char*, size_t)>;  // bytes to send to the child (replies, keys)
  Terminal(int rows, int cols, OutputFn out);
  ~Terminal();
  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;

  void feed(const char* data, size_t n);
  void resize(int rows, int cols);
  int rows() const { return rows_; }
  int cols() const { return cols_; }

  // Rows are addressed "absolutely": 0..scrollbackSize()-1 is history, then the live screen.
  int scrollbackSize() const { return (int)sb_.size(); }
  Cell cell(int absRow, int col) const;
  // Stable row ids don't move when history scrolls; used to keep selections attached to text.
  long long stableRow(int absRow) const { return dropped_ + absRow; }
  int absRow(long long stable) const { return (int)(stable - dropped_); }
  long long pushedTotal() const { return pushed_; }

  void cursor(int& row, int& col, bool& visible) const;
  int mouseMode() const { return mouseMode_; }
  bool altScreen() const { return alt_; }

  void key(VTermKey k, VTermModifier m) { vterm_keyboard_key(vt_, k, m); }
  void unichar(uint32_t c, VTermModifier m) { vterm_keyboard_unichar(vt_, c, m); }
  void startPaste() { vterm_keyboard_start_paste(vt_); }
  void endPaste() { vterm_keyboard_end_paste(vt_); }
  void mouseMove(int row, int col, VTermModifier m) { vterm_mouse_move(vt_, row, col, m); }
  void mouseButton(int button, bool pressed, VTermModifier m) { vterm_mouse_button(vt_, button, pressed, m); }

  // Text between two absolute positions (inclusive), trailing spaces trimmed per line.
  std::string text(int row0, int col0, int row1, int col1) const;
  // Visible screen text, one line per row.
  std::string screenText() const;

 private:
  static void outCb(const char* s, size_t len, void* user);
  static int sbPush(int cols, const VTermScreenCell* cells, void* user);
  static int sbPop(int cols, VTermScreenCell* cells, void* user);
  static int sbClear(void* user);
  static int setProp(VTermProp prop, VTermValue* val, void* user);
  static int onResize(int rows, int cols, void* user);
  Cell convert(const VTermScreenCell& vc) const;

  VTerm* vt_ = nullptr;
  VTermScreen* screen_ = nullptr;
  OutputFn out_;
  int rows_, cols_;
  std::deque<std::vector<Cell>> sb_;
  long long dropped_ = 0, pushed_ = 0;
  int mouseMode_ = 0;
  bool cursorVisible_ = true, alt_ = false;
  static constexpr size_t kMaxScrollback = 5000;
};
