#pragma once
#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

#include "ProjectLog.h"
#include "UiHelpers.h"
#include "imgui.h"

struct ProjectLogPalette
{
    ImVec4 Accent{};
    ImVec4 Text{};
    ImU32 PaneBg = 0;
    ImU32 TextU32 = 0;
    ImU32 TextDisabledU32 = 0;
};

// The Breakdown tab (work types as a donut chart with a legend) and the Activity tab (stacked bars over time)
// of the Project Log window.
class ProjectLogCharts
{
public:
    // Colors follow the all-time ranking of work types, so a type keeps its color in every chart and time range.
    static ImU32 WorkTypeColorFor(const ProjectLog& Log, const std::string& Type)
    {
        const std::vector<ProjectLog::TypeCount>& AllTime = Log.AllTimeCounts();
        for (size_t Index = 0; Index < AllTime.size(); Index++)
        {
            if (AllTime[Index].Type == Type)
            {
                return WorkTypeColor(Index);
            }
        }
        return WorkTypeColor(AllTime.size());
    }

    static void DrawEmptyState(const char* const Message, const float Scale)
    {
        ImGui::Dummy(ImVec2(0, 20 * Scale));
        ImGui::Indent(8 * Scale);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", Message);
        ImGui::PopStyleColor();
        ImGui::Unindent(8 * Scale);
    }

    void DrawBreakdown(const ProjectLog& Log, const float Scale, const ProjectLogPalette& Colors)
    {
        UiHelpers::SegmentedControl("##typeRange", {"Today", "This week", "All time"}, TypeRange_, ImGui::ColorConvertFloat4ToU32(Colors.Accent));
        const std::time_t Since = TypeRange_ == 2 ? 0 : ProjectLog::StartOfToday(TypeRange_ == 1);
        const std::vector<ProjectLog::TypeCount> Counts = Log.CountByType(Since);
        int Total = 0;
        for (const ProjectLog::TypeCount& Slice : Counts)
        {
            Total += Slice.Count;
        }
        if (Total == 0)
        {
            LegendHovered_ = -1;
            DrawEmptyState("Nothing logged in this period.", Scale);
            return;
        }
        ImGui::Spacing();
        ImGui::Spacing();

        // The donut takes whatever room the legend leaves; narrow windows put the legend underneath instead of beside it.
        const ImVec2 Avail = ImGui::GetContentRegionAvail();
        const float Gap = 32 * Scale;
        const bool Stacked = Avail.x < 520 * Scale;
        const float LegendWidth = Stacked == true ? Avail.x : std::max(260 * Scale, Avail.x * 0.42f);
        const float LegendHeight = LegendRowHeight(Scale) * static_cast<float>(Counts.size());
        const float RoomX = Stacked == true ? Avail.x : Avail.x - LegendWidth - Gap;
        const float RoomY = Stacked == true ? Avail.y - LegendHeight - Gap : Avail.y;
        const float Outer = std::max(48 * Scale, std::min(RoomX, RoomY) * 0.5f - 8 * Scale);
        const float Thickness = Outer * 0.32f;
        const float Inner = Outer - Thickness;
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const ImVec2 Center(Origin.x + RoomX * 0.5f, Origin.y + Outer + 8 * Scale);
        ImDrawList* const DrawList = ImGui::GetWindowDrawList();

        const float TwoPi = 6.2831853f;
        const ImVec2 Mouse = ImGui::GetIO().MousePos;
        const float MouseDx = Mouse.x - Center.x;
        const float MouseDy = Mouse.y - Center.y;
        const float MouseDistance = std::sqrt(MouseDx * MouseDx + MouseDy * MouseDy);
        const bool MouseInRing = MouseDistance >= Inner && MouseDistance <= Outer + 6 * Scale && ImGui::IsWindowHovered() == true;
        float MouseAngle = std::atan2(MouseDy, MouseDx) + TwoPi * 0.25f;
        if (MouseAngle < 0)
        {
            MouseAngle += TwoPi;
        }

        int HoveredIndex = -1;
        float Start = 0;
        for (size_t Index = 0; Index < Counts.size(); Index++)
        {
            const float Sweep = TwoPi * static_cast<float>(Counts[Index].Count) / static_cast<float>(Total);
            if (MouseInRing == true && MouseAngle >= Start && MouseAngle < Start + Sweep)
            {
                HoveredIndex = static_cast<int>(Index);
            }
            Start += Sweep;
        }
        if (LegendHovered_ >= static_cast<int>(Counts.size()))
        {
            LegendHovered_ = -1;
        }
        if (HoveredIndex < 0)
        {
            HoveredIndex = LegendHovered_;
        }

        // Each slice is a thick arc stroke, trimmed a little at both ends so neighbouring slices are separated by a gap.
        const float SliceGap = Counts.size() > 1 ? 0.025f : 0.0f;
        Start = 0;
        for (size_t Index = 0; Index < Counts.size(); Index++)
        {
            const float Sweep = TwoPi * static_cast<float>(Counts[Index].Count) / static_cast<float>(Total);
            const float Trim = std::min(SliceGap, Sweep * 0.25f);
            const bool IsHovered = HoveredIndex == static_cast<int>(Index);
            const float SliceThickness = IsHovered == true ? Thickness + 6 * Scale : Thickness;
            const float SliceRadius = Inner + SliceThickness * 0.5f;
            const int Segments = std::max(4, static_cast<int>(Sweep * 48.0f));
            DrawList->PathArcTo(Center, SliceRadius, Start + Trim - TwoPi * 0.25f, Start + Sweep - Trim - TwoPi * 0.25f, Segments);
            const ImU32 SliceColor = WorkTypeColorFor(Log, Counts[Index].Type);
            DrawList->PathStroke(HoveredIndex >= 0 && IsHovered == false ? UiHelpers::WithAlpha(SliceColor, 110) : SliceColor, ImDrawFlags_None, SliceThickness);
            Start += Sweep;
        }

        std::string CenterTop = std::to_string(Total);
        std::string CenterBottom = Total == 1 ? "entry" : "entries";
        if (HoveredIndex >= 0)
        {
            const ProjectLog::TypeCount& Hovered = Counts[static_cast<size_t>(HoveredIndex)];
            CenterTop = std::to_string(Percent(Hovered.Count, Total)) + "%";
            CenterBottom = Hovered.Type;
        }
        const ImVec2 TopSize = ImGui::CalcTextSize(CenterTop.c_str());
        const ImVec2 BottomSize = ImGui::CalcTextSize(CenterBottom.c_str());
        DrawList->AddText(ImVec2(Center.x - TopSize.x * 0.5f, Center.y - TopSize.y), Colors.TextU32, CenterTop.c_str());
        DrawList->AddText(ImVec2(Center.x - BottomSize.x * 0.5f, Center.y + 1 * Scale), Colors.TextDisabledU32, CenterBottom.c_str());

        const float DonutSize = Outer * 2 + 16 * Scale;
        ImGui::Dummy(ImVec2(RoomX, DonutSize));
        if (Stacked == true)
        {
            ImGui::Dummy(ImVec2(0, Gap * 0.5f));
            DrawLegend(Log, Counts, Total, HoveredIndex, LegendWidth, 0.0f, Scale);
            return;
        }
        ImGui::SameLine(0, Gap);
        DrawLegend(Log, Counts, Total, HoveredIndex, LegendWidth, DonutSize, Scale);
    }

    void DrawActivity(const ProjectLog& Log, const float Scale, const ProjectLogPalette& Colors)
    {
        constexpr int DAY_COUNT = 30;
        constexpr int MONTH_COUNT = 12;
        constexpr int GRID_LINES = 4;
        UiHelpers::SegmentedControl("##graphRange", {"Last 30 days", "Last 12 months"}, GraphMonthly_, ImGui::ColorConvertFloat4ToU32(Colors.Accent));
        const bool Monthly = GraphMonthly_ == 1;
        const std::vector<ProjectLog::Bucket> Buckets = Log.Buckets(Monthly, Monthly == true ? MONTH_COUNT : DAY_COUNT);
        const std::vector<ProjectLog::TypeCount>& AllTime = Log.AllTimeCounts();

        int Peak = 0;
        int Sum = 0;
        for (const ProjectLog::Bucket& Item : Buckets)
        {
            Peak = std::max(Peak, Item.Total);
            Sum += Item.Total;
        }
        if (Sum == 0)
        {
            DrawEmptyState("Nothing logged in this period.", Scale);
            return;
        }
        const std::string SumText = std::to_string(Sum) + (Sum == 1 ? " entry" : " entries");
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(SumText.c_str()).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", SumText.c_str());
        ImGui::Spacing();
        ImGui::Spacing();
        // Round the top of the axis up to a multiple of the grid count so the gridline labels are whole numbers.
        const int AxisMax = ((Peak + GRID_LINES - 1) / GRID_LINES) * GRID_LINES;

        const ImVec2 Avail = ImGui::GetContentRegionAvail();
        const float LegendHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        const float AxisLabelWidth = ImGui::CalcTextSize(std::to_string(AxisMax).c_str()).x + 10 * Scale;
        const float XLabelHeight = ImGui::GetTextLineHeight() + 8 * Scale;
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const float PlotLeft = Origin.x + AxisLabelWidth;
        const float PlotTop = Origin.y + ImGui::GetTextLineHeight() * 0.5f;
        const float PlotWidth = std::max(100 * Scale, Avail.x - AxisLabelWidth - 4 * Scale);
        const float PlotHeight = std::max(80 * Scale, Avail.y - LegendHeight - XLabelHeight - 16 * Scale);
        const float PlotBottom = PlotTop + PlotHeight;
        ImDrawList* const DrawList = ImGui::GetWindowDrawList();

        for (int Line = 0; Line <= GRID_LINES; Line++)
        {
            const float LineY = PlotBottom - PlotHeight * static_cast<float>(Line) / GRID_LINES;
            DrawList->AddLine(ImVec2(PlotLeft, LineY), ImVec2(PlotLeft + PlotWidth, LineY), UiHelpers::WithAlpha(Colors.TextDisabledU32, Line == 0 ? 120 : 35));
            const std::string Value = std::to_string(AxisMax * Line / GRID_LINES);
            const ImVec2 ValueSize = ImGui::CalcTextSize(Value.c_str());
            DrawList->AddText(ImVec2(PlotLeft - ValueSize.x - 8 * Scale, LineY - ValueSize.y * 0.5f), Colors.TextDisabledU32, Value.c_str());
        }

        const float Slot = PlotWidth / static_cast<float>(Buckets.size());
        const float BarWidth = std::max(2.0f, Slot * 0.62f);
        const float BarRounding = std::min(3 * Scale, BarWidth * 0.5f);
        const float WidestLabel = ImGui::CalcTextSize(Monthly == true ? "Mmm 00" : "00").x + 8 * Scale;
        const int LabelEvery = std::max(1, static_cast<int>(std::ceil(WidestLabel / Slot)));
        const ImVec2 Mouse = ImGui::GetIO().MousePos;
        int HoveredIndex = -1;
        for (size_t Index = 0; Index < Buckets.size(); Index++)
        {
            const ProjectLog::Bucket& Item = Buckets[Index];
            const float SlotLeft = PlotLeft + Slot * static_cast<float>(Index);
            const float BarLeft = SlotLeft + (Slot - BarWidth) * 0.5f;
            const bool Hovered = ImGui::IsWindowHovered() == true && Mouse.x >= SlotLeft && Mouse.x < SlotLeft + Slot && Mouse.y >= PlotTop && Mouse.y <= PlotBottom;
            if (Hovered == true)
            {
                HoveredIndex = static_cast<int>(Index);
                DrawList->AddRectFilled(ImVec2(SlotLeft, PlotTop), ImVec2(SlotLeft + Slot, PlotBottom), UiHelpers::WithAlpha(Colors.TextU32, 14), 4 * Scale);
            }
            // Stack segments in the all-time order so each type sits at the same height position in every bar.
            float Top = PlotBottom;
            int Remaining = Item.Total;
            for (const ProjectLog::TypeCount& Ranked : AllTime)
            {
                const std::vector<ProjectLog::TypeCount>::const_iterator Found = std::find_if(
                    Item.Types.begin(), Item.Types.end(), [&Ranked](const ProjectLog::TypeCount& Candidate) { return Candidate.Type == Ranked.Type; });
                if (Found == Item.Types.end())
                {
                    continue;
                }
                Remaining -= Found->Count;
                const float SegmentHeight = PlotHeight * static_cast<float>(Found->Count) / static_cast<float>(AxisMax);
                const ImDrawFlags Corners = Remaining <= 0 ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersNone;
                DrawList->AddRectFilled(ImVec2(BarLeft, Top - SegmentHeight), ImVec2(BarLeft + BarWidth, Top), WorkTypeColorFor(Log, Found->Type), BarRounding, Corners);
                Top -= SegmentHeight;
            }
            const bool IsLast = Index + 1 == Buckets.size();
            if ((Buckets.size() - 1 - Index) % static_cast<size_t>(LabelEvery) != 0 && IsLast == false)
            {
                continue;
            }
            const ImVec2 LabelSize = ImGui::CalcTextSize(Item.Label.c_str());
            DrawList->AddText(ImVec2(SlotLeft + (Slot - LabelSize.x) * 0.5f, PlotBottom + 6 * Scale), IsLast == true ? Colors.TextU32 : Colors.TextDisabledU32,
                              Item.Label.c_str());
        }

        if (HoveredIndex >= 0)
        {
            DrawBucketTooltip(Log, Buckets[static_cast<size_t>(HoveredIndex)], Scale);
        }

        ImGui::Dummy(ImVec2(AxisLabelWidth + PlotWidth, PlotHeight + XLabelHeight + 12 * Scale));
        DrawInlineLegend(Log, AllTime, Scale);
    }

private:
    void DrawLegend(const ProjectLog& Log, const std::vector<ProjectLog::TypeCount>& Counts, const int Total, const int HoveredIndex, const float Width,
                    const float AreaHeight, const float Scale)
    {
        const float LegendHeight = LegendRowHeight(Scale) * static_cast<float>(Counts.size());
        ImGui::BeginGroup();
        if (LegendHeight < AreaHeight)
        {
            ImGui::Dummy(ImVec2(0, (AreaHeight - LegendHeight) * 0.5f - ImGui::GetStyle().ItemSpacing.y));
        }
        LegendHovered_ = -1;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6 * Scale, 5 * Scale));
        if (ImGui::BeginTable("##typeLegend", 4, ImGuiTableFlags_SizingFixedFit, ImVec2(Width, 0)) == true)
        {
            const float BarColumnWidth = std::max(40 * Scale, Width * 0.28f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Percent", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("100%").x);
            ImGui::TableSetupColumn("Bar", ImGuiTableColumnFlags_WidthFixed, BarColumnWidth);
            ImDrawList* const DrawList = ImGui::GetWindowDrawList();
            const float Swatch = ImGui::GetTextLineHeight() * 0.6f;
            for (size_t Index = 0; Index < Counts.size(); Index++)
            {
                const ProjectLog::TypeCount& Row = Counts[Index];
                const ImU32 RowColor = WorkTypeColorFor(Log, Row.Type);
                const bool IsHovered = HoveredIndex == static_cast<int>(Index);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const ImVec2 RowPos = ImGui::GetCursorScreenPos();
                const float SwatchTop = RowPos.y + (ImGui::GetTextLineHeight() - Swatch) * 0.5f;
                DrawList->AddCircleFilled(ImVec2(RowPos.x + Swatch * 0.5f, SwatchTop + Swatch * 0.5f), Swatch * 0.5f, RowColor);
                ImGui::Dummy(ImVec2(Swatch, ImGui::GetTextLineHeight()));
                ImGui::SameLine(0, 8 * Scale);
                ImGui::PushID(static_cast<int>(Index));
                ImGui::PushStyleColor(ImGuiCol_Header, UiHelpers::WithAlpha(RowColor, 30));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, UiHelpers::WithAlpha(RowColor, 30));
                ImGui::Selectable(Row.Type.c_str(), IsHovered, ImGuiSelectableFlags_SpanAllColumns);
                ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered() == true)
                {
                    LegendHovered_ = static_cast<int>(Index);
                }
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                RightAlignedDisabled(std::to_string(Row.Count));
                ImGui::TableSetColumnIndex(2);
                RightAlignedDisabled(std::to_string(Percent(Row.Count, Total)) + "%");
                ImGui::TableSetColumnIndex(3);
                const ImVec2 BarPos = ImGui::GetCursorScreenPos();
                const float BarHeight = 6 * Scale;
                const float BarTop = BarPos.y + (ImGui::GetTextLineHeight() - BarHeight) * 0.5f;
                const float BarWidth = ImGui::GetContentRegionAvail().x;
                DrawList->AddRectFilled(ImVec2(BarPos.x, BarTop), ImVec2(BarPos.x + BarWidth, BarTop + BarHeight), ImGui::GetColorU32(ImGuiCol_FrameBg), BarHeight * 0.5f);
                const float Filled = std::max(BarHeight, BarWidth * static_cast<float>(Row.Count) / static_cast<float>(Counts.front().Count));
                DrawList->AddRectFilled(ImVec2(BarPos.x, BarTop), ImVec2(BarPos.x + Filled, BarTop + BarHeight), RowColor, BarHeight * 0.5f);
                ImGui::Dummy(ImVec2(BarWidth, ImGui::GetTextLineHeight()));
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        ImGui::EndGroup();
    }

    void DrawBucketTooltip(const ProjectLog& Log, const ProjectLog::Bucket& Item, const float Scale)
    {
        ImGui::BeginTooltip();
        ImGui::Text("%s", Item.Title.c_str());
        ImGui::SameLine(0, 12 * Scale);
        ImGui::TextDisabled("%d %s", Item.Total, Item.Total == 1 ? "entry" : "entries");
        if (Item.Types.empty() == false)
        {
            ImGui::Separator();
        }
        std::vector<ProjectLog::TypeCount> Sorted = Item.Types;
        std::stable_sort(Sorted.begin(), Sorted.end(), [](const ProjectLog::TypeCount& Left, const ProjectLog::TypeCount& Right) { return Left.Count > Right.Count; });
        for (const ProjectLog::TypeCount& Part : Sorted)
        {
            DrawSwatchLabel(WorkTypeColorFor(Log, Part.Type), Part.Type.c_str(), Scale);
            ImGui::SameLine();
            ImGui::TextDisabled("%d", Part.Count);
        }
        ImGui::EndTooltip();
    }

    void DrawInlineLegend(const ProjectLog& Log, const std::vector<ProjectLog::TypeCount>& AllTime, const float Scale)
    {
        bool First = true;
        for (const ProjectLog::TypeCount& Ranked : AllTime)
        {
            const float ItemWidth = ImGui::GetTextLineHeight() + ImGui::CalcTextSize(Ranked.Type.c_str()).x;
            if (First == false)
            {
                ImGui::SameLine(0, 16 * Scale);
                if (ImGui::GetContentRegionAvail().x < ItemWidth)
                {
                    ImGui::NewLine();
                }
            }
            First = false;
            DrawSwatchLabel(WorkTypeColorFor(Log, Ranked.Type), Ranked.Type.c_str(), Scale);
        }
    }

    static void DrawSwatchLabel(const ImU32 Color, const char* const Label, const float Scale)
    {
        const float Swatch = ImGui::GetTextLineHeight() * 0.6f;
        const ImVec2 RowPos = ImGui::GetCursorScreenPos();
        const float SwatchTop = RowPos.y + (ImGui::GetTextLineHeight() - Swatch) * 0.5f;
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(RowPos.x + Swatch * 0.5f, SwatchTop + Swatch * 0.5f), Swatch * 0.5f, Color);
        ImGui::Dummy(ImVec2(Swatch, ImGui::GetTextLineHeight()));
        ImGui::SameLine(0, 6 * Scale);
        ImGui::TextUnformatted(Label);
    }

    static void RightAlignedDisabled(const std::string& Text)
    {
        const float TextWidth = ImGui::CalcTextSize(Text.c_str()).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - TextWidth));
        ImGui::TextDisabled("%s", Text.c_str());
    }

    static float LegendRowHeight(const float Scale)
    {
        return ImGui::GetTextLineHeight() + 10 * Scale;
    }

    static int Percent(const int Count, const int Total)
    {
        return (Count * 100 + Total / 2) / Total;
    }

    static ImU32 WorkTypeColor(const size_t Index)
    {
        static constexpr ImU32 PALETTE[] = {IM_COL32(86, 156, 214, 255),  IM_COL32(230, 145, 56, 255),  IM_COL32(106, 190, 120, 255),
                                            IM_COL32(214, 92, 92, 255),   IM_COL32(170, 120, 210, 255), IM_COL32(220, 200, 80, 255),
                                            IM_COL32(80, 200, 200, 255),  IM_COL32(220, 120, 180, 255), IM_COL32(150, 150, 110, 255),
                                            IM_COL32(120, 140, 230, 255), IM_COL32(180, 220, 100, 255), IM_COL32(160, 110, 80, 255)};
        return PALETTE[Index % (sizeof(PALETTE) / sizeof(PALETTE[0]))];
    }

    int TypeRange_ = 2;      // 0 today, 1 this week, 2 all time
    int GraphMonthly_ = 0;   // 0 day by day, 1 month by month
    int LegendHovered_ = -1; // legend row under the mouse last frame, highlighted in the donut
};
