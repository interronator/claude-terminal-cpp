#pragma once
#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

#include "ProjectLog.h"
#include "Store.h"
#include "UiHelpers.h"
#include "imgui.h"

// The floating Project Log window: a project's PROJECT_LOG.md grouped by hour, day and week, a work-type pie chart,
// a stacked bar graph over time, and the editable notes section.
class ProjectLogWindow
{
public:
    struct Palette
    {
        ImVec4 Accent{};
        ImVec4 Text{};
        ImU32 PaneBg = 0;
        ImU32 TextU32 = 0;
        ImU32 TextDisabledU32 = 0;
    };

    void Open(const std::string& ProjectId)
    {
        ProjectId_ = ProjectId;
        CheckAt_ = 0;
        NotesDirty_ = false;
        Focus_ = true;
    }

    // Draws the window when it is open. Sets Error when saving the notes fails, for the caller to show.
    void Draw(const std::vector<Project>& Projects, const double Now, const float Scale, const Palette& Colors, std::string& Error)
    {
        if (ProjectId_.empty() == true)
        {
            return;
        }
        const std::vector<Project>::const_iterator Found =
            std::find_if(Projects.begin(), Projects.end(), [this](const Project& Candidate) { return Candidate.Id == ProjectId_; });
        if (Found == Projects.end())
        {
            ProjectId_.clear();
            return;
        }
        const Project* const LogProject = &*Found;
        Scale_ = Scale;
        Colors_ = Colors;
        if (Now >= CheckAt_)
        {
            CheckAt_ = Now + 1.0;
            if (Log_.Refresh(LogProject->Path) == true && NotesDirty_ == false)
            {
                Notes_ = Log_.Notes();
            }
        }

        ImGuiViewport* const Viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(760 * Scale, Viewport->WorkSize.x * 0.9f), std::min(620 * Scale, Viewport->WorkSize.y * 0.85f)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(380 * Scale, 260 * Scale), ImVec2(Viewport->WorkSize.x, Viewport->WorkSize.y));
        if (Focus_ == true)
        {
            ImGui::SetNextWindowFocus();
            Focus_ = false;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * Scale, 12 * Scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * Scale);
        bool IsOpen = true;
        const std::string Title = "Project log - " + LogProject->Name + "###projectLog";
        if (ImGui::Begin(Title.c_str(), &IsOpen, ImGuiWindowFlags_NoCollapse) == true)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * Scale, 6 * Scale));
            ImGui::SetNextItemWidth(220 * Scale);
            if (ImGui::BeginCombo("##logProject", LogProject->Name.c_str()) == true)
            {
                for (const Project& Candidate : Projects)
                {
                    if (ImGui::Selectable(Candidate.Name.c_str(), Candidate.Id == ProjectId_) == true && Candidate.Id != ProjectId_)
                    {
                        Open(Candidate.Id);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", ProjectLog::PathFor(LogProject->Path).string().c_str());
            if (ImGui::BeginTabBar("##logTabs") == true)
            {
                if (ImGui::BeginTabItem("By hour") == true)
                {
                    DrawEntries(ProjectLog::Grouping::Hour);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("By day") == true)
                {
                    DrawEntries(ProjectLog::Grouping::Day);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("By week") == true)
                {
                    DrawEntries(ProjectLog::Grouping::Week);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("By type") == true)
                {
                    DrawTypeChart();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Graph") == true)
                {
                    DrawGraph();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(NotesDirty_ == true ? "Notes*###notesTab" : "Notes###notesTab") == true)
                {
                    ImGui::TextDisabled("Notes are shared with every session in this project; they read them when they start.");
                    const float ButtonRow = ImGui::GetFrameHeightWithSpacing();
                    if (ImGui::InputTextMultiline("##logNotes", Notes_.data(), Notes_.capacity() + 1, ImVec2(-1, -ButtonRow),
                                                  ImGuiInputTextFlags_CallbackResize, ResizeStringCallback, &Notes_) == true)
                    {
                        NotesDirty_ = true;
                    }
                    ImGui::BeginDisabled(NotesDirty_ == false);
                    if (ImGui::Button("Save notes") == true)
                    {
                        if (Log_.SaveNotes(LogProject->Path, Notes_) == true)
                        {
                            NotesDirty_ = false;
                            Notes_ = Log_.Notes();
                        }
                        else
                        {
                            Error = "Could not write " + ProjectLog::PathFor(LogProject->Path).string();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Discard changes") == true)
                    {
                        Notes_ = Log_.Notes();
                        NotesDirty_ = false;
                    }
                    ImGui::EndDisabled();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::PopStyleVar();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        if (IsOpen == false)
        {
            ProjectId_.clear();
        }
    }


private:
    void DrawEntries(const ProjectLog::Grouping By)
    {
        ImGui::BeginChild("##logEntries", ImVec2(0, 0), ImGuiChildFlags_None);
        const std::vector<ProjectLog::Group> Groups = Log_.Grouped(By);
        if (Groups.empty() == true)
        {
            ImGui::TextWrapped("%s", Log_.FileExists() == true
                                         ? "Nothing has been logged yet. Sessions add a line here whenever they finish a piece of work."
                                         : "No log yet. Sessions create the file and add a line here whenever they finish a piece of work. "
                                           "Sessions that were already running before this was added need a Restart to pick it up.");
        }
        constexpr int OPEN_GROUP_COUNT = 3;
        for (size_t GroupIndex = 0; GroupIndex < Groups.size(); GroupIndex++)
        {
            const ProjectLog::Group& Current = Groups[GroupIndex];
            const std::string Header = Current.Title + "  (" + std::to_string(Current.Entries.size()) + ")###group" + Current.Title;
            const ImGuiTreeNodeFlags Flags = GroupIndex < OPEN_GROUP_COUNT ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
            if (ImGui::CollapsingHeader(Header.c_str(), Flags) == false)
            {
                continue;
            }
            for (const ProjectLog::Entry& Item : Current.Entries)
            {
                const size_t TimeAt = Item.Stamp.find(' ');
                const std::string When = By == ProjectLog::Grouping::Week || TimeAt == std::string::npos ? Item.Stamp : Item.Stamp.substr(TimeAt + 1);
                ImGui::TextDisabled("%s", When.c_str());
                if (Item.SessionName.empty() == false)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(Colors_.Accent, "%s", Item.SessionName.c_str());
                }
                if (Item.Type.empty() == false)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("[%s]", Item.Type.c_str());
                }
                ImGui::SameLine();
                ImGui::TextWrapped("%s", Item.Text.c_str());
            }
            ImGui::Spacing();
        }
        ImGui::EndChild();
    }

    void DrawTypeChart()
    {
        const float Scale = Scale_;
        ImGui::RadioButton("Today", &TypeRange_, 0);
        ImGui::SameLine();
        ImGui::RadioButton("This week", &TypeRange_, 1);
        ImGui::SameLine();
        ImGui::RadioButton("All time", &TypeRange_, 2);
        const std::time_t Since = TypeRange_ == 2 ? 0 : ProjectLog::StartOfToday(TypeRange_ == 1);
        const std::vector<ProjectLog::TypeCount> Counts = Log_.CountByType(Since);
        int Total = 0;
        for (const ProjectLog::TypeCount& Slice : Counts)
        {
            Total += Slice.Count;
        }
        if (Total == 0)
        {
            ImGui::TextDisabled("Nothing logged in this period.");
            return;
        }

        const ImVec2 Avail = ImGui::GetContentRegionAvail();
        const float LegendWidth = 240 * Scale;
        const float Radius = std::max(40 * Scale, std::min((Avail.x - LegendWidth) * 0.5f, Avail.y * 0.5f) - 12 * Scale);
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const ImVec2 Center(Origin.x + Radius + 8 * Scale, Origin.y + Radius + 8 * Scale);
        ImDrawList* const DrawList = ImGui::GetWindowDrawList();

        const ImVec2 Mouse = ImGui::GetIO().MousePos;
        const float MouseDx = Mouse.x - Center.x;
        const float MouseDy = Mouse.y - Center.y;
        const bool MouseInPie = MouseDx * MouseDx + MouseDy * MouseDy <= Radius * Radius && ImGui::IsWindowHovered() == true;
        const float TwoPi = 6.2831853f;
        float MouseAngle = std::atan2(MouseDy, MouseDx) + TwoPi * 0.25f;
        if (MouseAngle < 0)
        {
            MouseAngle += TwoPi;
        }

        // Slices are filled in pieces of at most a quarter turn, since a convex fill can't draw a wedge wider than half.
        constexpr float MAX_PIECE = 1.5707963f;
        int HoveredIndex = -1;
        float Start = 0;
        for (size_t Index = 0; Index < Counts.size(); Index++)
        {
            const float Sweep = TwoPi * static_cast<float>(Counts[Index].Count) / static_cast<float>(Total);
            if (MouseInPie == true && MouseAngle >= Start && MouseAngle < Start + Sweep)
            {
                HoveredIndex = static_cast<int>(Index);
            }
            const float SliceRadius = HoveredIndex == static_cast<int>(Index) ? Radius + 5 * Scale : Radius;
            float PieceStart = Start;
            while (PieceStart < Start + Sweep - 0.0001f)
            {
                const float PieceEnd = std::min(PieceStart + MAX_PIECE, Start + Sweep);
                DrawList->PathLineTo(Center);
                DrawList->PathArcTo(Center, SliceRadius, PieceStart - TwoPi * 0.25f, PieceEnd - TwoPi * 0.25f, 24);
                DrawList->PathFillConvex(WorkTypeColorFor(Counts[Index].Type));
                PieceStart = PieceEnd;
            }
            if (Counts.size() > 1)
            {
                const float Edge = Start - TwoPi * 0.25f;
                DrawList->AddLine(Center, ImVec2(Center.x + std::cos(Edge) * SliceRadius, Center.y + std::sin(Edge) * SliceRadius), Colors_.PaneBg, 2 * Scale);
            }
            Start += Sweep;
        }
        if (HoveredIndex >= 0)
        {
            const ProjectLog::TypeCount& Hovered = Counts[static_cast<size_t>(HoveredIndex)];
            ImGui::SetTooltip("%s: %d (%d%%)", Hovered.Type.c_str(), Hovered.Count, (Hovered.Count * 100 + Total / 2) / Total);
        }

        ImGui::Dummy(ImVec2(Radius * 2 + 16 * Scale, Radius * 2 + 16 * Scale));
        ImGui::SameLine(0, 20 * Scale);
        ImGui::BeginGroup();
        const float Swatch = ImGui::GetTextLineHeight() * 0.8f;
        for (size_t Index = 0; Index < Counts.size(); Index++)
        {
            const ImVec2 RowPos = ImGui::GetCursorScreenPos();
            const float SwatchTop = RowPos.y + (ImGui::GetTextLineHeight() - Swatch) * 0.5f;
            DrawList->AddRectFilled(ImVec2(RowPos.x, SwatchTop), ImVec2(RowPos.x + Swatch, SwatchTop + Swatch), WorkTypeColorFor(Counts[Index].Type), 2 * Scale);
            ImGui::Dummy(ImVec2(Swatch, ImGui::GetTextLineHeight()));
            ImGui::SameLine();
            const ImVec4 TextColor = static_cast<int>(Index) == HoveredIndex ? Colors_.Accent : Colors_.Text;
            ImGui::TextColored(TextColor, "%s", Counts[Index].Type.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%d  (%d%%)", Counts[Index].Count, (Counts[Index].Count * 100 + Total / 2) / Total);
        }
        ImGui::Spacing();
        ImGui::TextDisabled("%d entries", Total);
        ImGui::EndGroup();
    }

    void DrawGraph()
    {
        constexpr int DAY_COUNT = 30;
        constexpr int MONTH_COUNT = 12;
        constexpr int GRID_LINES = 4;
        const float Scale = Scale_;
        ImGui::RadioButton("Day by day (30 days)", &GraphMonthly_, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Month by month (12 months)", &GraphMonthly_, 1);
        const bool Monthly = GraphMonthly_ == 1;
        const std::vector<ProjectLog::Bucket> Buckets = Log_.Buckets(Monthly, Monthly == true ? MONTH_COUNT : DAY_COUNT);
        const std::vector<ProjectLog::TypeCount>& AllTime = Log_.AllTimeCounts();

        int Peak = 0;
        int Sum = 0;
        for (const ProjectLog::Bucket& Item : Buckets)
        {
            Peak = std::max(Peak, Item.Total);
            Sum += Item.Total;
        }
        if (Sum == 0)
        {
            ImGui::TextDisabled("Nothing logged in this period.");
            return;
        }
        // Round the top of the axis up to a multiple of the grid count so the gridline labels are whole numbers.
        const int AxisMax = ((Peak + GRID_LINES - 1) / GRID_LINES) * GRID_LINES;

        const ImVec2 Avail = ImGui::GetContentRegionAvail();
        const float LegendHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        const float AxisLabelWidth = ImGui::CalcTextSize(std::to_string(AxisMax).c_str()).x + 8 * Scale;
        const float XLabelHeight = ImGui::GetTextLineHeight() + 6 * Scale;
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const float PlotLeft = Origin.x + AxisLabelWidth;
        const float PlotTop = Origin.y + 6 * Scale;
        const float PlotWidth = std::max(100 * Scale, Avail.x - AxisLabelWidth - 8 * Scale);
        const float PlotHeight = std::max(80 * Scale, Avail.y - LegendHeight - XLabelHeight - 12 * Scale);
        const float PlotBottom = PlotTop + PlotHeight;
        ImDrawList* const DrawList = ImGui::GetWindowDrawList();

        for (int Line = 0; Line <= GRID_LINES; Line++)
        {
            const float Y = PlotBottom - PlotHeight * static_cast<float>(Line) / GRID_LINES;
            DrawList->AddLine(ImVec2(PlotLeft, Y), ImVec2(PlotLeft + PlotWidth, Y), UiHelpers::WithAlpha(Colors_.TextDisabledU32, Line == 0 ? 160 : 50));
            const std::string Value = std::to_string(AxisMax * Line / GRID_LINES);
            const ImVec2 ValueSize = ImGui::CalcTextSize(Value.c_str());
            DrawList->AddText(ImVec2(PlotLeft - ValueSize.x - 6 * Scale, Y - ValueSize.y * 0.5f), Colors_.TextDisabledU32, Value.c_str());
        }

        const float Slot = PlotWidth / static_cast<float>(Buckets.size());
        const float BarWidth = std::max(2.0f, Slot * 0.7f);
        const float WidestLabel = ImGui::CalcTextSize(Monthly == true ? "Mmm 00" : "00").x + 6 * Scale;
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
                DrawList->AddRectFilled(ImVec2(SlotLeft, PlotTop), ImVec2(SlotLeft + Slot, PlotBottom), UiHelpers::WithAlpha(Colors_.TextU32, 18));
            }
            // Stack segments in the all-time order so each type sits at the same height position in every bar.
            float Top = PlotBottom;
            for (const ProjectLog::TypeCount& Ranked : AllTime)
            {
                const std::vector<ProjectLog::TypeCount>::const_iterator Found = std::find_if(
                    Item.Types.begin(), Item.Types.end(), [&Ranked](const ProjectLog::TypeCount& Candidate) { return Candidate.Type == Ranked.Type; });
                if (Found == Item.Types.end())
                {
                    continue;
                }
                const float SegmentHeight = PlotHeight * static_cast<float>(Found->Count) / static_cast<float>(AxisMax);
                DrawList->AddRectFilled(ImVec2(BarLeft, Top - SegmentHeight), ImVec2(BarLeft + BarWidth, Top), WorkTypeColorFor(Found->Type));
                Top -= SegmentHeight;
            }
            const bool IsLast = Index + 1 == Buckets.size();
            if ((Buckets.size() - 1 - Index) % static_cast<size_t>(LabelEvery) != 0 && IsLast == false)
            {
                continue;
            }
            const ImVec2 LabelSize = ImGui::CalcTextSize(Item.Label.c_str());
            DrawList->AddText(ImVec2(SlotLeft + (Slot - LabelSize.x) * 0.5f, PlotBottom + 4 * Scale), IsLast == true ? Colors_.TextU32 : Colors_.TextDisabledU32,
                              Item.Label.c_str());
        }

        if (HoveredIndex >= 0)
        {
            const ProjectLog::Bucket& Item = Buckets[static_cast<size_t>(HoveredIndex)];
            ImGui::BeginTooltip();
            ImGui::Text("%s  -  %d %s", Item.Title.c_str(), Item.Total, Item.Total == 1 ? "entry" : "entries");
            std::vector<ProjectLog::TypeCount> Sorted = Item.Types;
            std::stable_sort(Sorted.begin(), Sorted.end(), [](const ProjectLog::TypeCount& Left, const ProjectLog::TypeCount& Right) { return Left.Count > Right.Count; });
            for (const ProjectLog::TypeCount& Part : Sorted)
            {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(WorkTypeColorFor(Part.Type)), "%s", Part.Type.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%d", Part.Count);
            }
            ImGui::EndTooltip();
        }

        ImGui::Dummy(ImVec2(AxisLabelWidth + PlotWidth, PlotHeight + XLabelHeight + 10 * Scale));
        const float Swatch = ImGui::GetTextLineHeight() * 0.8f;
        bool First = true;
        for (const ProjectLog::TypeCount& Ranked : AllTime)
        {
            const ImVec2 LegendTextSize = ImGui::CalcTextSize(Ranked.Type.c_str());
            if (First == false)
            {
                ImGui::SameLine(0, 14 * Scale);
                if (ImGui::GetContentRegionAvail().x < Swatch + LegendTextSize.x + 8 * Scale)
                {
                    ImGui::NewLine();
                }
            }
            First = false;
            const ImVec2 RowPos = ImGui::GetCursorScreenPos();
            const float SwatchTop = RowPos.y + (ImGui::GetTextLineHeight() - Swatch) * 0.5f;
            DrawList->AddRectFilled(ImVec2(RowPos.x, SwatchTop), ImVec2(RowPos.x + Swatch, SwatchTop + Swatch), WorkTypeColorFor(Ranked.Type), 2 * Scale);
            ImGui::Dummy(ImVec2(Swatch, ImGui::GetTextLineHeight()));
            ImGui::SameLine(0, 5 * Scale);
            ImGui::TextUnformatted(Ranked.Type.c_str());
        }
        ImGui::TextDisabled("%d entries in this period", Sum);
    }

    // Colors follow the all-time ranking of work types, so a type keeps its color in every chart and time range.
    ImU32 WorkTypeColorFor(const std::string& Type) const
    {
        const std::vector<ProjectLog::TypeCount>& AllTime = Log_.AllTimeCounts();
        for (size_t Index = 0; Index < AllTime.size(); Index++)
        {
            if (AllTime[Index].Type == Type)
            {
                return WorkTypeColor(Index);
            }
        }
        return WorkTypeColor(AllTime.size());
    }
    static ImU32 WorkTypeColor(const size_t Index)
    {
        static constexpr ImU32 PALETTE[] = {IM_COL32(86, 156, 214, 255),  IM_COL32(230, 145, 56, 255),  IM_COL32(106, 190, 120, 255),
                                            IM_COL32(214, 92, 92, 255),   IM_COL32(170, 120, 210, 255), IM_COL32(220, 200, 80, 255),
                                            IM_COL32(80, 200, 200, 255),  IM_COL32(220, 120, 180, 255), IM_COL32(150, 150, 110, 255),
                                            IM_COL32(120, 140, 230, 255), IM_COL32(180, 220, 100, 255), IM_COL32(160, 110, 80, 255)};
        return PALETTE[Index % (sizeof(PALETTE) / sizeof(PALETTE[0]))];
    }
    static int ResizeStringCallback(ImGuiInputTextCallbackData* const Data)
    {
        if (Data->EventFlag != ImGuiInputTextFlags_CallbackResize)
        {
            return 0;
        }
        std::string* const Text = static_cast<std::string*>(Data->UserData);
        Text->resize(static_cast<size_t>(Data->BufTextLen));
        Data->Buf = Text->data();
        return 0;
    }

    ProjectLog Log_;
    std::string ProjectId_;  // project whose log window is open; empty when closed
    std::string Notes_;
    bool NotesDirty_ = false;
    bool Focus_ = false;
    int TypeRange_ = 2;     // 0 today, 1 this week, 2 all time
    int GraphMonthly_ = 0;  // 0 day by day, 1 month by month
    double CheckAt_ = 0;
    float Scale_ = 1.0f;
    Palette Colors_;
};
