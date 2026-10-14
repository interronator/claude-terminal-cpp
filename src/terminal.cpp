#include "terminal.h"

#include <algorithm>
#include <cstring>

#include "util.h"

namespace
{
inline uint32_t PackRgb(const VTermColor& Color)
{
    return ((uint32_t)Color.rgb.red << 16) | ((uint32_t)Color.rgb.green << 8) | Color.rgb.blue;
}
inline void SetRgb(VTermColor& Color, uint32_t Value)
{
    vterm_color_rgb(&Color, (Value >> 16) & 0xFF, (Value >> 8) & 0xFF, Value & 0xFF);
}

constexpr uint32_t DEFAULT_FG = 0xE6E8EE, DEFAULT_BG = 0x0B0D11;
const uint32_t PALETTE[16] = {0x1B1F2A, 0xEF5B5B, 0x4CC38A, 0xE6A23C, 0x6AA3FF, 0xC58AF9, 0x5CCFE6, 0xC9CFDB,
                              0x5B6577, 0xFF7B7B, 0x6FDCA8, 0xF3C36B, 0x8CB9FF, 0xD8A8FF, 0x86E0F0, 0xFFFFFF};
}  // namespace

Terminal::Terminal(int Rows, int Cols, OutputFn Out) : Out_(std::move(Out)), Rows_(Rows), Cols_(Cols)
{
    Vt_ = vterm_new(Rows, Cols);
    vterm_set_utf8(Vt_, 1);
    vterm_output_set_callback(Vt_, &Terminal::OutCallback, this);

    Screen_ = vterm_obtain_screen(Vt_);
    static const VTermScreenCallbacks CALLBACKS = {
        nullptr,  // damage (we read cells directly every frame)
        nullptr,  // moverect
        nullptr,  // movecursor
        &Terminal::SetProp,
        nullptr,  // bell
        &Terminal::OnResize,
        &Terminal::ScrollbackPush,
        &Terminal::ScrollbackPop,
        &Terminal::ScrollbackClear,
        nullptr,  // sb_pushline4
    };
    vterm_screen_set_callbacks(Screen_, &CALLBACKS, this);
    vterm_screen_enable_altscreen(Screen_, 1);

    VTermState* const State = vterm_obtain_state(Vt_);
    for (int Index = 0; Index < 16; Index++)
    {
        VTermColor Color;
        SetRgb(Color, PALETTE[Index]);
        vterm_state_set_palette_color(State, Index, &Color);
    }
    VTermColor Fg, Bg;
    SetRgb(Fg, DEFAULT_FG);
    SetRgb(Bg, DEFAULT_BG);
    vterm_screen_set_default_colors(Screen_, &Fg, &Bg);
    vterm_screen_reset(Screen_, 1);
}

Terminal::~Terminal()
{
    vterm_free(Vt_);
}

void Terminal::Feed(const char* Data, size_t Length)
{
    vterm_input_write(Vt_, Data, Length);
}

void Terminal::Resize(int Rows, int Cols)
{
    if (Rows < 1 || Cols < 2 || (Rows == Rows_ && Cols == Cols_))
    {
        return;
    }
    Rows_ = Rows;
    Cols_ = Cols;
    vterm_set_size(Vt_, Rows, Cols);
}

Cell Terminal::ConvertCell(const VTermScreenCell& ScreenCell) const
{
    Cell Out;
    Out.Ch = ScreenCell.chars[0] == (uint32_t)-1 ? Cell::CONTINUATION : ScreenCell.chars[0];
    Out.Width = ScreenCell.width != 0 ? (uint8_t)ScreenCell.width : 1;
    VTermColor Fg = ScreenCell.fg, Bg = ScreenCell.bg;
    bool const DefaultBg = VTERM_COLOR_IS_DEFAULT_BG(&Bg);
    vterm_screen_convert_color_to_rgb(Screen_, &Fg);
    vterm_screen_convert_color_to_rgb(Screen_, &Bg);
    Out.Fg = PackRgb(Fg);
    Out.Bg = PackRgb(Bg);
    Out.DefBg = DefaultBg;
    if (ScreenCell.attrs.reverse)
    {
        std::swap(Out.Fg, Out.Bg);
        Out.DefBg = false;
    }
    Out.Bold = ScreenCell.attrs.bold;
    Out.Italic = ScreenCell.attrs.italic;
    Out.Underline = ScreenCell.attrs.underline != 0;
    Out.Strike = ScreenCell.attrs.strike;
    if (ScreenCell.attrs.conceal)
    {
        Out.Ch = 0;
    }
    return Out;
}

Cell Terminal::GetCell(int AbsRow, int Col) const
{
    if (AbsRow < 0 || Col < 0)
    {
        return Cell{};
    }
    if (AbsRow < (int)Scrollback_.size())
    {
        const std::vector<Cell>& Line = Scrollback_[(size_t)AbsRow];
        return Col < (int)Line.size() ? Line[(size_t)Col] : Cell{};
    }
    int const Row = AbsRow - (int)Scrollback_.size();
    if (Row >= Rows_ || Col >= Cols_)
    {
        return Cell{};
    }
    VTermScreenCell ScreenCell;
    if (vterm_screen_get_cell(Screen_, VTermPos{Row, Col}, &ScreenCell) == 0)
    {
        return Cell{};
    }
    return ConvertCell(ScreenCell);
}

std::vector<uint32_t> Terminal::FoldQuery(const std::string& Text)
{
    std::vector<uint32_t> Folded;
    size_t Index = 0;
    while (Index < Text.size())
    {
        const unsigned char Lead = static_cast<unsigned char>(Text[Index]);
        int Extra = 0;
        uint32_t CodePoint = Lead;
        if (Lead >= 0xF0)
        {
            Extra = 3;
            CodePoint = Lead & 0x07;
        }
        else if (Lead >= 0xE0)
        {
            Extra = 2;
            CodePoint = Lead & 0x0F;
        }
        else if (Lead >= 0xC0)
        {
            Extra = 1;
            CodePoint = Lead & 0x1F;
        }
        Index++;
        for (int Continuation = 0; Continuation < Extra && Index < Text.size(); Continuation++, Index++)
        {
            CodePoint = (CodePoint << 6) | (static_cast<unsigned char>(Text[Index]) & 0x3F);
        }
        Folded.push_back(CodePoint < 128 ? static_cast<uint32_t>(std::tolower(static_cast<int>(CodePoint))) : CodePoint);
    }
    return Folded;
}

void Terminal::MatchColumns(const std::vector<Cell>& Cells, const std::vector<uint32_t>& Query, std::vector<int>& OutColumns)
{
    OutColumns.clear();
    const int CellCount = static_cast<int>(Cells.size());
    const int Length = static_cast<int>(Query.size());
    if (Length == 0)
    {
        return;
    }
    for (int Start = 0; Start + Length <= CellCount;)
    {
        bool Matches = true;
        for (int Offset = 0; Offset < Length; Offset++)
        {
            const uint32_t Ch = Cells[static_cast<size_t>(Start + Offset)].Ch;
            uint32_t Folded = Ch;
            if (Ch == 0)
            {
                Folded = ' ';
            }
            else if (Ch < 128)
            {
                Folded = static_cast<uint32_t>(std::tolower(static_cast<int>(Ch)));
            }
            if (Folded != Query[static_cast<size_t>(Offset)])
            {
                Matches = false;
                break;
            }
        }
        if (Matches == false)
        {
            Start++;
            continue;
        }
        OutColumns.push_back(Start);
        Start++;
    }
}

bool Terminal::Find(const std::vector<uint32_t>& Query, int FromRow, int FromCol, bool Forward, int& OutRow, int& OutCol) const
{
    if (Query.empty() == true)
    {
        return false;
    }
    const int TotalRows = ScrollbackSize() + Rows_;
    const int StartRow = std::clamp(FromRow, 0, TotalRows - 1);
    std::vector<Cell> RowCells(static_cast<size_t>(Cols_));
    std::vector<int> Columns;
    for (int Step = 0; Step <= TotalRows; Step++)
    {
        const int Offset = Forward ? Step : -Step;
        const int Row = ((StartRow + Offset) % TotalRows + TotalRows) % TotalRows;
        for (int Col = 0; Col < Cols_; Col++)
        {
            RowCells[static_cast<size_t>(Col)] = GetCell(Row, Col);
        }
        MatchColumns(RowCells, Query, Columns);
        const int ColumnCount = static_cast<int>(Columns.size());
        for (int Index = 0; Index < ColumnCount; Index++)
        {
            const int Column = Columns[static_cast<size_t>(Forward ? Index : ColumnCount - 1 - Index)];
            bool Accept = true;
            if (Step == 0)
            {
                Accept = Forward ? Column > FromCol : Column < FromCol;
            }
            else if (Step == TotalRows)
            {
                Accept = Forward ? Column <= FromCol : Column >= FromCol;
            }
            if (Accept == false)
            {
                continue;
            }
            OutRow = Row;
            OutCol = Column;
            return true;
        }
    }
    return false;
}

bool Terminal::FindInputText(int& FirstRow, int& FirstCol, int& LastRow, int& LastCol) const
{
    const int ScreenTop = ScrollbackSize();
    std::vector<int> RuleRows;
    for (int Row = Rows_ - 1; Row >= 0 && RuleRows.size() < 2; Row--)
    {
        int RuleCells = 0;
        int FilledCells = 0;
        for (int Col = 0; Col < Cols_; Col++)
        {
            const uint32_t Ch = GetCell(ScreenTop + Row, Col).Ch;
            if (Ch == 0 || Ch == ' ')
            {
                continue;
            }
            FilledCells++;
            if (Ch == 0x2500 || Ch == 0x2501 || Ch == 0x256D || Ch == 0x256E || Ch == 0x2570 || Ch == 0x256F)
            {
                RuleCells++;
            }
        }
        if (FilledCells > 0 && RuleCells * 10 >= FilledCells * 9 && RuleCells >= Cols_ / 2)
        {
            RuleRows.push_back(Row);
        }
    }
    if (RuleRows.size() < 2 || RuleRows[0] - RuleRows[1] < 2 || RuleRows[0] - RuleRows[1] > 14)
    {
        return false;
    }

    bool Found = false;
    for (int Row = RuleRows[1] + 1; Row < RuleRows[0]; Row++)
    {
        int Start = 0;
        while (Start < Cols_)
        {
            const uint32_t Ch = GetCell(ScreenTop + Row, Start).Ch;
            if (Ch != ' ' && Ch != 0 && Ch != 0x2502)
            {
                break;
            }
            Start++;
        }
        const uint32_t Marker = Start < Cols_ ? GetCell(ScreenTop + Row, Start).Ch : 0;
        if (Marker == '>' || Marker == 0x276F)
        {
            Start++;
            while (Start < Cols_ && (GetCell(ScreenTop + Row, Start).Ch == ' ' || GetCell(ScreenTop + Row, Start).Ch == 0))
            {
                Start++;
            }
        }
        int End = Cols_ - 1;
        while (End >= Start)
        {
            const uint32_t Ch = GetCell(ScreenTop + Row, End).Ch;
            if (Ch != ' ' && Ch != 0 && Ch != 0x2502)
            {
                break;
            }
            End--;
        }
        if (End < Start)
        {
            continue;
        }
        if (Found == false)
        {
            FirstRow = ScreenTop + Row;
            FirstCol = Start;
            Found = true;
        }
        LastRow = ScreenTop + Row;
        LastCol = End;
    }
    return Found;
}

void Terminal::GetCursor(int& Row, int& Col, bool& Visible) const
{
    VTermPos Pos;
    vterm_state_get_cursorpos(vterm_obtain_state(Vt_), &Pos);
    Row = Pos.row;
    Col = Pos.col;
    Visible = CursorVisible_;
}

std::string Terminal::GetText(int Row0, int Col0, int Row1, int Col1) const
{
    std::string Out;
    for (int Row = Row0; Row <= Row1; Row++)
    {
        int const StartCol = Row == Row0 ? Col0 : 0;
        int const EndCol = Row == Row1 ? Col1 : Cols_ - 1;
        std::string Line;
        for (int Col = StartCol; Col <= EndCol; Col++)
        {
            Cell const Current = GetCell(Row, Col);
            if (Current.Ch == Cell::CONTINUATION)
            {
                continue;
            }
            Line += Current.Ch != 0 ? Util::Utf8(Current.Ch) : " ";
        }
        while (Line.empty() == false && Line.back() == ' ')
        {
            Line.pop_back();
        }
        Out += Line;
        if (Row != Row1)
        {
            Out += '\n';
        }
    }
    return Out;
}

std::string Terminal::GetScreenText() const
{
    int const Base = (int)Scrollback_.size();
    return GetText(Base, 0, Base + Rows_ - 1, Cols_ - 1);
}

// ---- libvterm callbacks ------------------------------------------------------

void Terminal::OutCallback(const char* Text, size_t Length, void* User)
{
    Terminal* const Self = static_cast<Terminal*>(User);
    if (Self->Out_)
    {
        Self->Out_(Text, Length);
    }
}

int Terminal::ScrollbackPush(int Cols, const VTermScreenCell* Cells, void* User)
{
    Terminal* const Self = static_cast<Terminal*>(User);
    std::vector<Cell> Line((size_t)Cols);
    size_t UsedCols = 0;
    for (int Index = 0; Index < Cols; Index++)
    {
        const Cell Converted = Self->ConvertCell(Cells[Index]);
        Line[(size_t)Index] = Converted;
        const bool IsBlank = (Converted.Ch == 0 || Converted.Ch == ' ') && Converted.DefBg == true && Converted.Underline == false &&
                             Converted.Strike == false;
        if (IsBlank == false)
        {
            UsedCols = (size_t)Index + 1;
        }
    }
    Line.resize(UsedCols);
    Line.shrink_to_fit();
    Self->Scrollback_.push_back(std::move(Line));
    Self->Pushed_++;
    if (Self->Scrollback_.size() > MAX_SCROLLBACK)
    {
        Self->Scrollback_.pop_front();
        Self->Dropped_++;
    }
    return 1;
}

int Terminal::ScrollbackPop(int Cols, VTermScreenCell* Cells, void* User)
{
    Terminal* const Self = static_cast<Terminal*>(User);
    if (Self->Scrollback_.empty())
    {
        return 0;
    }
    std::vector<Cell> Line = std::move(Self->Scrollback_.back());
    Self->Scrollback_.pop_back();
    Self->Pushed_--;
    for (int Index = 0; Index < Cols; Index++)
    {
        VTermScreenCell& ScreenCell = Cells[Index];
        std::memset(&ScreenCell, 0, sizeof ScreenCell);
        Cell const Source = Index < (int)Line.size() ? Line[(size_t)Index] : Cell{};
        if (Source.Ch == Cell::CONTINUATION)
        {
            ScreenCell.chars[0] = (uint32_t)-1;
        }
        else
        {
            ScreenCell.chars[0] = Source.Ch;
        }
        ScreenCell.width = (char)Source.Width;
        SetRgb(ScreenCell.fg, Source.Fg);
        SetRgb(ScreenCell.bg, Source.Bg);
        if (Source.DefBg)
        {
            ScreenCell.bg.type |= VTERM_COLOR_DEFAULT_BG;
        }
        ScreenCell.attrs.bold = Source.Bold;
        ScreenCell.attrs.italic = Source.Italic;
        ScreenCell.attrs.underline = Source.Underline ? 1 : 0;
        ScreenCell.attrs.strike = Source.Strike;
    }
    return 1;
}

int Terminal::ScrollbackClear(void* User)
{
    Terminal* const Self = static_cast<Terminal*>(User);
    Self->Dropped_ += (long long)Self->Scrollback_.size();
    Self->Scrollback_.clear();
    return 1;
}

int Terminal::SetProp(VTermProp Prop, VTermValue* Value, void* User)
{
    Terminal* const Self = static_cast<Terminal*>(User);
    switch (Prop)
    {
        case VTERM_PROP_CURSORVISIBLE:
            Self->CursorVisible_ = Value->boolean != 0;
            break;
        case VTERM_PROP_ALTSCREEN:
            Self->Alt_ = Value->boolean != 0;
            break;
        case VTERM_PROP_MOUSE:
            Self->MouseMode_ = Value->number;
            break;
        default:
            break;
    }
    return 1;
}

int Terminal::OnResize(int, int, void*)
{
    return 1;
}
