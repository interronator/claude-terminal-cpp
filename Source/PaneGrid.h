#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "imgui.h"

struct PaneRect
{
    ImVec2 Pos;
    ImVec2 Size;
};

class PaneGrid
{
public:
    void Prepare(const int Count, const int Columns)
    {
        if (Count == Count_ && Columns == Columns_)
        {
            return;
        }
        Count_ = Count;
        Columns_ = Columns;
        const int Rows = Columns > 0 ? (Count + Columns - 1) / Columns : 0;
        RowWeights_.assign(static_cast<size_t>(Rows), Rows > 0 ? 1.0f / static_cast<float>(Rows) : 0.0f);
        ColumnWeights_.clear();
        for (int Row = 0; Row < Rows; Row++)
        {
            const int InRow = PanesInRow(Row);
            ColumnWeights_.push_back(std::vector<float>(static_cast<size_t>(InRow), 1.0f / static_cast<float>(InRow)));
        }
    }

    void Compute(const ImVec2 Origin, const ImVec2 Avail, const float Gap)
    {
        Origin_ = Origin;
        Gap_ = Gap;
        const int Rows = static_cast<int>(RowWeights_.size());
        ContentHeight_ = Avail.y - Gap * static_cast<float>(Rows + 1);
        ContentWidth_ = Avail.x - Gap * 2.0f;
        Rects_.assign(static_cast<size_t>(Count_), PaneRect());
        float RowTop = Origin.y;
        for (int Row = 0; Row < Rows; Row++)
        {
            const float RowHeight = RowWeights_[static_cast<size_t>(Row)] * ContentHeight_;
            const int InRow = PanesInRow(Row);
            const float RowContentWidth = Avail.x - Gap * static_cast<float>(InRow + 1);
            float ColumnLeft = Origin.x;
            for (int Column = 0; Column < InRow; Column++)
            {
                const float Width = ColumnWeights_[static_cast<size_t>(Row)][static_cast<size_t>(Column)] * RowContentWidth;
                Rects_[static_cast<size_t>(Row * Columns_ + Column)] = PaneRect{ImVec2(ColumnLeft, RowTop), ImVec2(Width, RowHeight)};
                ColumnLeft += Width + Gap;
            }
            RowTop += RowHeight + Gap;
        }
    }

    const PaneRect& Cell(const int Index) const
    {
        return Rects_[static_cast<size_t>(Index)];
    }

    // Draws the draggable gaps between panes and moves size between the two panes on either side of a dragged gap.
    void DrawSplitters(const float MinWidth, const float MinHeight, const ImU32 HighlightColor)
    {
        const int Rows = static_cast<int>(RowWeights_.size());
        for (int Row = 0; Row < Rows; Row++)
        {
            const int InRow = PanesInRow(Row);
            const float RowContentWidth = ContentWidth_ + Gap_ * 2.0f - Gap_ * static_cast<float>(InRow + 1);
            for (int Column = 0; Column + 1 < InRow; Column++)
            {
                const PaneRect& Left = Rects_[static_cast<size_t>(Row * Columns_ + Column)];
                const ImVec2 Pos(Left.Pos.x + Left.Size.x, Left.Pos.y);
                const std::string Id = "##paneSplitV" + std::to_string(Row) + "_" + std::to_string(Column);
                float Delta = 0.0f;
                bool Reset = false;
                DrawGap(Id, Pos, ImVec2(Gap_, Left.Size.y), true, HighlightColor, Delta, Reset);
                std::vector<float>& Weights = ColumnWeights_[static_cast<size_t>(Row)];
                if (Reset == true)
                {
                    Weights.assign(Weights.size(), 1.0f / static_cast<float>(InRow));
                    continue;
                }
                MoveWeight(Weights[static_cast<size_t>(Column)], Weights[static_cast<size_t>(Column + 1)],
                           Delta / RowContentWidth, MinWidth / RowContentWidth, InRow);
            }
        }

        for (int Row = 0; Row + 1 < Rows; Row++)
        {
            const PaneRect& Top = Rects_[static_cast<size_t>(Row * Columns_)];
            const ImVec2 Pos(Origin_.x, Top.Pos.y + Top.Size.y);
            const std::string Id = "##paneSplitH" + std::to_string(Row);
            float Delta = 0.0f;
            bool Reset = false;
            DrawGap(Id, Pos, ImVec2(ContentWidth_, Gap_), false, HighlightColor, Delta, Reset);
            if (Reset == true)
            {
                RowWeights_.assign(RowWeights_.size(), 1.0f / static_cast<float>(Rows));
                continue;
            }
            MoveWeight(RowWeights_[static_cast<size_t>(Row)], RowWeights_[static_cast<size_t>(Row + 1)],
                       Delta / ContentHeight_, MinHeight / ContentHeight_, Rows);
        }
    }

private:
    int PanesInRow(const int Row) const
    {
        return std::min(Columns_, Count_ - Row * Columns_);
    }

    void DrawGap(const std::string& Id, const ImVec2 Pos, const ImVec2 Size, const bool Vertical, const ImU32 HighlightColor, float& OutDelta,
                 bool& OutReset) const
    {
        if (Size.x <= 0.0f || Size.y <= 0.0f)
        {
            return;
        }
        ImGui::SetCursorPos(Pos);
        ImGui::InvisibleButton(Id.c_str(), Size);
        const bool Active = ImGui::IsItemActive();
        const bool Hovered = ImGui::IsItemHovered();
        if (Hovered == true || Active == true)
        {
            ImGui::SetMouseCursor(Vertical == true ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
            ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), HighlightColor);
        }
        if (Active == true)
        {
            OutDelta = Vertical == true ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
        }
        if (Hovered == true && ImGui::IsMouseDoubleClicked(0) == true)
        {
            OutReset = true;
        }
    }

    static void MoveWeight(float& First, float& Second, const float Delta, const float MinFraction, const int Parts)
    {
        const float Minimum = std::min(MinFraction, 0.5f / static_cast<float>(Parts));
        const float Lower = Minimum - First;
        const float Upper = Second - Minimum;
        if (Lower > Upper)
        {
            return;
        }
        const float Moved = std::clamp(Delta, Lower, Upper);
        First += Moved;
        Second -= Moved;
    }

    int Count_ = 0;
    int Columns_ = 0;
    std::vector<float> RowWeights_;
    std::vector<std::vector<float>> ColumnWeights_;
    std::vector<PaneRect> Rects_;
    ImVec2 Origin_ = ImVec2(0, 0);
    float Gap_ = 0.0f;
    float ContentWidth_ = 0.0f;
    float ContentHeight_ = 0.0f;
};
