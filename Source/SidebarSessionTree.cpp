#include "App.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "UiHelpers.h"

std::vector<App::SidebarRow> App::BuildSidebarRows(const std::string& ProjectId)
{
    std::vector<Session*> InProject;
    for (const std::unique_ptr<Session>& Candidate : Sessions_)
    {
        if (Candidate->Rec.ProjectId == ProjectId)
        {
            InProject.push_back(Candidate.get());
        }
    }
    std::set<std::string> Placed;
    std::vector<SidebarRow> Rows;
    for (Session* const Candidate : InProject)
    {
        const std::string& ParentId = Candidate->Rec.ParentId;
        const bool ParentShown = ParentId.empty() == false && ParentId != Candidate->Rec.Id &&
                                 std::any_of(InProject.begin(), InProject.end(), [&ParentId](const Session* const Other) { return Other->Rec.Id == ParentId; });
        if (ParentShown == true)
        {
            continue;
        }
        AppendSidebarRow(*Candidate, 0, InProject, Placed, Rows);
    }
    for (Session* const Candidate : InProject)
    {
        if (Placed.count(Candidate->Rec.Id) == 0)
        {
            AppendSidebarRow(*Candidate, 0, InProject, Placed, Rows);
        }
    }
    return Rows;
}

void App::AppendSidebarRow(Session& Current, const int Depth, const std::vector<Session*>& InProject, std::set<std::string>& Placed, std::vector<SidebarRow>& Rows)
{
    Placed.insert(Current.Rec.Id);
    std::vector<Session*> Children;
    for (Session* const Candidate : InProject)
    {
        if (Candidate->Rec.ParentId == Current.Rec.Id && Placed.count(Candidate->Rec.Id) == 0)
        {
            Children.push_back(Candidate);
        }
    }
    const size_t RowIndex = Rows.size();
    SidebarRow Row;
    Row.Target = &Current;
    Row.Depth = Depth;
    Row.HasChildren = Children.empty() == false;
    Rows.push_back(Row);
    if (Children.empty() == true)
    {
        return;
    }

    const std::map<std::string, bool>::const_iterator Folded = State_.Ui.Collapsed.find("children:" + Current.Rec.Id);
    const bool ChildrenHidden = Folded != State_.Ui.Collapsed.end() && Folded->second == true;
    std::vector<SidebarRow> Below;
    for (Session* const Child : Children)
    {
        if (Placed.count(Child->Rec.Id) == 0)
        {
            AppendSidebarRow(*Child, Depth + 1, InProject, Placed, Below);
        }
    }
    if (ChildrenHidden == false)
    {
        Rows.insert(Rows.end(), Below.begin(), Below.end());
        return;
    }
    Rows[RowIndex].HiddenChildren = static_cast<int>(Below.size());
    for (const SidebarRow& Hidden : Below)
    {
        if (Hidden.Target->CurrentStatus == Status::Busy || Hidden.Target->CurrentStatus == Status::Waiting)
        {
            Rows[RowIndex].HiddenChildBusy = true;
        }
    }
}

void App::DrawSidebarSessionRow(const SidebarRow& Row, std::string& DragSourceId, std::string& DragTargetId)
{
    const float Scale = UiScale_;
    Session& CurrentSession = *Row.Target;
    const float DepthIndent = 14.0f * Scale * static_cast<float>(Row.Depth);
    ImGui::PushID(CurrentSession.Rec.Id.c_str());
    const bool IsOpen = std::find(State_.Ui.OpenIds.begin(), State_.Ui.OpenIds.end(), CurrentSession.Rec.Id) != State_.Ui.OpenIds.end();
    const bool IsActive = State_.Ui.ActiveId == CurrentSession.Rec.Id;
    const float RowHeight = ImGui::GetFrameHeight();
    const float RowWidth = ImGui::GetContentRegionAvail().x - 26 * Scale - DepthIndent;
    ImGui::Indent(12 * Scale + DepthIndent);
    const ImVec2 RowPos = ImGui::GetCursorScreenPos();
    const float CloseWidth = RowCloseButtonWidth();
    const float SelectableWidth = RowWidth - 10 * Scale + CloseWidth;
    const ImVec2 ClosePos(RowPos.x + SelectableWidth - CloseWidth, RowPos.y);
    if (ImGui::IsRectVisible(RowPos, ImVec2(RowPos.x + SelectableWidth, RowPos.y + RowHeight)) == true)
    {
        PeerVeins_.AddRow(CurrentSession.Rec.Id, RowPos, ImVec2(RowPos.x + SelectableWidth, RowPos.y + RowHeight));
    }
    const bool CloseHovered =
        ImGui::IsWindowHovered() == true && ImGui::IsMouseHoveringRect(ClosePos, ImVec2(ClosePos.x + CloseWidth, ClosePos.y + RowHeight)) == true;
    // Hovering the "x" steals hover from the row, so paint the row as hovered ourselves to keep them one piece.
    const ImU32 ActiveFill = ImGui::GetColorU32(ImGuiCol_Header);
    const ImU32 HoverFill = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
    const ImU32 PressFill = ImGui::GetColorU32(ImGuiCol_HeaderActive);
    const ImVec4 Clear(0, 0, 0, 0);
    ImGui::PushStyleColor(ImGuiCol_Header, Clear);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Clear);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, Clear);
    const bool RowClicked = ImGui::Selectable("##s", IsActive, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
                                              ImVec2(SelectableWidth, RowHeight));
    ImGui::PopStyleColor(3);
    const bool RowHovered = ImGui::IsItemHovered() == true || CloseHovered == true;
    ImU32 FillColor = 0;
    if (ImGui::IsItemActive() == true)
    {
        FillColor = PressFill;
    }
    else if (RowHovered == true)
    {
        FillColor = HoverFill;
    }
    else if (IsActive == true)
    {
        FillColor = ActiveFill;
    }
    if (FillColor != 0)
    {
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(RowPos.x, RowPos.y + 1.0f * Scale), ImVec2(RowPos.x + SelectableWidth, RowPos.y + RowHeight - 1.0f * Scale),
                                                  FillColor, 5.0f * Scale);
    }
    if (RowClicked == true)
    {
        OpenSessionFromSidebar(CurrentSession.Rec.Id);
        if (ImGui::IsMouseDoubleClicked(0) == true)
        {
            ModalTarget_ = CurrentSession.Rec.Id;
            RenameIsProject_ = false;
            std::snprintf(NameBuffer_, sizeof NameBuffer_, "%s", CurrentSession.Rec.Name.c_str());
            OpenModal(Modal::Rename);
        }
    }
    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    if (Row.Depth > 0)
    {
        const float ElbowX = RowPos.x - 7 * Scale;
        const float MidY = RowPos.y + RowHeight * 0.5f;
        DrawList->AddLine(ImVec2(ElbowX, RowPos.y - ImGui::GetStyle().ItemSpacing.y), ImVec2(ElbowX, MidY), TextDisabledColorU32_, 1.0f);
        DrawList->AddLine(ImVec2(ElbowX, MidY), ImVec2(RowPos.x + 2 * Scale, MidY), TextDisabledColorU32_, 1.0f);
    }
    DrawStatusDot(DrawList, ImVec2(RowPos.x + 9 * Scale, RowPos.y + RowHeight * 0.5f), 4.5f * Scale, CurrentSession, IsOpen == false);
    DrawList->AddText(ImVec2(RowPos.x + 22 * Scale, RowPos.y + (RowHeight - ImGui::GetFontSize()) * 0.5f), TextColorU32_, CurrentSession.Rec.Name.c_str());

    float BadgeRight = RowPos.x + RowWidth - 12 * Scale;
    if (CurrentSession.Rec.WorktreeInfo.has_value() == true)
    {
        const char* const Tag = "wt";
        const ImVec2 TagSize = ImGui::CalcTextSize(Tag);
        DrawList->AddText(ImVec2(BadgeRight - TagSize.x - 6 * Scale, RowPos.y + (RowHeight - TagSize.y) * 0.5f), TextDisabledColorU32_, Tag);
        BadgeRight -= TagSize.x + 14 * Scale;
    }
    if (Row.HiddenChildren > 0)
    {
        const std::string Badge = "+" + std::to_string(Row.HiddenChildren);
        const ImVec2 BadgeSize = ImGui::CalcTextSize(Badge.c_str());
        DrawList->AddText(ImVec2(BadgeRight - BadgeSize.x - 6 * Scale, RowPos.y + (RowHeight - BadgeSize.y) * 0.5f),
                          Row.HiddenChildBusy == true ? BusyColor_ : TextDisabledColorU32_, Badge.c_str());
    }

    if (ImGui::BeginDragDropSource() == true)
    {
        ImGui::SetDragDropPayload("SESSION_REORDER", CurrentSession.Rec.Id.c_str(), CurrentSession.Rec.Id.size() + 1);
        ImGui::TextUnformatted(CurrentSession.Rec.Name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget() == true)
    {
        const ImGuiPayload* const Payload = ImGui::AcceptDragDropPayload("SESSION_REORDER");
        if (Payload != nullptr)
        {
            DragSourceId = static_cast<const char*>(Payload->Data);
            DragTargetId = CurrentSession.Rec.Id;
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered() == true && ImGui::IsMouseDragging(0) == false)
    {
        const Session* const Parent = CurrentSession.Rec.ParentId.empty() == true ? nullptr : FindSession(CurrentSession.Rec.ParentId);
        std::string Tip = CurrentSession.Rec.Cwd;
        if (CurrentSession.Rec.WorktreeInfo.has_value() == true)
        {
            Tip += "\nbranch: " + CurrentSession.Rec.WorktreeInfo->Branch;
        }
        if (Parent != nullptr)
        {
            Tip += "\nStarted by " + Parent->Rec.Name;
        }
        ImGui::SetTooltip("%s", Tip.c_str());
    }
    if (ImGui::BeginPopupContextItem("##sessmenu") == true)
    {
        if (ImGui::MenuItem("Open") == true)
        {
            OpenSessionFromSidebar(CurrentSession.Rec.Id);
        }
        if (ImGui::MenuItem("Rename") == true)
        {
            ModalTarget_ = CurrentSession.Rec.Id;
            RenameIsProject_ = false;
            std::snprintf(NameBuffer_, sizeof NameBuffer_, "%s", CurrentSession.Rec.Name.c_str());
            OpenModal(Modal::Rename);
        }
        const Session* const ReviewTarget = ReviewTargetFor(CurrentSession);
        if (ImGui::MenuItem(ReviewTarget != nullptr ? "Review 3D model" : "Review 3D model (debug)") == true)
        {
            OpenBlenderReview(ReviewTarget != nullptr ? ReviewTarget->Rec.Id : CurrentSession.Rec.Id);
        }
        if (IsOpen == true && ImGui::MenuItem("Hide pane") == true)
        {
            HideSession(CurrentSession.Rec.Id);
        }
        if (CurrentSession.Rec.ParentId.empty() == false && ImGui::MenuItem("Detach from parent") == true)
        {
            CurrentSession.Rec.ParentId.clear();
            MarkDirty();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Close session...") == true)
        {
            RequestCloseSession(CurrentSession.Rec.Id);
        }
        ImGui::EndPopup();
    }
    if (RowCloseButton(ClosePos, CloseHovered) == true)
    {
        RequestCloseSession(CurrentSession.Rec.Id);
    }
    if (Row.HasChildren == true)
    {
        const float ToggleSize = RowHeight * 0.6f;
        const ImVec2 TogglePos(RowPos.x - 13 * Scale, RowPos.y + (RowHeight - ToggleSize) * 0.5f);
        ImGui::SetCursorScreenPos(TogglePos);
        const bool ToggleClicked = ImGui::InvisibleButton("##children", ImVec2(ToggleSize, ToggleSize));
        const bool ToggleHovered = ImGui::IsItemHovered();
        const bool Folded = Row.HiddenChildren > 0;
        const ImU32 ToggleColor = ToggleHovered == true ? TextColorU32_ : TextDisabledColorU32_;
        const ImVec2 Center(TogglePos.x + ToggleSize * 0.5f, TogglePos.y + ToggleSize * 0.5f);
        const float Half = ToggleSize * 0.28f;
        if (Folded == true)
        {
            DrawList->AddTriangleFilled(ImVec2(Center.x - Half, Center.y - Half * 1.3f), ImVec2(Center.x - Half, Center.y + Half * 1.3f),
                                        ImVec2(Center.x + Half * 1.3f, Center.y), ToggleColor);
        }
        else
        {
            DrawList->AddTriangleFilled(ImVec2(Center.x - Half * 1.3f, Center.y - Half), ImVec2(Center.x + Half * 1.3f, Center.y - Half),
                                        ImVec2(Center.x, Center.y + Half * 1.3f), ToggleColor);
        }
        if (ToggleClicked == true)
        {
            State_.Ui.Collapsed["children:" + CurrentSession.Rec.Id] = Folded == false;
            MarkDirty();
        }
    }
    ImGui::Unindent(12 * Scale + DepthIndent);
    ImGui::PopID();
}
