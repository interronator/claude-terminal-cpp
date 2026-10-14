#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <vterm.h>

// One screen cell with colors already resolved to RGB (0xRRGGBB).
struct Cell
{
    uint32_t Ch = 0;  // 0 = empty, CONTINUATION = right half of a wide glyph
    uint32_t Fg = 0xE6E8EE, Bg = 0x0B0D11;
    bool Bold = false, Italic = false, Underline = false, Strike = false;
    bool DefBg = true;  // background is the terminal default (no need to paint it)
    uint8_t Width = 1;
    static constexpr uint32_t CONTINUATION = 0xFFFFFFFFu;
};

// libvterm-backed terminal: parses output, holds the screen and a scrollback buffer.
// Not thread-safe; use from the UI thread only.
class Terminal
{
public:
    using OutputFn = std::function<void(const char*, size_t)>;  // bytes to send to the child (replies, keys)
    Terminal(int Rows, int Cols, OutputFn Out);
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    void Feed(const char* Data, size_t Length);
    void Resize(int Rows, int Cols);
    int Rows() const { return Rows_; }
    int Cols() const { return Cols_; }

    // Rows are addressed "absolutely": 0..ScrollbackSize()-1 is history, then the live screen.
    int ScrollbackSize() const { return (int)Scrollback_.size(); }
    Cell GetCell(int AbsRow, int Col) const;
    // Stable row ids don't move when history scrolls; used to keep selections attached to text.
    long long StableRow(int AbsRow) const { return Dropped_ + AbsRow; }
    int AbsRow(long long Stable) const { return (int)(Stable - Dropped_); }
    long long PushedTotal() const { return Pushed_; }

    void GetCursor(int& Row, int& Col, bool& Visible) const;
    int MouseMode() const { return MouseMode_; }
    bool AltScreen() const { return Alt_; }

    void SendKey(VTermKey CurrentKey, VTermModifier Mod) { vterm_keyboard_key(Vt_, CurrentKey, Mod); }
    void SendUnichar(uint32_t Character, VTermModifier Mod) { vterm_keyboard_unichar(Vt_, Character, Mod); }
    void StartPaste() { vterm_keyboard_start_paste(Vt_); }
    void EndPaste() { vterm_keyboard_end_paste(Vt_); }
    void SendMouseMove(int Row, int Col, VTermModifier Mod) { vterm_mouse_move(Vt_, Row, Col, Mod); }
    void SendMouseButton(int Button, bool Pressed, VTermModifier Mod) { vterm_mouse_button(Vt_, Button, Pressed, Mod); }

    // Text between two absolute positions (inclusive), trailing spaces trimmed per line.
    std::string GetText(int Row0, int Col0, int Row1, int Col1) const;
    // Visible screen text, one line per row.
    std::string GetScreenText() const;

    // Case-insensitive search. Queries are folded once with FoldQuery and matched against cell rows.
    static std::vector<uint32_t> FoldQuery(const std::string& Text);
    // Start columns of the non-overlapping matches of Query in Cells, ascending.
    static void MatchColumns(const std::vector<Cell>& Cells, const std::vector<uint32_t>& Query, std::vector<int>& OutColumns);
    // Next/previous match strictly after/before (FromRow, FromCol), wrapping around the whole history.
    bool Find(const std::vector<uint32_t>& Query, int FromRow, int FromCol, bool Forward, int& OutRow, int& OutCol) const;
    // Locates the text inside Claude's prompt box on the live screen: the rows between the last two horizontal
    // rules, minus the box borders and the "> " marker. Absolute rows/columns, inclusive. False if no box has text.
    bool FindInputText(int& FirstRow, int& FirstCol, int& LastRow, int& LastCol) const;

private:
    static void OutCallback(const char* Text, size_t Length, void* User);
    static int ScrollbackPush(int Cols, const VTermScreenCell* Cells, void* User);
    static int ScrollbackPop(int Cols, VTermScreenCell* Cells, void* User);
    static int ScrollbackClear(void* User);
    static int SetProp(VTermProp Prop, VTermValue* Value, void* User);
    static int OnResize(int Rows, int Cols, void* User);
    Cell ConvertCell(const VTermScreenCell& CurrentCell) const;

    VTerm* Vt_ = nullptr;
    VTermScreen* Screen_ = nullptr;
    OutputFn Out_;
    int Rows_, Cols_;
    std::deque<std::vector<Cell>> Scrollback_;
    long long Dropped_ = 0, Pushed_ = 0;
    int MouseMode_ = 0;
    bool CursorVisible_ = true, Alt_ = false;
    static constexpr size_t MAX_SCROLLBACK = 5000;
};
