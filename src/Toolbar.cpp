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


float App::OverflowChipWidth() const
{
    return ImGui::CalcTextSize("...").x + 30.0f * UiScale_;
}

void App::DrawOverflowChip(const char* const PopupId, const std::vector<const Session*>& Hidden)
{
    const float Scale = UiScale_;
    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    const ImVec2 ChipPos = ImGui::GetCursorScreenPos();
    const float ChipWidth = OverflowChipWidth();
    const float ChipHeight = ImGui::GetFrameHeight();
    ImGui::PushID(PopupId);
    if (ImGui::InvisibleButton("##chip", ImVec2(ChipWidth, ChipHeight)) == true)
    {
        ImGui::OpenPopup(PopupId);
    }
    const bool Hovered = ImGui::IsItemHovered();
    if (Hovered == true)
    {
        int RunningCount = 0;
        for (const Session* const HiddenSession : Hidden)
        {
            if (IsSessionRunning(*HiddenSession) == true)
            {
                RunningCount++;
            }
        }
        ImGui::SetTooltip("%d more session%s, %d running", static_cast<int>(Hidden.size()), Hidden.size() == 1 ? "" : "s", RunningCount);
    }
    DrawList->AddRectFilled(ChipPos, ImVec2(ChipPos.x + ChipWidth, ChipPos.y + ChipHeight), ChipBgColorU32_, 6);
    if (Hovered == true)
    {
        DrawList->AddRect(ChipPos, ImVec2(ChipPos.x + ChipWidth, ChipPos.y + ChipHeight), UiHelpers::WithAlpha(TextDisabledColorU32_, 160), 6);
    }
    const Session* const Summary = SummarySession(Hidden);
    if (Summary != nullptr)
    {
        DrawStatusDot(DrawList, ImVec2(ChipPos.x + 12.0f * Scale, ChipPos.y + ChipHeight * 0.5f), 4.0f * Scale, *Summary);
    }
    const ImVec2 DotsSize = ImGui::CalcTextSize("...");
    DrawList->AddText(ImVec2(ChipPos.x + 22.0f * Scale, ChipPos.y + (ChipHeight - DotsSize.y) * 0.5f), TextColorU32_, "...");

    if (ImGui::BeginPopup(PopupId) == true)
    {
        std::string LastProjectId;
        for (const Session* const HiddenSession : Hidden)
        {
            if (HiddenSession->Rec.ProjectId != LastProjectId)
            {
                LastProjectId = HiddenSession->Rec.ProjectId;
                const Project* const HiddenProject = FindProject(LastProjectId);
                ImGui::SeparatorText(HiddenProject != nullptr ? HiddenProject->Name.c_str() : "");
            }
            const ImVec2 RowPos = ImGui::GetCursorScreenPos();
            const std::string Entry = "      " + HiddenSession->Rec.Name + "##" + HiddenSession->Rec.Id;
            if (ImGui::Selectable(Entry.c_str(), HiddenSession->Rec.Id == State_.Ui.ActiveId) == true)
            {
                OpenSession(HiddenSession->Rec.Id);
            }
            DrawStatusDot(ImGui::GetWindowDrawList(), ImVec2(RowPos.x + 8.0f * Scale, RowPos.y + ImGui::GetTextLineHeight() * 0.5f), 4.0f * Scale, *HiddenSession);
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void App::DrawToolbar()
{
    const float Scale = UiScale_;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * Scale, 6 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * Scale, 6 * Scale));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, SurfaceColorV4_);
    float const ToolbarHeight = ImGui::GetFrameHeight() + 12 * Scale;
    ImGui::BeginChild("##toolbar", ImVec2(0, ToolbarHeight), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* DrawList = ImGui::GetWindowDrawList();

    // right-aligned controls first so chips know how much room is left
    const char* const AllLabel = "All";
    const char* const RunningLabel = "Running";
    const char* const ActionLabel = "Requires action";
    const char* const TileLabel = "Tiles";
    const char* const TabLabel = "Tabs";
    float const PadX = ImGui::GetStyle().FramePadding.x * 2;
    const float SessionMeterWidth = 120.0f * Scale;
    const float WeeklyMeterWidth = 140.0f * Scale;
    const bool GitHubShown = State_.AppSettings.GitHubEnabled;
    const std::string GitHubText = GitHubShown == true ? GitHubLabel() : std::string();
    float RightWidth = ImGui::CalcTextSize(AllLabel).x + ImGui::CalcTextSize(RunningLabel).x + ImGui::CalcTextSize(ActionLabel).x +
                       ImGui::CalcTextSize(TileLabel).x + ImGui::CalcTextSize(TabLabel).x + ImGui::CalcTextSize("Settings").x + ImGui::CalcTextSize("Focus project").x + ImGui::GetFrameHeight() +
                       SessionMeterWidth + WeeklyMeterWidth + ImGui::CalcTextSize("Session:").x + ImGui::CalcTextSize("Weekly:").x +
                       2.0f * Scale * UiHelpers::USAGE_LABEL_GAP + 10.0f * Scale + 1.0f * Scale + ImGui::GetStyle().ItemSpacing.x + PadX * 6 + 48 * Scale;
    if (GitHubShown == true)
    {
        RightWidth += ImGui::CalcTextSize(GitHubText.c_str()).x + PadX + ImGui::GetStyle().ItemSpacing.x;
    }
    RightWidth += PauseButtonWidth() + ImGui::GetStyle().ItemSpacing.x;
    RightWidth += AutoButtonWidth() + ImGui::GetStyle().ItemSpacing.x;
    float const Avail = ImGui::GetContentRegionAvail().x;

    // Group open session chips under a header per project, enclosed in one visible container box
    // so the children read as nested inside their parent. Clicking a header narrows the tab/tile
    // pane area down to that project (State_.Ui.TabFilterProjectId), but every project's group
    // stays visible here — only the selected one's box is highlighted, so the others aren't hidden.
    const std::vector<std::string> BarIds = ActionFilteredIds();
    std::vector<std::string> ProjectOrder;
    for (const std::string& OpenId : BarIds)
    {
        const Session* const SessionPtr = FindSession(OpenId);
        if (SessionPtr == nullptr)
        {
            continue;
        }
        if (std::find(ProjectOrder.begin(), ProjectOrder.end(), SessionPtr->Rec.ProjectId) == ProjectOrder.end())
        {
            ProjectOrder.push_back(SessionPtr->Rec.ProjectId);
        }
    }

    const float ItemGap = ImGui::GetStyle().ItemSpacing.x;
    const float GroupPadding = 3.0f * Scale;
    const float GroupGap = 14.0f * Scale;
    const float MaxChipTextWidth = 170.0f * Scale;
    const float MoreChipWidth = OverflowChipWidth();
    const float OverflowReserve = MoreChipWidth + GroupGap;
    const float BarLimit = Avail - RightWidth;
    std::vector<const Session*> Overflow;
    bool AnyChip = false;
    for (const std::string& GroupId : ProjectOrder)
    {
        const Project* const ProjectRecord = FindProject(GroupId);
        if (ProjectRecord == nullptr)
        {
            continue;
        }

        std::vector<const Session*> ChildSessions;
        for (const std::string& ChildId : BarIds)
        {
            const Session* const SessionPtr = FindSession(ChildId);
            if (SessionPtr != nullptr && SessionPtr->Rec.ProjectId == GroupId)
            {
                ChildSessions.push_back(SessionPtr);
            }
        }

        if (Overflow.empty() == false)
        {
            Overflow.insert(Overflow.end(), ChildSessions.begin(), ChildSessions.end());
            continue;
        }

        const ImVec2 HeaderTextSize = ImGui::CalcTextSize(ProjectRecord->Name.c_str());
        const float HeaderCloseWidth = 20.0f * Scale;
        const float HeaderTextAreaWidth = HeaderTextSize.x + 16.0f * Scale;
        const float HeaderWidth = HeaderTextAreaWidth + HeaderCloseWidth;
        const float GroupStartX = ImGui::GetCursorPosX();
        if (GroupStartX + HeaderWidth + ItemGap + MoreChipWidth + GroupPadding * 2.0f + OverflowReserve > BarLimit)
        {
            Overflow.insert(Overflow.end(), ChildSessions.begin(), ChildSessions.end());
            continue;
        }
        // Long session names are shortened, and chips that don't fit go behind this group's own "..." chip.
        std::vector<std::string> ChipLabels;
        float GroupInnerWidth = HeaderWidth;
        for (size_t ChipIndex = 0; ChipIndex < ChildSessions.size(); ChipIndex++)
        {
            const std::string Label = TruncateToWidth(ChildSessions[ChipIndex]->Rec.Name, MaxChipTextWidth);
            const float ChipWidth = ImGui::CalcTextSize(Label.c_str()).x + 40.0f * Scale;
            const bool IsLastChip = ChipIndex + 1 == ChildSessions.size();
            const float MoreReserve = IsLastChip == true ? 0.0f : ItemGap + MoreChipWidth;
            if (GroupStartX + GroupInnerWidth + ItemGap + ChipWidth + MoreReserve + GroupPadding * 2.0f + OverflowReserve > BarLimit)
            {
                break;
            }
            GroupInnerWidth += ItemGap + ChipWidth;
            ChipLabels.push_back(Label);
        }
        const std::vector<const Session*> GroupHidden(ChildSessions.begin() + static_cast<std::ptrdiff_t>(ChipLabels.size()), ChildSessions.end());
        if (GroupHidden.empty() == false)
        {
            GroupInnerWidth += ItemGap + MoreChipWidth;
        }
        AnyChip = true;

        const bool IsSelected = State_.Ui.TabFilterProjectId == ProjectRecord->Id;
        const ImU32 ContainerBg = IsSelected ? UiHelpers::WithAlpha(AccentColor_, 35) : UiHelpers::WithAlpha(PaneBgColorU32_, 160);
        const ImU32 ContainerBorder = IsSelected ? AccentColor_ : UiHelpers::WithAlpha(TextDisabledColorU32_, 110);
        const ImVec2 GroupStart = ImGui::GetCursorScreenPos();
        const ImVec2 BoxMin(GroupStart.x - GroupPadding, GroupStart.y - GroupPadding);
        const ImVec2 BoxMax(GroupStart.x + GroupInnerWidth + GroupPadding, GroupStart.y + ImGui::GetFrameHeight() + GroupPadding);
        DrawList->AddRectFilled(BoxMin, BoxMax, ContainerBg, 8.0f);
        DrawList->AddRect(BoxMin, BoxMax, ContainerBorder, 8.0f, 0, IsSelected ? 2.0f : 1.0f);

        ImGui::PushID(ProjectRecord->Id.c_str());
        const ImVec2 ClosePos = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##projclose", ImVec2(HeaderCloseWidth, ImGui::GetFrameHeight())))
        {
            for (const Session* const ChildSession : ChildSessions)
            {
                HideSession(ChildSession->Rec.Id);
            }
            if (IsSelected == true)
            {
                SetTabFilter(std::string());
            }
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Close %s's open tabs", ProjectRecord->Name.c_str());
        }
        const ImU32 CloseColor = ImGui::IsItemHovered() == true ? WaitingColor_ : TextDisabledColorU32_;
        const ImVec2 CloseTextSize = ImGui::CalcTextSize("x");
        DrawList->AddText(ImVec2(ClosePos.x + (HeaderCloseWidth - CloseTextSize.x) * 0.5f,
                                 ClosePos.y + (ImGui::GetFrameHeight() - CloseTextSize.y) * 0.5f - ImGui::GetFontSize() * 0.12f),
                          CloseColor, "x");

        ImGui::SameLine(0, 0);
        const ImVec2 HeaderPos = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##projchip", ImVec2(HeaderTextAreaWidth, ImGui::GetFrameHeight())))
        {
            SetTabFilter(IsSelected ? std::string() : ProjectRecord->Id);
            ProjectScopeId_ = IsSelected == true ? std::string() : ProjectRecord->Id;
        }
        const ImU32 HeaderColor = IsSelected ? AccentColor_ : TextDisabledColorU32_;
        DrawList->AddText(ImVec2(HeaderPos.x + 4.0f * Scale, HeaderPos.y + (ImGui::GetFrameHeight() - HeaderTextSize.y) * 0.5f), HeaderColor,
                          ProjectRecord->Name.c_str());
        ImGui::PopID();
        ImGui::SameLine();

        for (size_t ChipIndex = 0; ChipIndex < ChipLabels.size(); ChipIndex++)
        {
            const Session* const ChildSession = ChildSessions[ChipIndex];
            const std::string& ChipLabel = ChipLabels[ChipIndex];
            const bool IsActiveChip = ChildSession->Rec.Id == State_.Ui.ActiveId && ProjectScopeId_.empty() == true;
            ImGui::PushID(ChildSession->Rec.Id.c_str());
            const ImVec2 ChipPos = ImGui::GetCursorScreenPos();
            const ImVec2 ChipTextSize = ImGui::CalcTextSize(ChipLabel.c_str());
            const float ChipWidth = ChipTextSize.x + 40.0f * Scale;
            const float ChipCloseWidth = 20.0f * Scale;
            if (ImGui::InvisibleButton("##chip", ImVec2(ChipWidth - ChipCloseWidth, ImGui::GetFrameHeight())))
            {
                const std::vector<std::string>::iterator ExtraAt = std::find(FilterExtraIds_.begin(), FilterExtraIds_.end(), ChildSession->Rec.Id);
                if (IsActiveChip == true && ExtraAt != FilterExtraIds_.end())
                {
                    FilterExtraIds_.erase(ExtraAt);
                    EnsureActiveVisible();
                    MarkDirty();
                }
                else
                {
                    OpenSession(ChildSession->Rec.Id);
                }
            }
            if (ImGui::IsItemHovered() == true && ChipLabel != ChildSession->Rec.Name)
            {
                ImGui::SetTooltip("%s", ChildSession->Rec.Name.c_str());
            }
            if (ImGui::BeginPopupContextItem("##chipmenu") == true)
            {
                if (ImGui::MenuItem("Close") == true)
                {
                    RequestCloseSession(ChildSession->Rec.Id);
                }
                if (ImGui::MenuItem("Close All") == true)
                {
                    for (const Session* const SiblingSession : ChildSessions)
                    {
                        HideSession(SiblingSession->Rec.Id);
                    }
                    if (IsSelected == true)
                    {
                        SetTabFilter(std::string());
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine(0, 0);
            const ImVec2 ChipClosePos = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##chipclose", ImVec2(ChipCloseWidth, ImGui::GetFrameHeight())) == true)
            {
                HideSession(ChildSession->Rec.Id);
            }
            const bool ChipCloseHovered = ImGui::IsItemHovered();
            if (ChipCloseHovered == true)
            {
                ImGui::SetTooltip("Hide tab");
            }
            DrawList->AddRectFilled(ChipPos, ImVec2(ChipPos.x + ChipWidth, ChipPos.y + ImGui::GetFrameHeight()), ChipBgColorU32_, 6);
            if (IsActiveChip == true)
            {
                DrawList->AddRect(ChipPos, ImVec2(ChipPos.x + ChipWidth, ChipPos.y + ImGui::GetFrameHeight()), AccentColor_, 6);
            }
            DrawStatusDot(DrawList, ImVec2(ChipPos.x + 12.0f * Scale, ChipPos.y + ImGui::GetFrameHeight() * 0.5f), 4.0f * Scale, *ChildSession);
            DrawList->AddText(ImVec2(ChipPos.x + 24.0f * Scale, ChipPos.y + (ImGui::GetFrameHeight() - ChipTextSize.y) * 0.5f), TextColorU32_,
                              ChipLabel.c_str());
            const ImVec2 ChipXSize = ImGui::CalcTextSize("x");
            DrawList->AddText(ImVec2(ChipClosePos.x + (ChipCloseWidth - ChipXSize.x) * 0.5f,
                                     ChipClosePos.y + (ImGui::GetFrameHeight() - ChipXSize.y) * 0.5f - ImGui::GetFontSize() * 0.12f),
                              ChipCloseHovered == true ? WaitingColor_ : TextDisabledColorU32_, "x");
            ImGui::SameLine();
            ImGui::PopID();
        }
        if (GroupHidden.empty() == false)
        {
            ImGui::PushID(ProjectRecord->Id.c_str());
            DrawOverflowChip("##groupmore", GroupHidden);
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::SameLine(0, GroupGap);
    }
    if (Overflow.empty() == false)
    {
        DrawOverflowChip("##morechips", Overflow);
    }
    else if (AnyChip == false)
    {
        ImGui::TextDisabled("No open sessions");
    }

    ImGui::SameLine(Avail - RightWidth + 8 * Scale);
    const std::function<void(const char*, const char*)> FilterButton = [&](const char* Label, const char* Mode)
    {
        bool const Active = State_.Ui.ActionFilter == Mode;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              Active ? ImGui::ColorConvertU32ToFloat4(UiHelpers::WithAlpha(AccentColor_, 90)) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
        bool const Clicked = ImGui::Button(Label);
        ImGui::PopStyleColor();
        if (Clicked)
        {
            State_.Ui.ActionFilter = Active ? "all" : Mode;
            MarkDirty();
        }
    };
    FilterButton(AllLabel, "all");
    ImGui::SameLine(0, 2);
    FilterButton(RunningLabel, "running");
    ImGui::SameLine(0, 2);
    FilterButton(ActionLabel, "action");
    ImGui::SameLine();
    bool const TileMode = State_.Ui.Mode == "tile";
    ImGui::PushStyleColor(ImGuiCol_Button,
                          TileMode ? ImGui::ColorConvertU32ToFloat4(UiHelpers::WithAlpha(AccentColor_, 90)) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::Button(TileLabel))
    {
        State_.Ui.Mode = "tile";
        MaximizedId_.clear();
        MarkDirty();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 2);
    ImGui::PushStyleColor(ImGuiCol_Button,
                          TileMode == false ? ImGui::ColorConvertU32ToFloat4(UiHelpers::WithAlpha(AccentColor_, 90)) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::Button(TabLabel))
    {
        State_.Ui.Mode = "tabs";
        MaximizedId_.clear();
        MarkDirty();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Checkbox("Focus project", &State_.Ui.FocusProjectOnClick) == true)
    {
        MarkDirty();
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Clicking a session in the sidebar shows only that project's open sessions");
    }
    ImGui::SameLine();
    const ImVec2 DividerPos = ImGui::GetCursorScreenPos();
    const float DividerHeight = ImGui::GetFrameHeight();
    ImGui::Dummy(ImVec2(1.0f * Scale, DividerHeight));
    DrawList->AddLine(ImVec2(DividerPos.x, DividerPos.y), ImVec2(DividerPos.x, DividerPos.y + DividerHeight),
                      ImGui::GetColorU32(ImGuiCol_Separator), 1.0f * Scale);
    ImGui::SameLine();

    bool UsageInFlight = false, UsageDone = false;
    UsageInfo CurrentUsage;
    ReadUsageFetch(UsageInFlight, UsageDone, CurrentUsage);
    const std::string UsageError = UsageDone == true && CurrentUsage.Ok == false
                                        ? (CurrentUsage.Error.empty() == false ? CurrentUsage.Error : "Could not fetch usage.")
                                        : std::string();
    const std::string SessionResetText = UiHelpers::FormatTimeUntil(CurrentUsage.SessionResetsAt);
    const std::string WeeklyResetText = UiHelpers::FormatTimeUntil(CurrentUsage.WeeklyResetsAt);
    const std::string WeeklyResetSuffix = WeeklyResetText.empty() == false ? "  |  " + WeeklyResetText : std::string();
    const std::string SessionResetSuffix = SessionResetText.empty() == false ? "  |  " + SessionResetText : std::string();
    bool UsageMeterClicked = false;
    if (DrawUsageMeter(DrawList, SessionMeterWidth, "Session", CurrentUsage.HasSession, CurrentUsage.SessionPct,
                       UsageFillColor(CurrentUsage.SessionPct),
                       UiHelpers::UsageResetProgress(CurrentUsage.SessionPct, UiHelpers::SecondsUntil(CurrentUsage.SessionResetsAt), UiHelpers::SESSION_WINDOW_SECONDS),
                       SessionResetSuffix, UsageError) == true)
    {
        UsageMeterClicked = true;
    }
    ImGui::SameLine(0, 10 * Scale);
    if (DrawUsageMeter(DrawList, WeeklyMeterWidth, "Weekly", CurrentUsage.HasWeekly, CurrentUsage.WeeklyPct,
                       UsageFillColor(CurrentUsage.WeeklyPct),
                       UiHelpers::UsageResetProgress(CurrentUsage.WeeklyPct, UiHelpers::SecondsUntil(CurrentUsage.WeeklyResetsAt), UiHelpers::WEEKLY_WINDOW_SECONDS),
                       WeeklyResetSuffix, UsageError) == true)
    {
        UsageMeterClicked = true;
    }
    if (UsageMeterClicked == true)
    {
        OpenModal(Modal::Usage);
    }
    ImGui::SameLine();
    if (GitHubShown == true)
    {
        const Git::GitHubStatus::State GitHubState = ReadGitHubStatus().Value;
        const ImVec4 GitHubColor = GitHubState == Git::GitHubStatus::State::LoggedIn ? ImGui::ColorConvertU32ToFloat4(DoneColor_)
                                   : GitHubState == Git::GitHubStatus::State::Unknown ? TextDisabledColorV4_
                                                                                      : WaitingColorV4_;
        ImGui::PushStyleColor(ImGuiCol_Text, GitHubColor);
        const bool GitHubClicked = ImGui::Button(GitHubText.c_str());
        ImGui::PopStyleColor();
        if (GitHubClicked == true)
        {
            GitHubLastCheckAt_ = -1.0e9;
            OpenModal(Modal::GitHub);
        }
        ImGui::SameLine();
    }
    DrawPauseButton();
    ImGui::SameLine();
    DrawAutoButton();
    ImGui::SameLine();
    if (ImGui::Button("Settings"))
    {
        std::snprintf(CommandBuffer_, sizeof CommandBuffer_, "%s", State_.AppSettings.ClaudeCommand.c_str());
        FontSizeEdit_ = State_.AppSettings.FontSize;
        PaneTitleScaleEdit_ = State_.AppSettings.PaneTitleScale;
        NotifyEdit_ = State_.AppSettings.Notifications;
        AutoStartEdit_ = State_.AppSettings.AutoStartWindow;
        GitHubEdit_ = State_.AppSettings.GitHubEnabled;
        KeysEdit_ = State_.AppSettings.Keys;
        KeyCaptureIndex_ = -1;
        KeyCaptureError_.clear();
        ThemeEdit_ = State_.AppSettings.Theme;
        SessionNameEdits_.clear();
        for (const std::unique_ptr<Session>& SessionPtr : Sessions_)
        {
            std::array<char, 256> Buffer{};
            std::snprintf(Buffer.data(), Buffer.size(), "%s", SessionPtr->Rec.Name.c_str());
            SessionNameEdits_[SessionPtr->Rec.Id] = Buffer;
        }
        OpenModal(Modal::Settings);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

float App::PauseButtonWidth() const
{
    return std::max(ImGui::CalcTextSize(PAUSE_LABEL).x, ImGui::CalcTextSize("Continue 00").x) + ImGui::GetStyle().FramePadding.x * 2;
}

void App::DrawPauseButton()
{
    const ImVec2 Size(PauseButtonWidth(), 0);
    if (PausedIds_.empty() == false)
    {
        const std::string ContinueLabel = "Continue " + std::to_string(PausedIds_.size()) + "###pauseToggle";
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(UiHelpers::WithAlpha(WaitingColor_, 110)));
        const bool Clicked = ImGui::Button(ContinueLabel.c_str(), Size);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Send \"continue\" to the %d session%s you paused", static_cast<int>(PausedIds_.size()), PausedIds_.size() == 1 ? "" : "s");
        }
        if (Clicked == true)
        {
            ContinuePausedSessions();
        }
        return;
    }
    int WorkingCount = 0;
    for (const std::unique_ptr<Session>& SessionPtr : Sessions_)
    {
        if (SessionPtr->IsRunning() == true && SessionPtr->CurrentStatus == Status::Busy)
        {
            WorkingCount++;
        }
    }
    ImGui::BeginDisabled(WorkingCount == 0);
    const std::string PauseLabel = std::string(PAUSE_LABEL) + "###pauseToggle";
    const bool Clicked = ImGui::Button(PauseLabel.c_str(), Size);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) == true)
    {
        ImGui::SetTooltip(WorkingCount == 0 ? "No sessions are working right now" : "Send \"Pause\" to the %d working session%s", WorkingCount, WorkingCount == 1 ? "" : "s");
    }
    if (Clicked == true)
    {
        PauseWorkingSessions();
    }
}

void App::DrawAutoButton()
{
    if (AutoMode_ == true)
    {
        // Pulse so it's obvious from across the room that sessions are running unattended.
        const float Pulse = 0.5f + 0.5f * std::sin(static_cast<float>(Now_) * 2.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(UiHelpers::WithAlpha(DoneColor_, static_cast<int>(150 + 90 * Pulse))));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(DoneColor_));
    }
    const bool Clicked = ImGui::Button("Auto##autoMode", ImVec2(AutoButtonWidth(), 0));
    if (AutoMode_ == true)
    {
        ImGui::PopStyleColor(2);
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("%s", AutoMode_ == true
                                    ? "Auto mode is ON: sessions keep working without stopping to ask you, and get a nudge to\n"
                                      "continue whenever they finish a turn. Click to turn it off (sessions are told you're back)."
                                    : "Turn on when you're stepping away: every session is told to keep working without asking\n"
                                      "you, making sensible choices itself (nothing destructive), and is nudged to continue when it\n"
                                      "stops. Approve/deny permission prompts still wait for you.");
    }
    if (Clicked == true)
    {
        SetAutoMode(AutoMode_ == false);
    }
}

void App::PauseWorkingSessions()
{
    PausedIds_.clear();
    for (const std::unique_ptr<Session>& SessionPtr : Sessions_)
    {
        if (SessionPtr->IsRunning() == false || SessionPtr->CurrentStatus != Status::Busy)
        {
            continue;
        }
        SendPrompt(*SessionPtr, "Pause");
        PausedIds_.push_back(SessionPtr->Rec.Id);
    }
    ShowToast("Paused " + std::to_string(PausedIds_.size()) + " session" + (PausedIds_.size() == 1 ? "" : "s"));
}

void App::ContinuePausedSessions()
{
    int Continued = 0;
    for (const std::string& Id : PausedIds_)
    {
        Session* const Paused = FindSession(Id);
        if (Paused == nullptr || Paused->IsRunning() == false)
        {
            continue;
        }
        // Typing into an approval menu would pick one of its options, so wait until the session is idle instead.
        if (Paused->CurrentStatus == Status::Waiting)
        {
            Paused->PendingPrompt = "continue";
        }
        else
        {
            SendPrompt(*Paused, "continue");
        }
        Continued++;
    }
    PausedIds_.clear();
    ShowToast("Continued " + std::to_string(Continued) + " session" + (Continued == 1 ? "" : "s"));
}
