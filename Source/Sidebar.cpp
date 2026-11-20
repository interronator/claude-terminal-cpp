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


float App::RowCloseButtonWidth() const
{
    return ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// The "x" on a sidebar row: no box of its own (the row's highlight runs underneath it), just a dim
// glyph that lights up on hover, like the close "x" on the top bar chips.
bool App::RowCloseButton(const ImVec2& Pos, const bool Hovered)
{
    ImGui::SetCursorScreenPos(Pos);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, Hovered == true ? WaitingColor_ : TextDisabledColorU32_);
    const bool Clicked = ImGui::Button("x", ImVec2(RowCloseButtonWidth(), ImGui::GetFrameHeight()));
    ImGui::PopStyleColor(4);
    return Clicked;
}

void App::DrawSidebar(float Width)
{
    const float Scale = UiScale_;
    PeerVeins_.BeginSidebarFrame();

    // A row icon button (the "+"/"x" on project and session rows): transparent at rest so it reads
    // as part of the row, and on hover/press it fills with the exact same flat rectangle the row
    // itself uses for its own hover/selected highlight, instead of looking like a separate pill button.
    const std::function<bool(const char*)> IconButton = [&](const char* Label)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * Scale);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        bool const Clicked = ImGui::Button(Label);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        return Clicked;
    };

    ImGui::PushStyleColor(ImGuiCol_ChildBg, SurfaceColorV4_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * Scale, 10 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * Scale, 6 * Scale));
    ImGui::BeginChild("##side", ImVec2(Width, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::PushStyleColor(ImGuiCol_Text, AccentColorV4_);
    ImGui::TextUnformatted("*");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted("Claude Terminal");
    ImGui::SameLine();
    const float CollapseButtonWidth = ImGui::CalcTextSize("<").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - CollapseButtonWidth);
    if (IconButton("<") == true)
    {
        State_.Ui.SidebarCollapsed = true;
        MarkDirty();
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Collapse sidebar");
    }
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, AccentColorV4_);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentHoverColorV4_);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentActiveColorV4_);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.06f, 0.04f, 1));
    if (ImGui::Button("+ Add project folder", ImVec2(-1, 0)))
    {
        PathBuffer_[0] = 0;
        ModalError_.clear();
        OpenModal(Modal::AddProject);
    }
    ImGui::PopStyleColor(4);
    if (ImGui::Button("+ General session", ImVec2(-1, 0)))
    {
        StartGeneralSession();
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Start Claude without picking a project. It works in %s.", GeneralFolderPath().c_str());
    }
    ImGui::Spacing();

    const float FooterHeight = ImGui::GetTextLineHeightWithSpacing() * 3.2f + ImGui::GetFrameHeightWithSpacing();
    const float ArchiveHeight = State_.Ui.ArchiveOpen == true ? 170.0f * Scale : ImGui::GetFrameHeightWithSpacing() + 6.0f * Scale;
    ImGui::BeginChild("##tree", ImVec2(0, -(FooterHeight + ArchiveHeight + ImGui::GetStyle().ItemSpacing.y)), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    LayoutDebug_.Sample("tree.width", ImGui::GetWindowWidth());
    LayoutDebug_.Sample("tree.availX", ImGui::GetContentRegionAvail().x);
    LayoutDebug_.Sample("tree.height", ImGui::GetWindowHeight());
    LayoutDebug_.Sample("tree.scrollY", ImGui::GetScrollY());
    LayoutDebug_.Sample("tree.scrollMaxY", ImGui::GetScrollMaxY());
    LayoutDebug_.Sample("sidebar.width", Width);
    if (State_.Projects.empty())
    {
        ImGui::TextDisabled("No projects yet.");
    }

    size_t DragSourceIndex = SIZE_MAX;
    size_t DragTargetIndex = SIZE_MAX;
    std::string SessionDragSourceId;
    std::string SessionDragTargetId;
    for (size_t ProjectIndex = 0; ProjectIndex < State_.Projects.size(); ProjectIndex++)
    {
        Project const& CurrentProject = State_.Projects[ProjectIndex];
        ImGui::PushID(CurrentProject.Id.c_str());
        bool const Collapsed = State_.Ui.Collapsed[CurrentProject.Id];

        // attention summary for the header dot
        const Session* Worst = nullptr;
        for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
        {
            if (CurrentSession->Rec.ProjectId != CurrentProject.Id)
            {
                continue;
            }
            if (CurrentSession->CurrentStatus == Status::Waiting)
            {
                Worst = CurrentSession.get();
                break;
            }
            if (CurrentSession->Unseen && CurrentSession->InBackground() == false)
            {
                Worst = CurrentSession.get();
            }
            else if (Worst == nullptr && (CurrentSession->CurrentStatus == Status::Busy || CurrentSession->InBackground() == true))
            {
                Worst = CurrentSession.get();
            }
        }

        ImGui::SetNextItemOpen(Collapsed == false, ImGuiCond_Always);
        bool const Open = ImGui::TreeNodeEx(CurrentProject.Name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding |
                                                                 ImGuiTreeNodeFlags_AllowOverlap);
        const float ArrowWidth = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) == true && ImGui::GetMousePos().x - ImGui::GetItemRectMin().x > ArrowWidth)
        {
            ProjectScopeId_ = CurrentProject.Id;
        }
        if (ImGui::IsItemToggledOpen())
        {
            State_.Ui.Collapsed[CurrentProject.Id] = Open == false;
            MarkDirty();
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID) == true)
        {
            ImGui::SetDragDropPayload("PROJECT_REORDER", &ProjectIndex, sizeof(size_t));
            ImGui::TextUnformatted(CurrentProject.Name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget() == true)
        {
            const ImGuiPayload* const Payload = ImGui::AcceptDragDropPayload("PROJECT_REORDER");
            if (Payload != nullptr)
            {
                DragSourceIndex = *static_cast<const size_t*>(Payload->Data);
                DragTargetIndex = ProjectIndex;
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(0) == false)
        {
            ImGui::SetTooltip("%s", CurrentProject.Path.c_str());
        }
        if (ImGui::BeginPopupContextItem("##projmenu"))
        {
            DrawNewSessionChoices(CurrentProject);
            ImGui::Separator();
            if (ImGui::MenuItem("Rename project"))
            {
                ModalTarget_ = CurrentProject.Id;
                RenameIsProject_ = true;
                std::snprintf(NameBuffer_, sizeof NameBuffer_, "%s", CurrentProject.Name.c_str());
                OpenModal(Modal::Rename);
            }
            if (ImGui::MenuItem("Reveal folder"))
            {
                Platform::Reveal(CurrentProject.Path);
            }
            if (ImGui::MenuItem("Auto mode for this project", nullptr, ProjectIsAuto(CurrentProject.Id)) == true)
            {
                SetProjectAutoMode(CurrentProject.Id, ProjectIsAuto(CurrentProject.Id) == false);
            }
            if (ImGui::MenuItem("Project log") == true)
            {
                ProjectLogWindow_.Open(CurrentProject.Id);
            }
            if (ImGui::MenuItem("Project settings...") == true)
            {
                ProjectSettingsWindow_.Open(CurrentProject.Id);
            }
            if (NightlyBuild_.SupportsCached(CurrentProject.Path, Now_) == true && ImGui::MenuItem("Nightly Build") == true)
            {
                OpenNightlyBuild(CurrentProject.Id);
            }
            if (ImGui::MenuItem("Close unopened sessions..."))
            {
                RequestCloseUnopenedSessions(CurrentProject.Id);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove project"))
            {
                std::string Id = CurrentProject.Id, Name = CurrentProject.Name;
                ConfirmTitle_ = "Remove \"" + Name + "\"?";
                ConfirmText_ = "This stops its sessions and removes the project from the list.\nFiles and git worktrees on disk are left untouched.";
                ConfirmYesAction_ = [this, Id] { RemoveProject(Id); };
                OpenModal(Modal::Confirm);
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 28 * Scale);
        if (IconButton("+"))
        {
            BeginNewSession(CurrentProject.Id);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("New session (right-click for more)");
        }
        if (ImGui::BeginPopupContextItem("##plusmenu") == true)
        {
            DrawNewSessionChoices(CurrentProject);
            ImGui::EndPopup();
        }
        if (Worst != nullptr)
        {
            ImVec2 const DotCenter(ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x - 42 * Scale,
                             ImGui::GetItemRectMin().y + ImGui::GetItemRectSize().y * 0.5f);
            DrawStatusDot(ImGui::GetWindowDrawList(), DotCenter, 4.5f * Scale, *Worst);
        }

        if (Open)
        {
            ImGui::Separator();
            ImDrawList* const GroupDrawList = ImGui::GetWindowDrawList();
            ImDrawListSplitter GroupSplitter;
            GroupSplitter.Split(GroupDrawList, 2);
            GroupSplitter.SetCurrentChannel(GroupDrawList, 1);
            ImVec2 const GroupStart = ImGui::GetCursorScreenPos();
            float const GroupWidth = ImGui::GetContentRegionAvail().x;
            bool Any = false;
            const std::vector<SidebarRow> Rows = BuildSidebarRows(CurrentProject.Id);
            for (const SidebarRow& Row : Rows)
            {
                Any = true;
                DrawSidebarSessionRow(Row, SessionDragSourceId, SessionDragTargetId);
            }
            if (Any == false)
            {
                ImGui::Indent(12 * Scale);
                ImGui::TextDisabled("No sessions yet");
                ImGui::Unindent(12 * Scale);
            }
            GroupSplitter.SetCurrentChannel(GroupDrawList, 0);
            GroupDrawList->AddRectFilled(GroupStart, ImVec2(GroupStart.x + GroupWidth, ImGui::GetCursorScreenPos().y),
                                         UiHelpers::WithAlpha(TextColorU32_, 14), 4 * Scale);
            GroupSplitter.Merge(GroupDrawList);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (DragSourceIndex != DragTargetIndex && DragSourceIndex < State_.Projects.size() && DragTargetIndex < State_.Projects.size())
    {
        Project Moved = std::move(State_.Projects[DragSourceIndex]);
        State_.Projects.erase(State_.Projects.begin() + DragSourceIndex);
        State_.Projects.insert(State_.Projects.begin() + DragTargetIndex, std::move(Moved));
        MarkDirty();
    }
    if (SessionDragSourceId.empty() == false && SessionDragSourceId != SessionDragTargetId)
    {
        const auto FindSession = [this](const std::string& Id)
        {
            return std::find_if(Sessions_.begin(), Sessions_.end(), [&Id](const std::unique_ptr<Session>& Item) { return Item->Rec.Id == Id; });
        };
        const auto SourceIt = FindSession(SessionDragSourceId);
        const auto TargetIt = FindSession(SessionDragTargetId);
        if (SourceIt != Sessions_.end() && TargetIt != Sessions_.end() && (*SourceIt)->Rec.ProjectId == (*TargetIt)->Rec.ProjectId)
        {
            const size_t TargetIndex = static_cast<size_t>(TargetIt - Sessions_.begin());
            std::unique_ptr<Session> Moved = std::move(*SourceIt);
            Sessions_.erase(SourceIt);
            Sessions_.insert(Sessions_.begin() + TargetIndex, std::move(Moved));
            MarkDirty();
        }
    }
    ImGui::EndChild();

    ImGui::BeginChild("##archive", ImVec2(0, ArchiveHeight), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    LayoutDebug_.Sample("archive.availX", ImGui::GetContentRegionAvail().x);
    LayoutDebug_.Sample("archive.height", ImGui::GetWindowHeight());
    ImGui::SetNextItemOpen(State_.Ui.ArchiveOpen, ImGuiCond_Always);
    const std::string ArchiveLabel = "Archive (" + std::to_string(State_.Archive.size()) + ")";
    const bool ArchiveExpanded = ImGui::TreeNodeEx(ArchiveLabel.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
    if (ImGui::IsItemToggledOpen() == true)
    {
        State_.Ui.ArchiveOpen = ArchiveExpanded;
        MarkDirty();
    }
    if (ImGui::BeginPopupContextItem("##archiveHeaderMenu") == true)
    {
        if (ImGui::MenuItem("Delete all archived", nullptr, false, State_.Archive.empty() == false) == true)
        {
            RequestDeleteAllArchived();
        }
        ImGui::EndPopup();
    }
    size_t ArchiveDeleteIndex = SIZE_MAX;
    size_t ArchiveRestoreIndex = SIZE_MAX;
    if (ArchiveExpanded == true)
    {
        if (State_.Archive.empty() == true)
        {
            ImGui::Indent(12 * Scale);
            ImGui::TextDisabled("Nothing archived");
            ImGui::Unindent(12 * Scale);
        }
        else if (ImGui::SmallButton("Delete all archived") == true)
        {
            RequestDeleteAllArchived();
        }
        for (size_t ArchiveIndex = 0; ArchiveIndex < State_.Archive.size(); ArchiveIndex++)
        {
            const ArchivedSession& Archived = State_.Archive[ArchiveIndex];
            ImGui::PushID(static_cast<int>(ArchiveIndex));
            ImGui::Indent(12 * Scale);
            const ImVec2 RowPos = ImGui::GetCursorScreenPos();
            const float CloseWidth = RowCloseButtonWidth();
            const float RowWidth = ImGui::GetContentRegionMax().x - 24 * Scale - ImGui::GetCursorPosX() + CloseWidth;
            const ImVec2 ClosePos(RowPos.x + RowWidth - CloseWidth, RowPos.y);
            const bool CloseHovered = ImGui::IsWindowHovered() == true &&
                                      ImGui::IsMouseHoveringRect(ClosePos, ImVec2(ClosePos.x + CloseWidth, ClosePos.y + ImGui::GetFrameHeight())) == true;
            const ImU32 ArchiveHoverFill = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
            const ImU32 ArchivePressFill = ImGui::GetColorU32(ImGuiCol_HeaderActive);
            const ImVec4 ArchiveClear(0, 0, 0, 0);
            ImGui::PushStyleColor(ImGuiCol_Header, ArchiveClear);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ArchiveClear);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ArchiveClear);
            const bool RowClicked = ImGui::Selectable("##archiverow", false, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
                                                      ImVec2(RowWidth, ImGui::GetFrameHeight()));
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemActive() == true || ImGui::IsItemHovered() == true || CloseHovered == true)
            {
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(RowPos.x, RowPos.y + 1.0f * Scale),
                                                          ImVec2(RowPos.x + RowWidth, RowPos.y + ImGui::GetFrameHeight() - 1.0f * Scale),
                                                          ImGui::IsItemActive() == true ? ArchivePressFill : ArchiveHoverFill, 5.0f * Scale);
            }
            if (RowClicked == true &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) == true && Archived.Rec.ClaudeSessionId.empty() == false)
            {
                ArchiveRestoreIndex = ArchiveIndex;
            }
            if (ImGui::BeginPopupContextItem("##archivemenu") == true)
            {
                if (ImGui::MenuItem("Restore", nullptr, false, Archived.Rec.ClaudeSessionId.empty() == false) == true)
                {
                    ArchiveRestoreIndex = ArchiveIndex;
                }
                if (ImGui::MenuItem("Delete permanently") == true)
                {
                    ArchiveDeleteIndex = ArchiveIndex;
                }
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos(RowPos);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Archived.Name.c_str());
            if (Archived.ProjectName.empty() == false)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", Archived.ProjectName.c_str());
            }
            if (RowCloseButton(ClosePos, CloseHovered) == true)
            {
                ArchiveDeleteIndex = ArchiveIndex;
            }
            if (ImGui::IsItemHovered() == true)
            {
                ImGui::SetTooltip("Delete permanently");
            }
            ImGui::Unindent(12 * Scale);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (ArchiveRestoreIndex < State_.Archive.size())
    {
        RestoreArchivedSession(ArchiveRestoreIndex);
        MarkDirty();
    }
    else if (ArchiveDeleteIndex < State_.Archive.size())
    {
        State_.Archive.erase(State_.Archive.begin() + ArchiveDeleteIndex);
        MarkDirty();
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Checkbox("Close without confirmation", &State_.Ui.CloseWithoutConfirm) == true)
    {
        MarkDirty();
    }
    ImGui::TextDisabled("Alt+1-9  focus pane");
    ImGui::TextDisabled("Ctrl+Shift+N  new session");
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}
