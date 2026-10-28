#include "App.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
#include <thread>

#include "Git.h"
#include "BuildScripts.h"
#include "ImageCache.h"
#include "Platform.h"
#include "Style.h"
#include "UiHelpers.h"
#include "Util.h"

namespace fs = std::filesystem;


bool App::DrawBoxChar(ImDrawList* DrawList, float CellX, float CellY, float CellWidth, float CellHeight, uint32_t CodePoint, ImU32 CurrentColor)
{
    // blocks and shades
    if (CodePoint >= 0x2580 && CodePoint <= 0x259F)
    {
        const std::function<void(float, float, float, float, int)> RectWithAlpha = [&](float X0, float Y0, float X1, float Y1, int Alpha)
        {
            ImU32 const RectColor = (CurrentColor & 0x00FFFFFF) | ((ImU32)Alpha << 24);
            DrawList->AddRectFilled(ImVec2(CellX + X0 * CellWidth, CellY + Y0 * CellHeight), ImVec2(CellX + X1 * CellWidth, CellY + Y1 * CellHeight), RectColor);
        };
        const std::function<void(float, float, float, float)> Rect = [&](float X0, float Y0, float X1, float Y1) { RectWithAlpha(X0, Y0, X1, Y1, 255); };
        if (CodePoint == 0x2580)
        {
            Rect(0, 0, 1, 0.5f);
        }
        else if (CodePoint >= 0x2581 && CodePoint <= 0x2587)
        {
            Rect(0, 1.0f - (float)(CodePoint - 0x2580) / 8.0f, 1, 1);
        }
        else if (CodePoint == 0x2588)
        {
            Rect(0, 0, 1, 1);
        }
        else if (CodePoint >= 0x2589 && CodePoint <= 0x258F)
        {
            Rect(0, 0, (float)(0x2590 - CodePoint) / 8.0f, 1);
        }
        else if (CodePoint == 0x2590)
        {
            Rect(0.5f, 0, 1, 1);
        }
        else if (CodePoint == 0x2591)
        {
            RectWithAlpha(0, 0, 1, 1, 64);
        }
        else if (CodePoint == 0x2592)
        {
            RectWithAlpha(0, 0, 1, 1, 128);
        }
        else if (CodePoint == 0x2593)
        {
            RectWithAlpha(0, 0, 1, 1, 192);
        }
        else if (CodePoint == 0x2594)
        {
            Rect(0, 0, 1, 0.125f);
        }
        else if (CodePoint == 0x2595)
        {
            Rect(0.875f, 0, 1, 1);
        }
        else
        {
            return false;  // quadrants fall back to the font
        }
        return true;
    }

    int Left, Right, Up, Down;
    if (CodePoint < 0x2500 || CodePoint > 0x2577 || UiHelpers::BoxArms(CodePoint, Left, Right, Up, Down) == false)
    {
        return false;
    }
    float Thickness = std::max(1.0f, std::round(MonoFontPx_ / 14.0f));
    float CenterX = std::floor(CellX + CellWidth * 0.5f), CenterY = std::floor(CellY + CellHeight * 0.5f);
    const std::function<float(int)> Thick = [&](int Weight) { return Weight == 2 ? Thickness * 2 : Thickness; };

    bool const Rounded = CodePoint >= 0x256D && CodePoint <= 0x2570;
    if (Rounded)
    {
        float const Radius = std::min(CellWidth, CellHeight) * 0.5f;
        DrawList->PathClear();
        const float Pi = 3.14159265f;
        switch (CodePoint)
        {
            case 0x256D:  // down + right
                DrawList->PathLineTo(ImVec2(CenterX, CellY + CellHeight));
                DrawList->PathLineTo(ImVec2(CenterX, CenterY + Radius));
                DrawList->PathArcTo(ImVec2(CenterX + Radius, CenterY + Radius), Radius, Pi, 1.5f * Pi, 8);
                DrawList->PathLineTo(ImVec2(CellX + CellWidth, CenterY));
                break;
            case 0x256E:  // left + down
                DrawList->PathLineTo(ImVec2(CellX, CenterY));
                DrawList->PathLineTo(ImVec2(CenterX - Radius, CenterY));
                DrawList->PathArcTo(ImVec2(CenterX - Radius, CenterY + Radius), Radius, 1.5f * Pi, 2.0f * Pi, 8);
                DrawList->PathLineTo(ImVec2(CenterX, CellY + CellHeight));
                break;
            case 0x256F:  // up + left
                DrawList->PathLineTo(ImVec2(CenterX, CellY));
                DrawList->PathLineTo(ImVec2(CenterX, CenterY - Radius));
                DrawList->PathArcTo(ImVec2(CenterX - Radius, CenterY - Radius), Radius, 0.0f, 0.5f * Pi, 8);
                DrawList->PathLineTo(ImVec2(CellX, CenterY));
                break;
            default:  // 0x2570: right + up
                DrawList->PathLineTo(ImVec2(CellX + CellWidth, CenterY));
                DrawList->PathLineTo(ImVec2(CenterX + Radius, CenterY));
                DrawList->PathArcTo(ImVec2(CenterX + Radius, CenterY - Radius), Radius, 0.5f * Pi, Pi, 8);
                DrawList->PathLineTo(ImVec2(CenterX, CellY));
                break;
        }
        DrawList->PathStroke(CurrentColor, 0, Thickness);
        return true;
    }

    const std::function<void(float, float, int)> HorizontalBar = [&](float X0, float X1, int Weight)
    {
        float const BarThickness = Thick(Weight);
        DrawList->AddRectFilled(ImVec2(X0, CenterY - std::floor(BarThickness * 0.5f)),
                                ImVec2(X1, CenterY - std::floor(BarThickness * 0.5f) + BarThickness), CurrentColor);
    };
    const std::function<void(float, float, int)> VerticalBar = [&](float Y0, float Y1, int Weight)
    {
        float const BarThickness = Thick(Weight);
        DrawList->AddRectFilled(ImVec2(CenterX - std::floor(BarThickness * 0.5f), Y0),
                                ImVec2(CenterX - std::floor(BarThickness * 0.5f) + BarThickness, Y1), CurrentColor);
    };
    float JoinX = std::ceil(Thick(std::max(Up, Down)) * 0.5f), JoinY = std::ceil(Thick(std::max(Left, Right)) * 0.5f);
    if (Left != 0)
    {
        HorizontalBar(CellX, CenterX + JoinX, Left);
    }
    if (Right != 0)
    {
        HorizontalBar(CenterX - JoinX, CellX + CellWidth, Right);
    }
    if (Up != 0)
    {
        VerticalBar(CellY, CenterY + JoinY, Up);
    }
    if (Down != 0)
    {
        VerticalBar(CenterY - JoinY, CellY + CellHeight, Down);
    }
    return true;
}

void App::RunFind(Session& Target, bool Forward, bool KeepCurrent)
{
    Terminal const& Term = *Target.Term;
    const std::vector<uint32_t> Query = Terminal::FoldQuery(Target.FindBuffer);
    Target.FindNoMatch = false;
    if (Query.empty() == true)
    {
        Target.FindHasMatch = false;
        return;
    }

    int FromRow = Term.ScrollbackSize() - Target.ScrollOffset;
    int FromCol = Forward ? -1 : Term.Cols();
    if (Target.FindHasMatch == true)
    {
        FromRow = Term.AbsRow(Target.FindStableRow);
        FromCol = KeepCurrent ? Target.FindCol - 1 : Target.FindCol;
    }

    int MatchRow = 0;
    int MatchCol = 0;
    if (Term.Find(Query, FromRow, FromCol, Forward, MatchRow, MatchCol) == false)
    {
        Target.FindHasMatch = false;
        Target.FindNoMatch = true;
        return;
    }
    Target.FindHasMatch = true;
    Target.FindStableRow = Term.StableRow(MatchRow);
    Target.FindCol = MatchCol;

    const int ViewTop = Term.ScrollbackSize() - Target.ScrollOffset;
    if (MatchRow < ViewTop || MatchRow >= ViewTop + Term.Rows())
    {
        Target.ScrollOffset = std::clamp(Term.ScrollbackSize() - (MatchRow - Term.Rows() / 2), 0, Term.ScrollbackSize());
    }
}

void App::DrawFindBar(Session& Target, ImVec2 PanelPos, ImVec2 PanelSize)
{
    ImGuiIO const& Io = ImGui::GetIO();
    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    const float Scale = UiScale_;
    const float ButtonSize = ImGui::GetFrameHeight();
    const float Gap = 2.0f * Scale;
    const float Pad = 4.0f * Scale;
    const float BarWidth = std::min(PanelSize.x - 24.0f * Scale, 360.0f * Scale);
    const ImVec2 BarMin(PanelPos.x + PanelSize.x - BarWidth - 16.0f * Scale, PanelPos.y + 6.0f * Scale);
    const ImVec2 BarMax(BarMin.x + BarWidth, BarMin.y + ButtonSize + Pad * 2.0f);

    DrawList->AddRectFilled(BarMin, BarMax, PaneBgColorU32_, 4.0f * Scale);
    DrawList->AddRect(BarMin, BarMax, PaneBorderColorU32_, 4.0f * Scale);
    if (Target.FindNoMatch == true)
    {
        const char* const Message = "No matches";
        const ImVec2 MessageSize = ImGui::CalcTextSize(Message);
        DrawList->AddText(ImVec2(BarMin.x - MessageSize.x - 8.0f * Scale, BarMin.y + (BarMax.y - BarMin.y - MessageSize.y) * 0.5f),
                          TextDisabledColorU32_, Message);
    }

    const ImVec2 SavedCursor = ImGui::GetCursorScreenPos();
    ImGui::PushID(Target.Rec.Id.c_str());
    ImGui::PushID("find");
    ImGui::SetCursorScreenPos(ImVec2(BarMin.x + Pad, BarMin.y + Pad));
    ImGui::SetNextItemWidth(BarWidth - Pad * 2.0f - (ButtonSize + Gap) * 3.0f);
    if (Target.FindFocus == true)
    {
        ImGui::SetKeyboardFocusHere();
        Target.FindFocus = false;
    }
    const bool Edited = ImGui::InputText("##query", Target.FindBuffer, sizeof Target.FindBuffer, ImGuiInputTextFlags_AutoSelectAll);
    const bool InputActive = ImGui::IsItemActive();
    const bool InputFinished = InputActive == true || ImGui::IsItemDeactivated() == true;

    bool Close = false;
    bool GoNext = false;
    bool GoPrev = false;
    if (InputActive == true && ImGui::IsKeyPressed(ImGuiKey_Enter) == true)
    {
        GoNext = Io.KeyShift == false;
        GoPrev = Io.KeyShift;
    }
    if (InputFinished == true && ImGui::IsKeyPressed(ImGuiKey_Escape) == true)
    {
        Close = true;
    }
    if (InputActive == true && Io.KeyCtrl == true && ImGui::IsKeyPressed(ImGuiKey_F) == true)
    {
        Target.FindFocus = true;
    }

    ImGui::SameLine(0, Gap);
    GoPrev = ImGui::Button("^", ImVec2(ButtonSize, ButtonSize)) || GoPrev;
    ImGui::SameLine(0, Gap);
    GoNext = ImGui::Button("v", ImVec2(ButtonSize, ButtonSize)) || GoNext;
    ImGui::SameLine(0, Gap);
    Close = ImGui::Button("x", ImVec2(ButtonSize, ButtonSize)) || Close;
    ImGui::PopID();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(SavedCursor);

    if (Close == true)
    {
        Target.FindOpen = false;
        Target.FindHasMatch = false;
        Target.FindNoMatch = false;
        return;
    }
    if (Edited == true)
    {
        RunFind(Target, true, true);
        return;
    }
    if (GoNext == true || GoPrev == true)
    {
        RunFind(Target, GoNext, false);
    }
}

void App::DrawTerminal(Session& CurrentSession, ImVec2 Size, bool IsActive)
{
    ImGuiIO& Io = ImGui::GetIO();
    ImDrawList* DrawList = ImGui::GetWindowDrawList();
    ImVec2 PanelPos = ImGui::GetCursorScreenPos();
    const float Padding = 6 * UiScale_;
    const int Cols = std::max(10, (int)((Size.x - Padding * 2) / CellSize_.x));
    const int Rows = std::max(3, (int)((Size.y - 2) / CellSize_.y));

    if (CurrentSession.Attempted == false)
    {
        CurrentSession.Attempted = true;
        std::string Err;
        if (StartSession(CurrentSession, Cols, Rows, Err) == false)
        {
            ShowToast("Could not launch Claude: " + Err, true);
        }
        MarkDirty();
    }
    if (CurrentSession.Term)
    {
        CurrentSession.RequestResize(Cols, Rows, Now_);
    }

    DrawList->AddRectFilled(PanelPos, ImVec2(PanelPos.x + Size.x, PanelPos.y + Size.y), TerminalBgColorU32_);

    // Submitted before the terminal button: the first-submitted item under the mouse claims the press,
    // so the scrollbar must come first or the terminal would start a text selection instead.
    const bool ShowScrollbar = CurrentSession.Term != nullptr && CurrentSession.Term->ScrollbackSize() > 0 && CurrentSession.Term->AltScreen() == false;
    const float TrackWidth = 18.0f * UiScale_;
    const float TrackInset = 8.0f * UiScale_;
    const float TrackLeft = PanelPos.x + Size.x - TrackWidth - 4.0f * UiScale_;
    const float TrackTop = PanelPos.y + TrackInset;
    const float TrackHeight = std::max(1.0f, Size.y - TrackInset * 2.0f);
    float BarHeight = 0.0f;
    bool BarHovered = false;
    bool BarActive = false;
    if (ShowScrollbar == true)
    {
        const int HistoryRows = CurrentSession.Term->ScrollbackSize();
        const int ScreenRows = CurrentSession.Term->Rows();
        BarHeight = std::max(32.0f * UiScale_, TrackHeight * ScreenRows / (float)(HistoryRows + ScreenRows));
        const float Travel = std::max(1.0f, TrackHeight - BarHeight);

        ImGui::SetCursorScreenPos(ImVec2(TrackLeft, TrackTop));
        ImGui::InvisibleButton("##scrollbar", ImVec2(TrackWidth, TrackHeight));
        BarHovered = ImGui::IsItemHovered();
        BarActive = ImGui::IsItemActive();
        if (BarHovered == true || BarActive == true)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        }
        if (BarActive == true)
        {
            const float Fraction = std::clamp((Io.MousePos.y - TrackTop - BarHeight * 0.5f) / Travel, 0.0f, 1.0f);
            CurrentSession.ScrollOffset = HistoryRows - static_cast<int>(std::round(Fraction * HistoryRows));
        }
        ImGui::SetCursorScreenPos(PanelPos);
    }

    // Submitted before the terminal button for the same reason as the scrollbar: otherwise the terminal swallows the click.
    const bool ShowResume = CurrentSession.CurrentStatus == Status::Exited && CurrentSession.Attempted == true;
    const float ResumeWidth = 170 * UiScale_;
    const ImVec2 ResumeMin(PanelPos.x + (Size.x - ResumeWidth) * 0.5f, PanelPos.y + Size.y * 0.5f + 4);
    const ImVec2 ResumeSize(ResumeWidth, ImGui::GetFrameHeight());
    bool ResumeClicked = false;
    bool ResumeHovered = false;
    if (ShowResume == true)
    {
        ImGui::SetCursorScreenPos(ResumeMin);
        ImGui::PushID(("resume" + CurrentSession.Rec.Id).c_str());
        ResumeClicked = ImGui::InvisibleButton("##resume", ResumeSize);
        ResumeHovered = ImGui::IsItemHovered();
        ImGui::PopID();
        ImGui::SetCursorScreenPos(PanelPos);
    }

    ImGui::InvisibleButton("##term", Size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool Hovered = ImGui::IsItemHovered();
    if (CurrentSession.FindOpen == true && (IsActive == false || ImGui::IsItemClicked(ImGuiMouseButton_Left) == true || CurrentSession.Term == nullptr))
    {
        CurrentSession.FindOpen = false;
        CurrentSession.FindHasMatch = false;
        CurrentSession.FindNoMatch = false;
    }

    if (CurrentSession.Term == nullptr)
    {
        DrawList->AddText(ImVec2(PanelPos.x + Padding, PanelPos.y + Padding), TextDisabledColorU32_, "Session not started.");
    }
    else
    {
        Terminal& CurrentTerminal = *CurrentSession.Term;
        const int ScrollbackSize = CurrentTerminal.ScrollbackSize();
        // keep the view anchored while output scrolls into history
        long long const PushedDelta = CurrentTerminal.PushedTotal() - CurrentSession.LastPushed;
        CurrentSession.LastPushed = CurrentTerminal.PushedTotal();
        if (CurrentSession.ScrollOffset > 0 && PushedDelta > 0)
        {
            CurrentSession.ScrollOffset += (int)PushedDelta;
        }
        CurrentSession.ScrollOffset = std::clamp(CurrentSession.ScrollOffset, 0, ScrollbackSize);
        const int ViewTop = ScrollbackSize - CurrentSession.ScrollOffset;
        const bool Reporting = CurrentTerminal.MouseMode() > 0 && Io.KeyShift == false;

        const std::function<void(ImVec2, int&, int&)> CellAt = [&](ImVec2 MousePos, int& OutRow, int& OutCol)
        {
            OutCol = std::clamp((int)std::floor((MousePos.x - (PanelPos.x + Padding)) / CellSize_.x), 0, CurrentTerminal.Cols() - 1);
            OutRow = std::clamp((int)std::floor((MousePos.y - PanelPos.y) / CellSize_.y), 0, CurrentTerminal.Rows() - 1);
        };

        // ---- mouse ----
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            int Row = 0, Col = 0;
            CellAt(Io.MousePos, Row, Col);
            if (Reporting)
            {
                CurrentSession.ReportingDrag = true;
                CurrentSession.ReportedRow = Row;
                CurrentSession.ReportedCol = Col;
                CurrentSession.Typing(Now_, StatusCallback(),
                               [&](Terminal& Term)
                               {
                                   Term.SendMouseMove(Row, Col, VTERM_MOD_NONE);
                                   Term.SendMouseButton(1, true, VTERM_MOD_NONE);
                               });
            }
            else
            {
                CurrentSession.Selecting = true;
                CurrentSession.HasSelection = false;
                CurrentSession.InputSelected = false;
                CurrentSession.SelectionStartRow = CurrentSession.SelectionEndRow = CurrentTerminal.StableRow(ViewTop + Row);
                CurrentSession.SelectionStartCol = CurrentSession.SelectionEndCol = Col;
            }
        }
        if (CurrentSession.Selecting && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left)))
        {
            int Row, Col;
            CellAt(Io.MousePos, Row, Col);
            CurrentSession.SelectionEndRow = CurrentTerminal.StableRow(ViewTop + Row);
            CurrentSession.SelectionEndCol = Col;
            CurrentSession.HasSelection = (CurrentSession.SelectionStartRow == CurrentSession.SelectionEndRow && CurrentSession.SelectionStartCol == CurrentSession.SelectionEndCol) == false;
        }
        // The program draws its own selection, so it needs every drag step, not just the press and release.
        if (CurrentSession.ReportingDrag == true && Reporting == true && ImGui::IsMouseDown(ImGuiMouseButton_Left) == true)
        {
            int Row = 0, Col = 0;
            CellAt(Io.MousePos, Row, Col);
            if (Row != CurrentSession.ReportedRow || Col != CurrentSession.ReportedCol)
            {
                CurrentSession.ReportedRow = Row;
                CurrentSession.ReportedCol = Col;
                CurrentSession.Typing(Now_, StatusCallback(), [&](Terminal& Term) { Term.SendMouseMove(Row, Col, VTERM_MOD_NONE); });
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            const bool WasReportingDrag = CurrentSession.ReportingDrag;
            CurrentSession.ReportingDrag = false;
            if (CurrentSession.Selecting)
            {
                CurrentSession.Selecting = false;
            }
            else if (Reporting && (Hovered || WasReportingDrag == true))
            {
                int Row = 0, Col = 0;
                CellAt(Io.MousePos, Row, Col);
                CurrentSession.Typing(Now_, StatusCallback(),
                               [&](Terminal& Term)
                               {
                                   Term.SendMouseMove(Row, Col, VTERM_MOD_NONE);
                                   Term.SendMouseButton(1, false, VTERM_MOD_NONE);
                               });
            }
        }
        if (Hovered && Reporting == false && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            if (CurrentSession.HasSelection)
            {
                CopySelection(CurrentSession);
            }
            else
            {
                Paste(CurrentSession);
            }
        }
        if (Hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
        {
            Paste(CurrentSession);
        }
        if (Hovered && Io.MouseWheel != 0.0f && Io.KeyCtrl == false)
        {
            if (Reporting)
            {
                int Row = 0, Col = 0;
                CellAt(Io.MousePos, Row, Col);
                int Steps = std::max(1, (int)std::abs(Io.MouseWheel * 3));
                CurrentSession.Typing(Now_, StatusCallback(),
                               [&](Terminal& Term)
                               {
                                   Term.SendMouseMove(Row, Col, VTERM_MOD_NONE);
                                   for (int Step = 0; Step < Steps; Step++)
                                   {
                                       Term.SendMouseButton(Io.MouseWheel > 0 ? 4 : 5, true, VTERM_MOD_NONE);
                                   }
                               });
            }
            else if (CurrentTerminal.AltScreen() == false)
            {
                CurrentSession.ScrollOffset = std::clamp(CurrentSession.ScrollOffset + (int)std::round(Io.MouseWheel * 3), 0, ScrollbackSize);
            }
        }
        if (Hovered)
        {
            ImGui::SetMouseCursor(Reporting ? ImGuiMouseCursor_Arrow : ImGuiMouseCursor_TextInput);
        }

        // ---- selection range (absolute rows) ----
        int SelRowA = 0, SelColA = 0, SelRowB = -1, SelColB = 0;
        if (CurrentSession.HasSelection)
        {
            SelRowA = CurrentTerminal.AbsRow(CurrentSession.SelectionStartRow);
            SelRowB = CurrentTerminal.AbsRow(CurrentSession.SelectionEndRow);
            SelColA = CurrentSession.SelectionStartCol;
            SelColB = CurrentSession.SelectionEndCol;
            if (SelRowA > SelRowB || (SelRowA == SelRowB && SelColA > SelColB))
            {
                std::swap(SelRowA, SelRowB);
                std::swap(SelColA, SelColB);
            }
        }

        // ---- cells ----
        const float FontSize = MonoFont_->FontSize;
        const float OriginX = PanelPos.x + Padding;
        const float TextOffsetY = (CellSize_.y - FontSize) * 0.5f;
        int CursorRow = 0, CursorCol = 0;
        bool CursorVisible = false;
        CurrentTerminal.GetCursor(CursorRow, CursorCol, CursorVisible);
        std::vector<Cell> RowCells((size_t)CurrentTerminal.Cols());
        const std::vector<uint32_t> FindQuery = CurrentSession.FindOpen == true ? Terminal::FoldQuery(CurrentSession.FindBuffer) : std::vector<uint32_t>{};
        const int FindLength = static_cast<int>(FindQuery.size());
        const int CurrentMatchRow = CurrentSession.FindHasMatch == true ? CurrentTerminal.AbsRow(CurrentSession.FindStableRow) : -1;
        std::vector<int> MatchStarts;
        std::vector<char> Highlight(static_cast<size_t>(CurrentTerminal.Cols()));

        DrawList->PushClipRect(PanelPos, ImVec2(PanelPos.x + Size.x, PanelPos.y + Size.y), true);
        for (int Row = 0; Row < CurrentTerminal.Rows(); Row++)
        {
            const int AbsRow = ViewTop + Row;
            const float RowY = PanelPos.y + Row * CellSize_.y;
            if (RowY > PanelPos.y + Size.y)
            {
                break;
            }
            for (int Col = 0; Col < CurrentTerminal.Cols(); Col++)
            {
                RowCells[(size_t)Col] = CurrentTerminal.GetCell(AbsRow, Col);
            }

            if (FindLength > 0)
            {
                std::fill(Highlight.begin(), Highlight.end(), static_cast<char>(0));
                Terminal::MatchColumns(RowCells, FindQuery, MatchStarts);
                for (const int Start : MatchStarts)
                {
                    const char Kind = (AbsRow == CurrentMatchRow && Start == CurrentSession.FindCol) ? 2 : 1;
                    std::fill(Highlight.begin() + Start, Highlight.begin() + Start + FindLength, Kind);
                }
            }

            // backgrounds (+ selection)
            int RunStart = 0;
            ImU32 RunColor = 0;
            bool RunActive = false;
            for (int Col = 0; Col <= CurrentTerminal.Cols();)
            {
                ImU32 CellColor = 0;
                bool CellPainted = false;
                int Span = 1;
                if (Col < CurrentTerminal.Cols())
                {
                    const Cell& CurrentCell = RowCells[(size_t)Col];
                    Span = CurrentCell.Width > 1 ? CurrentCell.Width : 1;
                    if (FindLength > 0 && Highlight[(size_t)Col] != 0)
                    {
                        CellColor = UiHelpers::WithAlpha(AccentColor_, Highlight[(size_t)Col] == 2 ? 230 : 110);
                        CellPainted = true;
                    }
                    else if (CurrentSession.HasSelection == true &&
                             ((AbsRow > SelRowA && AbsRow < SelRowB) || (AbsRow == SelRowA && AbsRow == SelRowB && Col >= SelColA && Col <= SelColB) ||
                              (AbsRow == SelRowA && AbsRow != SelRowB && Col >= SelColA) || (AbsRow == SelRowB && AbsRow != SelRowA && Col <= SelColB)))
                    {
                        CellColor = SelectionColorU32_;
                        CellPainted = true;
                    }
                    else if (CurrentCell.DefBg == false)
                    {
                        CellColor = UiHelpers::RgbCol(CurrentCell.Bg);
                        CellPainted = true;
                    }
                }
                if (RunActive == true && (CellPainted == false || CellColor != RunColor))
                {
                    DrawList->AddRectFilled(ImVec2(OriginX + RunStart * CellSize_.x, RowY), ImVec2(OriginX + Col * CellSize_.x, RowY + CellSize_.y), RunColor);
                    RunActive = false;
                }
                if (CellPainted == true && RunActive == false)
                {
                    RunStart = Col;
                    RunColor = CellColor;
                    RunActive = true;
                }
                Col += Span;
            }
            if (RunActive == true)
            {
                DrawList->AddRectFilled(ImVec2(OriginX + RunStart * CellSize_.x, RowY),
                                        ImVec2(OriginX + CurrentTerminal.Cols() * CellSize_.x, RowY + CellSize_.y), RunColor);
            }

            // cursor block
            if (CursorVisible && CurrentSession.ScrollOffset == 0 && Row == CursorRow && CurrentSession.IsRunning())
            {
                ImVec2 CursorMin(OriginX + CursorCol * CellSize_.x, RowY), CursorMax(OriginX + (CursorCol + 1) * CellSize_.x, RowY + CellSize_.y);
                if (IsActive && Focused_)
                {
                    DrawList->AddRectFilled(CursorMin, CursorMax, CursorFillColorU32_);
                }
                else
                {
                    DrawList->AddRect(CursorMin, CursorMax, CursorOutlineColorU32_);
                }
            }

            // text - each glyph is drawn on its own, pinned to its exact grid column. A bold font's glyphs
            // are usually a little wider than the regular font's, so batching same-colored characters into
            // one AddText() call (trusting the font's own cumulative advance) let a bold word overflow its
            // allotted cells and bleed into whatever was drawn next on the row.
            for (int Col = 0; Col < CurrentTerminal.Cols(); Col++)
            {
                const Cell& CurrentCell = RowCells[(size_t)Col];
                if (CurrentCell.Ch == Cell::CONTINUATION)
                {
                    continue;
                }
                const bool IsBlank = CurrentCell.Ch == 0 || CurrentCell.Ch == ' ';
                if (CurrentCell.Underline || CurrentCell.Strike)
                {
                    float LineX0 = OriginX + Col * CellSize_.x, LineX1 = LineX0 + CellSize_.x * (CurrentCell.Width > 1 ? CurrentCell.Width : 1);
                    if (CurrentCell.Underline)
                    {
                        DrawList->AddLine(ImVec2(LineX0, RowY + CellSize_.y - 2), ImVec2(LineX1, RowY + CellSize_.y - 2), UiHelpers::RgbCol(CurrentCell.Fg), 1.0f);
                    }
                    if (CurrentCell.Strike)
                    {
                        DrawList->AddLine(ImVec2(LineX0, RowY + CellSize_.y * 0.5f), ImVec2(LineX1, RowY + CellSize_.y * 0.5f), UiHelpers::RgbCol(CurrentCell.Fg), 1.0f);
                    }
                }
                if (IsBlank)
                {
                    continue;
                }
                const float GlyphX = OriginX + Col * CellSize_.x;
                ImFont* const CurrentFont = (CurrentCell.Bold == true && MonoBoldFont_ != nullptr && CurrentCell.Ch <= 0xFF) ? MonoBoldFont_ : MonoFont_;
                if (CurrentCell.Ch < 0x80)
                {
                    const char Ascii = (char)CurrentCell.Ch;
                    DrawList->AddText(CurrentFont, FontSize, ImVec2(GlyphX, RowY + TextOffsetY), UiHelpers::RgbCol(CurrentCell.Fg), &Ascii, &Ascii + 1);
                    continue;
                }
                if (DrawBoxChar(DrawList, GlyphX, RowY, CellSize_.x * (CurrentCell.Width > 1 ? CurrentCell.Width : 1), CellSize_.y, CurrentCell.Ch, UiHelpers::RgbCol(CurrentCell.Fg)))
                {
                    continue;
                }
                if (CurrentCell.Ch > 0xFFFF)
                {
                    continue;  // outside the atlas (emoji)
                }
                std::string const Glyph = Util::Utf8(CurrentCell.Ch);
                DrawList->AddText(CurrentFont, FontSize, ImVec2(GlyphX, RowY + TextOffsetY), UiHelpers::RgbCol(CurrentCell.Fg), Glyph.c_str(), Glyph.c_str() + Glyph.size());
            }
        }

        // scrollback indicator
        if (ShowScrollbar == true)
        {
            const float Travel = std::max(1.0f, TrackHeight - BarHeight);
            const int ShownViewTop = ScrollbackSize - CurrentSession.ScrollOffset;
            const float BarY0 = TrackTop + (float)ShownViewTop / (float)ScrollbackSize * Travel;
            const int BarAlpha = (BarActive == true) ? 190 : (BarHovered == true ? 140 : (CurrentSession.ScrollOffset > 0 ? 100 : 65));
            const float BarWidth = (BarHovered == true || BarActive == true) ? 10.0f * UiScale_ : 7.0f * UiScale_;
            const float BarRight = TrackLeft + TrackWidth - 2.0f * UiScale_;
            if (BarHovered == true || BarActive == true)
            {
                DrawList->AddRectFilled(ImVec2(TrackLeft, TrackTop), ImVec2(TrackLeft + TrackWidth, TrackTop + TrackHeight),
                                        UiHelpers::WithAlpha(IM_COL32(0xC9, 0xCF, 0xDB, 255), 20), 4);
            }
            DrawList->AddRectFilled(ImVec2(BarRight - BarWidth, BarY0), ImVec2(BarRight, BarY0 + BarHeight), UiHelpers::WithAlpha(IM_COL32(0xC9, 0xCF, 0xDB, 255), BarAlpha), 4);
        }
        if (CurrentSession.ScrollOffset > 0 && ScrollbackSize > 0)
        {
            const char* const Hint = "scrolled back - type to return";
            ImVec2 const HintSize = ImGui::CalcTextSize(Hint);
            DrawList->AddRectFilled(ImVec2(PanelPos.x + Size.x - HintSize.x - 22, PanelPos.y + Size.y - HintSize.y - 12),
                                    ImVec2(PanelPos.x + Size.x - 10, PanelPos.y + Size.y - 6), IM_COL32(0, 0, 0, 170), 4);
            DrawList->AddText(ImVec2(PanelPos.x + Size.x - HintSize.x - 16, PanelPos.y + Size.y - HintSize.y - 9),
                              IM_COL32(0xC9, 0xCF, 0xDB, 255), Hint);
        }
        DrawList->PopClipRect();
        if (CurrentSession.FindOpen == true)
        {
            DrawFindBar(CurrentSession, PanelPos, Size);
        }
    }

    // ---- "not running" overlay ----
    if (ShowResume == true)
    {
        DrawList->AddRectFilled(PanelPos, ImVec2(PanelPos.x + Size.x, PanelPos.y + Size.y), IM_COL32(11, 13, 17, 215));
        const char* const Message = "This session is not running.";
        ImVec2 const TextSize = ImGui::CalcTextSize(Message);
        ImGui::SetCursorScreenPos(ImVec2(PanelPos.x + (Size.x - TextSize.x) * 0.5f, PanelPos.y + Size.y * 0.5f - TextSize.y - 10));
        ImGui::TextDisabled("%s", Message);

        const char* const Label = "Resume session";
        const ImVec2 LabelSize = ImGui::CalcTextSize(Label);
        ImVec4 ButtonColor = AccentColorV4_;
        if (ResumeHovered == true)
        {
            ButtonColor = ImVec4(ButtonColor.x * 1.12f, ButtonColor.y * 1.12f, ButtonColor.z * 1.12f, ButtonColor.w);
        }
        DrawList->AddRectFilled(ResumeMin, ImVec2(ResumeMin.x + ResumeSize.x, ResumeMin.y + ResumeSize.y), ImGui::ColorConvertFloat4ToU32(ButtonColor), ImGui::GetStyle().FrameRounding);
        DrawList->AddText(ImVec2(ResumeMin.x + (ResumeSize.x - LabelSize.x) * 0.5f, ResumeMin.y + (ResumeSize.y - LabelSize.y) * 0.5f),
                          IM_COL32(26, 15, 10, 255), Label);

        if (ResumeClicked == true)
        {
            std::string Err;
            if (StartSession(CurrentSession, Cols, Rows, Err) == false)
            {
                ShowToast("Could not launch Claude: " + Err, true);
            }
            MarkDirty();
        }
    }
}
