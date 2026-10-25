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


void App::DrawProjectLog()
{
    if (ProjectLogId_.empty() == true)
    {
        return;
    }
    const Project* const LogProject = FindProject(ProjectLogId_);
    if (LogProject == nullptr)
    {
        ProjectLogId_.clear();
        return;
    }
    if (Now_ >= ProjectLogCheckAt_)
    {
        ProjectLogCheckAt_ = Now_ + 1.0;
        if (ProjectLog_.Refresh(LogProject->Path) == true && ProjectLogNotesDirty_ == false)
        {
            ProjectLogNotes_ = ProjectLog_.Notes();
        }
    }

    const float Scale = UiScale_;
    ImGuiViewport* const Viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760 * Scale, Viewport->WorkSize.x * 0.9f), std::min(620 * Scale, Viewport->WorkSize.y * 0.85f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(380 * Scale, 260 * Scale), ImVec2(Viewport->WorkSize.x, Viewport->WorkSize.y));
    if (ProjectLogFocus_ == true)
    {
        ImGui::SetNextWindowFocus();
        ProjectLogFocus_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * Scale, 12 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * Scale);
    bool Open = true;
    const std::string Title = "Project log - " + LogProject->Name + "###projectLog";
    if (ImGui::Begin(Title.c_str(), &Open, ImGuiWindowFlags_NoCollapse) == true)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * Scale, 6 * Scale));
        ImGui::SetNextItemWidth(220 * Scale);
        if (ImGui::BeginCombo("##logProject", LogProject->Name.c_str()) == true)
        {
            for (const Project& Candidate : State_.Projects)
            {
                if (ImGui::Selectable(Candidate.Name.c_str(), Candidate.Id == ProjectLogId_) == true && Candidate.Id != ProjectLogId_)
                {
                    OpenProjectLog(Candidate.Id);
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
                DrawProjectLogEntries(ProjectLog::Grouping::Hour);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("By day") == true)
            {
                DrawProjectLogEntries(ProjectLog::Grouping::Day);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("By week") == true)
            {
                DrawProjectLogEntries(ProjectLog::Grouping::Week);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("By type") == true)
            {
                DrawProjectLogTypeChart();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Graph") == true)
            {
                DrawProjectLogGraph();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(ProjectLogNotesDirty_ == true ? "Notes*###notesTab" : "Notes###notesTab") == true)
            {
                ImGui::TextDisabled("Notes are shared with every session in this project; they read them when they start.");
                const float ButtonRow = ImGui::GetFrameHeightWithSpacing();
                if (ImGui::InputTextMultiline("##logNotes", ProjectLogNotes_.data(), ProjectLogNotes_.capacity() + 1, ImVec2(-1, -ButtonRow),
                                              ImGuiInputTextFlags_CallbackResize, ResizeStringCallback, &ProjectLogNotes_) == true)
                {
                    ProjectLogNotesDirty_ = true;
                }
                ImGui::BeginDisabled(ProjectLogNotesDirty_ == false);
                if (ImGui::Button("Save notes") == true)
                {
                    if (ProjectLog_.SaveNotes(LogProject->Path, ProjectLogNotes_) == true)
                    {
                        ProjectLogNotesDirty_ = false;
                        ProjectLogNotes_ = ProjectLog_.Notes();
                    }
                    else
                    {
                        ShowToast("Could not write " + ProjectLog::PathFor(LogProject->Path).string(), true);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Discard changes") == true)
                {
                    ProjectLogNotes_ = ProjectLog_.Notes();
                    ProjectLogNotesDirty_ = false;
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
    if (Open == false)
    {
        ProjectLogId_.clear();
    }
}

void App::DrawProjectLogEntries(const ProjectLog::Grouping By)
{
    ImGui::BeginChild("##logEntries", ImVec2(0, 0), ImGuiChildFlags_None);
    const std::vector<ProjectLog::Group> Groups = ProjectLog_.Grouped(By);
    if (Groups.empty() == true)
    {
        ImGui::TextWrapped("%s", ProjectLog_.FileExists() == true
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
                ImGui::TextColored(AccentColorV4_, "%s", Item.SessionName.c_str());
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

void App::DrawProjectLogTypeChart()
{
    const float Scale = UiScale_;
    ImGui::RadioButton("Today", &ProjectLogTypeRange_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("This week", &ProjectLogTypeRange_, 1);
    ImGui::SameLine();
    ImGui::RadioButton("All time", &ProjectLogTypeRange_, 2);
    const std::time_t Since = ProjectLogTypeRange_ == 2 ? 0 : ProjectLog::StartOfToday(ProjectLogTypeRange_ == 1);
    const std::vector<ProjectLog::TypeCount> Counts = ProjectLog_.CountByType(Since);
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
            DrawList->AddLine(Center, ImVec2(Center.x + std::cos(Edge) * SliceRadius, Center.y + std::sin(Edge) * SliceRadius), PaneBgColorU32_, 2 * Scale);
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
        const ImVec4 TextColor = static_cast<int>(Index) == HoveredIndex ? AccentColorV4_ : TextColorV4_;
        ImGui::TextColored(TextColor, "%s", Counts[Index].Type.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%d  (%d%%)", Counts[Index].Count, (Counts[Index].Count * 100 + Total / 2) / Total);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%d entries", Total);
    ImGui::EndGroup();
}

void App::DrawProjectLogGraph()
{
    constexpr int DAY_COUNT = 30;
    constexpr int MONTH_COUNT = 12;
    constexpr int GRID_LINES = 4;
    const float Scale = UiScale_;
    ImGui::RadioButton("Day by day (30 days)", &ProjectLogGraphMonthly_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Month by month (12 months)", &ProjectLogGraphMonthly_, 1);
    const bool Monthly = ProjectLogGraphMonthly_ == 1;
    const std::vector<ProjectLog::Bucket> Buckets = ProjectLog_.Buckets(Monthly, Monthly == true ? MONTH_COUNT : DAY_COUNT);
    const std::vector<ProjectLog::TypeCount>& AllTime = ProjectLog_.AllTimeCounts();

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
        DrawList->AddLine(ImVec2(PlotLeft, Y), ImVec2(PlotLeft + PlotWidth, Y), UiHelpers::WithAlpha(TextDisabledColorU32_, Line == 0 ? 160 : 50));
        const std::string Value = std::to_string(AxisMax * Line / GRID_LINES);
        const ImVec2 ValueSize = ImGui::CalcTextSize(Value.c_str());
        DrawList->AddText(ImVec2(PlotLeft - ValueSize.x - 6 * Scale, Y - ValueSize.y * 0.5f), TextDisabledColorU32_, Value.c_str());
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
            DrawList->AddRectFilled(ImVec2(SlotLeft, PlotTop), ImVec2(SlotLeft + Slot, PlotBottom), UiHelpers::WithAlpha(TextColorU32_, 18));
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
        DrawList->AddText(ImVec2(SlotLeft + (Slot - LabelSize.x) * 0.5f, PlotBottom + 4 * Scale), IsLast == true ? TextColorU32_ : TextDisabledColorU32_,
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
