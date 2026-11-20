#include "App.h"
#include "SidePanelTabs.h"

std::set<std::string> App::SelectedSessionFamily() const
{
    std::set<std::string> Family;
    Family.insert(State_.Ui.ActiveId);
    bool Grew = true;
    while (Grew == true)
    {
        Grew = false;
        for (const std::unique_ptr<Session>& Candidate : Sessions_)
        {
            if (Candidate->Rec.ParentId.empty() == false && Family.count(Candidate->Rec.ParentId) > 0 && Family.insert(Candidate->Rec.Id).second == true)
            {
                Grew = true;
            }
        }
    }
    return Family;
}

std::vector<const Session*> App::ShellPanelSessions() const
{
    std::vector<const Session*> Scoped;
    const bool ProjectSelected = ProjectScopeId_.empty() == false;
    const std::set<std::string> Family = SelectedSessionFamily();
    for (const std::unique_ptr<Session>& Candidate : Sessions_)
    {
        const bool InScope = ProjectSelected == true ? Candidate->Rec.ProjectId == ProjectScopeId_ : Family.count(Candidate->Rec.Id) > 0;
        if (InScope == false)
        {
            continue;
        }
        Scoped.push_back(Candidate.get());
    }
    return Scoped;
}

int App::RunningShellCount() const
{
    int Total = 0;
    for (const Session* const Scoped : ShellPanelSessions())
    {
        const std::map<std::string, std::vector<TranscriptScanner::ActiveCommand>>::const_iterator Found = RunningCommands_.find(Scoped->Rec.Id);
        if (Found == RunningCommands_.end())
        {
            continue;
        }
        Total += static_cast<int>(Found->second.size());
    }
    return Total;
}

void App::DrawRightPanel(const float Width)
{
    const float Scale = UiScale_;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, SurfaceColorV4_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * Scale, 8 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * Scale, 6 * Scale));
    ImGui::BeginChild("##shots", ImVec2(Width, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const bool ShellsShown = State_.Ui.RightPanelTab == "shells";
    const std::vector<SidePanelTabs::Tab> Tabs = {{"shots", "Screenshots", 0}, {"shells", "Shells", RunningShellCount()}};
    const std::string Clicked = SidePanelTabs::Draw(Tabs, ShellsShown == true ? "shells" : "shots", AccentColor_, Scale);
    if (Clicked.empty() == false && Clicked != State_.Ui.RightPanelTab)
    {
        State_.Ui.RightPanelTab = Clicked;
        MarkDirty();
    }

    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetFrameHeight() - 8 * Scale);
    if (ImGui::Button(">", ImVec2(ImGui::GetFrameHeight(), 0)) == true)
    {
        State_.Ui.ScreenshotPanelOpen = false;
        MarkDirty();
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Hide side panel");
    }

    if (State_.Ui.RightPanelTab == "shells")
    {
        DrawShellsTab();
    }
    else
    {
        DrawScreenshotsTab();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}
