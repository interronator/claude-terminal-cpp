#pragma once
#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>

#include "Platform.h"
#include "ProjectLog.h"
#include "ProjectLogCharts.h"
#include "Store.h"
#include "UiHelpers.h"
#include "imgui.h"

// The floating Project Log window: summary figures for a project's PROJECT_LOG.md, a filterable timeline grouped by
// hour, day or week, the work-type breakdown and activity charts, and the editable notes section.
class ProjectLogWindow
{
public:
    using Palette = ProjectLogPalette;

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
        const Project& LogProject = *Found;
        Scale_ = Scale;
        Colors_ = Colors;
        if (Now >= CheckAt_)
        {
            CheckAt_ = Now + 1.0;
            if (Log_.Refresh(LogProject.Path) == true && NotesDirty_ == false)
            {
                Notes_ = Log_.Notes();
            }
        }

        ImGuiViewport* const Viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(820 * Scale, Viewport->WorkSize.x * 0.9f), std::min(640 * Scale, Viewport->WorkSize.y * 0.85f)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(460 * Scale, 320 * Scale), ImVec2(Viewport->WorkSize.x, Viewport->WorkSize.y));
        if (Focus_ == true)
        {
            ImGui::SetNextWindowFocus();
            Focus_ = false;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * Scale, 14 * Scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * Scale);
        bool IsOpen = true;
        const std::string Title = "Project log - " + LogProject.Name + "###projectLog";
        if (ImGui::Begin(Title.c_str(), &IsOpen, ImGuiWindowFlags_NoCollapse) == true)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * Scale, 6 * Scale));
            DrawHeader(Projects, LogProject);
            ImGui::Spacing();
            DrawSummary();
            ImGui::Spacing();
            ImGui::Spacing();
            if (ImGui::BeginTabBar("##logTabs") == true)
            {
                if (ImGui::BeginTabItem("Timeline") == true)
                {
                    ImGui::Spacing();
                    DrawTimeline();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Breakdown") == true)
                {
                    ImGui::Spacing();
                    Charts_.DrawBreakdown(Log_, Scale_, Colors_);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Activity") == true)
                {
                    ImGui::Spacing();
                    Charts_.DrawActivity(Log_, Scale_, Colors_);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(NotesDirty_ == true ? "Notes*###notesTab" : "Notes###notesTab") == true)
                {
                    ImGui::Spacing();
                    DrawNotes(LogProject, Error);
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
    void DrawHeader(const std::vector<Project>& Projects, const Project& LogProject)
    {
        ImGui::SetNextItemWidth(std::min(260 * Scale_, ImGui::GetContentRegionAvail().x * 0.5f));
        if (ImGui::BeginCombo("##logProject", LogProject.Name.c_str()) == true)
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
        const std::string LogPath = ProjectLog::PathFor(LogProject.Path).string();
        const char* const ShowLabel = "Show file";
        const float ShowWidth = ImGui::CalcTextSize(ShowLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ShowWidth);
        if (ImGui::Button(ShowLabel) == true)
        {
            if (Log_.FileExists() == true)
            {
                Platform::ShowInFolder(LogPath);
            }
            else
            {
                Platform::Reveal(LogProject.Path);
            }
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("%s", LogPath.c_str());
        }
    }

    void DrawSummary()
    {
        constexpr int CARD_COUNT = 4;
        const std::time_t TodayStart = ProjectLog::StartOfToday(false);
        const std::time_t WeekStart = ProjectLog::StartOfToday(true);
        int TodayCount = 0;
        int WeekCount = 0;
        std::time_t Latest = 0;
        for (const ProjectLog::Entry& Item : Log_.Entries())
        {
            TodayCount += Item.When >= TodayStart ? 1 : 0;
            WeekCount += Item.When >= WeekStart ? 1 : 0;
            Latest = std::max(Latest, Item.When);
        }
        const std::string Values[CARD_COUNT] = {std::to_string(TodayCount), std::to_string(WeekCount), std::to_string(Log_.Entries().size()),
                                                Latest == 0 ? std::string("-") : UiHelpers::FormatAgo(Latest)};
        const char* const Labels[CARD_COUNT] = {"Today", "This week", "All time", "Last entry"};

        const float Gap = 10 * Scale_;
        const ImVec2 Padding(12 * Scale_, 8 * Scale_);
        const float LineHeight = ImGui::GetTextLineHeight();
        const float CardWidth = (ImGui::GetContentRegionAvail().x - Gap * (CARD_COUNT - 1)) / CARD_COUNT;
        const float CardHeight = LineHeight * 2.0f + Padding.y * 2.0f + 3 * Scale_;
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        ImDrawList* const DrawList = ImGui::GetWindowDrawList();
        for (int Index = 0; Index < CARD_COUNT; Index++)
        {
            const ImVec2 CardMin(Origin.x + (CardWidth + Gap) * static_cast<float>(Index), Origin.y);
            const ImVec2 CardMax(CardMin.x + CardWidth, CardMin.y + CardHeight);
            DrawList->AddRectFilled(CardMin, CardMax, UiHelpers::WithAlpha(Colors_.TextU32, 10), 6 * Scale_);
            DrawList->AddRect(CardMin, CardMax, UiHelpers::WithAlpha(Colors_.TextU32, 22), 6 * Scale_);
            DrawList->PushClipRect(CardMin, CardMax, true);
            DrawList->AddText(ImVec2(CardMin.x + Padding.x, CardMin.y + Padding.y), Colors_.TextDisabledU32, Labels[Index]);
            DrawList->AddText(ImVec2(CardMin.x + Padding.x, CardMin.y + Padding.y + LineHeight + 3 * Scale_), Colors_.TextU32, Values[Index].c_str());
            DrawList->PopClipRect();
        }
        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, CardHeight));
    }

    void DrawTimeline()
    {
        constexpr size_t OPEN_GROUP_COUNT = 3;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Group by");
        ImGui::SameLine();
        UiHelpers::SegmentedControl("##logGrouping", {"Hour", "Day", "Week"}, Grouping_, ImGui::ColorConvertFloat4ToU32(Colors_.Accent));
        const float FilterWidth = std::min(420 * Scale_, ImGui::GetContentRegionAvail().x * 0.35f);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - FilterWidth);
        ImGui::SetNextItemWidth(FilterWidth);
        ImGui::InputTextWithHint("##logFilter", "Filter entries", Filter_, sizeof Filter_);
        ImGui::Spacing();

        ImGui::BeginChild("##logEntries", ImVec2(0, 0), ImGuiChildFlags_None);
        const ProjectLog::Grouping By = Grouping_ == 0 ? ProjectLog::Grouping::Hour : (Grouping_ == 1 ? ProjectLog::Grouping::Day : ProjectLog::Grouping::Week);
        const std::vector<ProjectLog::Group> Groups = Log_.Grouped(By);
        if (Groups.empty() == true)
        {
            ProjectLogCharts::DrawEmptyState(Log_.FileExists() == true
                                                 ? "Nothing has been logged yet. Sessions add a line here whenever they finish a piece of work."
                                                 : "No log yet. Sessions create the file and add a line here whenever they finish a piece of work. "
                                                   "Sessions that were already running before this was added need a Restart to pick it up.",
                                             Scale_);
            ImGui::EndChild();
            return;
        }
        const std::string Needle = Lowered(Filter_);
        const float TimeWidth = ImGui::CalcTextSize(By == ProjectLog::Grouping::Week ? "0000-00-00 00:00" : "00:00").x;
        const float TypeWidth = TypeColumnWidth();
        const float SessionWidth = SessionColumnWidth();
        size_t Shown = 0;
        for (size_t GroupIndex = 0; GroupIndex < Groups.size(); GroupIndex++)
        {
            const ProjectLog::Group& Current = Groups[GroupIndex];
            std::vector<const ProjectLog::Entry*> Visible;
            for (const ProjectLog::Entry& Item : Current.Entries)
            {
                if (Matches(Item, Needle) == true)
                {
                    Visible.push_back(&Item);
                }
            }
            if (Visible.empty() == true)
            {
                continue;
            }
            Shown += Visible.size();
            ImGui::PushID(Current.Title.c_str());
            if (Needle.empty() == false)
            {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            }
            if (DrawGroupHeader(Current.Title, Visible.size(), GroupIndex < OPEN_GROUP_COUNT) == true)
            {
                DrawEntryTable(Visible, By, TimeWidth, TypeWidth, SessionWidth);
            }
            ImGui::PopID();
        }
        if (Shown == 0)
        {
            ProjectLogCharts::DrawEmptyState("No entries match the filter.", Scale_);
        }
        ImGui::EndChild();
    }

    bool DrawGroupHeader(const std::string& Title, const size_t Count, const bool DefaultOpen)
    {
        const ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                         (DefaultOpen == true ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None);
        const bool Open = ImGui::TreeNodeEx("##group", Flags, "%s", Title.c_str());
        const ImVec2 HeaderMin = ImGui::GetItemRectMin();
        const ImVec2 HeaderMax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(HeaderMin.x, HeaderMax.y), HeaderMax, UiHelpers::WithAlpha(Colors_.TextU32, 28));
        const std::string CountText = std::to_string(Count) + (Count == 1 ? " entry" : " entries");
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(CountText.c_str()).x - ImGui::GetStyle().FramePadding.x);
        ImGui::TextDisabled("%s", CountText.c_str());
        return Open;
    }

    void DrawEntryTable(const std::vector<const ProjectLog::Entry*>& Visible, const ProjectLog::Grouping By, const float TimeWidth, const float TypeWidth,
                        const float SessionWidth)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8 * Scale_, 5 * Scale_));
        const ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##entries", 4, Flags) == true)
        {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, TimeWidth);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, TypeWidth);
            ImGui::TableSetupColumn("Session", ImGuiTableColumnFlags_WidthFixed, SessionWidth);
            ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthStretch);
            for (const ProjectLog::Entry* const Item : Visible)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const size_t TimeAt = Item->Stamp.find(' ');
                const std::string When = By == ProjectLog::Grouping::Week || TimeAt == std::string::npos ? Item->Stamp : Item->Stamp.substr(TimeAt + 1);
                ImGui::TextDisabled("%s", When.c_str());
                ImGui::TableSetColumnIndex(1);
                if (Item->Type.empty() == false)
                {
                    DrawTypePill(Item->Type);
                }
                ImGui::TableSetColumnIndex(2);
                if (Item->SessionName.empty() == false)
                {
                    ImGui::TextColored(Colors_.Accent, "%s", Item->SessionName.c_str());
                    if (ImGui::IsItemHovered() == true && ImGui::CalcTextSize(Item->SessionName.c_str()).x > SessionWidth)
                    {
                        ImGui::SetTooltip("%s", Item->SessionName.c_str());
                    }
                }
                ImGui::TableSetColumnIndex(3);
                ImGui::TextWrapped("%s", Item->Text.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        ImGui::Spacing();
    }

    void DrawTypePill(const std::string& Type)
    {
        const ImVec2 TextSize = ImGui::CalcTextSize(Type.c_str());
        const float PadX = PillPadding();
        const ImVec2 Min = ImGui::GetCursorScreenPos();
        const ImVec2 Max(Min.x + TextSize.x + PadX * 2.0f, Min.y + TextSize.y);
        const ImU32 Color = ProjectLogCharts::WorkTypeColorFor(Log_, Type);
        ImDrawList* const DrawList = ImGui::GetWindowDrawList();
        DrawList->AddRectFilled(ImVec2(Min.x, Min.y - 1 * Scale_), ImVec2(Max.x, Max.y + 1 * Scale_), UiHelpers::WithAlpha(Color, 46), TextSize.y * 0.5f + 1 * Scale_);
        DrawList->AddText(ImVec2(Min.x + PadX, Min.y), Color, Type.c_str());
        ImGui::Dummy(ImVec2(Max.x - Min.x, Max.y - Min.y));
    }

    void DrawNotes(const Project& LogProject, std::string& Error)
    {
        ImGui::TextDisabled("Shared with every session in this project. Sessions read these notes when they start.");
        ImGui::Spacing();
        const ImGuiStyle& Style = ImGui::GetStyle();
        const float FooterHeight = ImGui::GetFrameHeight() + Style.ItemSpacing.y * 2.0f;
        if (ImGui::InputTextMultiline("##logNotes", Notes_.data(), Notes_.capacity() + 1, ImVec2(-1, -FooterHeight), ImGuiInputTextFlags_CallbackResize,
                                      ResizeStringCallback, &Notes_) == true)
        {
            NotesDirty_ = true;
        }
        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        if (NotesDirty_ == true)
        {
            ImGui::TextColored(Colors_.Accent, "Unsaved changes");
        }
        else
        {
            ImGui::TextDisabled("All changes saved");
        }
        const char* const DiscardLabel = "Discard";
        const char* const SaveLabel = "Save notes";
        const float ButtonsWidth = ImGui::CalcTextSize(DiscardLabel).x + ImGui::CalcTextSize(SaveLabel).x + Style.FramePadding.x * 4.0f + Style.ItemSpacing.x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ButtonsWidth);
        ImGui::BeginDisabled(NotesDirty_ == false);
        if (ImGui::Button(DiscardLabel) == true)
        {
            Notes_ = Log_.Notes();
            NotesDirty_ = false;
        }
        ImGui::SameLine();
        const ImVec4 Accent = Colors_.Accent;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(Accent.x, Accent.y, Accent.z, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(Accent.x, Accent.y, Accent.z, 0.75f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(Accent.x, Accent.y, Accent.z, 0.9f));
        if (ImGui::Button(SaveLabel) == true)
        {
            SaveNotes(LogProject, Error);
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
    }

    void SaveNotes(const Project& LogProject, std::string& Error)
    {
        if (Log_.SaveNotes(LogProject.Path, Notes_) == false)
        {
            Error = "Could not write " + ProjectLog::PathFor(LogProject.Path).string();
            return;
        }
        NotesDirty_ = false;
        Notes_ = Log_.Notes();
    }

    float PillPadding() const
    {
        return 7 * Scale_;
    }

    float TypeColumnWidth() const
    {
        float Widest = 0.0f;
        for (const ProjectLog::TypeCount& Ranked : Log_.AllTimeCounts())
        {
            Widest = std::max(Widest, ImGui::CalcTextSize(Ranked.Type.c_str()).x);
        }
        return Widest + PillPadding() * 2.0f;
    }

    float SessionColumnWidth() const
    {
        float Widest = 0.0f;
        for (const ProjectLog::Entry& Item : Log_.Entries())
        {
            Widest = std::max(Widest, ImGui::CalcTextSize(Item.SessionName.c_str()).x);
        }
        return std::min(Widest, ImGui::GetContentRegionAvail().x * 0.22f);
    }

    static std::string Lowered(const std::string& Text)
    {
        std::string Result = Text;
        std::transform(Result.begin(), Result.end(), Result.begin(), [](const unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
        return Result;
    }

    static bool Matches(const ProjectLog::Entry& Item, const std::string& Needle)
    {
        if (Needle.empty() == true)
        {
            return true;
        }
        return Lowered(Item.Text).find(Needle) != std::string::npos || Lowered(Item.SessionName).find(Needle) != std::string::npos ||
               Lowered(Item.Type).find(Needle) != std::string::npos;
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
    ProjectLogCharts Charts_;
    std::string ProjectId_;  // project whose log window is open; empty when closed
    std::string Notes_;
    char Filter_[128] = {};
    bool NotesDirty_ = false;
    bool Focus_ = false;
    int Grouping_ = 1;  // 0 hour, 1 day, 2 week
    double CheckAt_ = 0;
    float Scale_ = 1.0f;
    Palette Colors_;
};
