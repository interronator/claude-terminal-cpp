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


void App::OpenSearch()
{
    if (Modal_ != Modal::None)
    {
        return;
    }
    SearchBuffer_[0] = 0;
    SearchLastQuery_ = "\x01";
    SearchSelected_ = 0;
    SearchFocus_ = true;
    SearchResults_.clear();

    const Session* const Active = ActiveSession();
    std::string ProjectId = Active != nullptr ? Active->Rec.ProjectId : State_.Ui.TabFilterProjectId;
    if (ProjectId.empty() == true && State_.Projects.empty() == false)
    {
        ProjectId = State_.Projects.front().Id;
    }
    SearchRootProjectId_ = ProjectId;
    const Project* const Root = FindProject(ProjectId);
    if (Active != nullptr && Active->Rec.Cwd.empty() == false)
    {
        FileIndex_.Start(Active->Rec.Cwd);
    }
    else if (Root != nullptr)
    {
        FileIndex_.Start(Root->Path);
    }
    OpenModal(Modal::Search);
}

void App::RefreshSearchResults()
{
    SearchResults_.clear();
    const std::string Query = FileIndex::Lower(Util::Trim(SearchBuffer_));

    for (const Project& Candidate : State_.Projects)
    {
        if (Query.empty() == false && FileIndex::Lower(Candidate.Name).find(Query) == std::string::npos &&
            FileIndex::Lower(Candidate.Path).find(Query) == std::string::npos)
        {
            continue;
        }
        SearchResults_.push_back({SearchResult::Kind::Project, Candidate.Name, Candidate.Path, Candidate.Id});
    }

    size_t SessionCount = 0;
    for (const std::unique_ptr<Session>& Candidate : Sessions_)
    {
        const Project* const Owner = FindProject(Candidate->Rec.ProjectId);
        const std::string OwnerName = Owner != nullptr ? Owner->Name : std::string();
        if (Query.empty() == false && FileIndex::Lower(Candidate->Rec.Name).find(Query) == std::string::npos &&
            FileIndex::Lower(OwnerName).find(Query) == std::string::npos)
        {
            continue;
        }
        SearchResults_.push_back({SearchResult::Kind::Session, Candidate->Rec.Name, OwnerName, Candidate->Rec.Id});
        SessionCount++;
        if (SessionCount >= 20)
        {
            break;
        }
    }

    if (Query.empty() == true)
    {
        return;
    }
    std::vector<std::string> FilePaths;
    FileIndex_.Search(Query, 50, FilePaths);
    for (const std::string& FilePath : FilePaths)
    {
        const size_t NameStart = FilePath.find_last_of('/') == std::string::npos ? 0 : FilePath.find_last_of('/') + 1;
        const std::string Directory = NameStart == 0 ? std::string() : FilePath.substr(0, NameStart - 1);
        SearchResults_.push_back({SearchResult::Kind::File, FilePath.substr(NameStart), Directory, FilePath});
    }
}

void App::ActivateSearchResult(const SearchResult& Result)
{
    ImGui::CloseCurrentPopup();
    Modal_ = Modal::None;
    switch (Result.Type)
    {
        case SearchResult::Kind::Project:
        {
            State_.Ui.Collapsed[Result.Target] = false;
            std::string TargetSessionId;
            for (const std::unique_ptr<Session>& Candidate : Sessions_)
            {
                if (Candidate->Rec.ProjectId != Result.Target)
                {
                    continue;
                }
                if (TargetSessionId.empty() == true)
                {
                    TargetSessionId = Candidate->Rec.Id;
                }
                if (std::find(State_.Ui.OpenIds.begin(), State_.Ui.OpenIds.end(), Candidate->Rec.Id) != State_.Ui.OpenIds.end())
                {
                    TargetSessionId = Candidate->Rec.Id;
                    break;
                }
            }
            MarkDirty();
            if (TargetSessionId.empty() == false)
            {
                OpenSession(TargetSessionId);
                return;
            }
            ModalTarget_ = Result.Target;
            OpenModal(Modal::NewSession);
            return;
        }
        case SearchResult::Kind::Session:
        {
            OpenSession(Result.Target);
            return;
        }
        case SearchResult::Kind::File:
        {
            Session* const Active = ActiveSession();
            if (Active == nullptr || Active->IsRunning() == false)
            {
                ShowToast("Open a running session first to add a file to its context.", true);
                return;
            }
            const bool NeedsQuotes = Result.Target.find(' ') != std::string::npos;
            const std::string Reference = "@" + (NeedsQuotes ? "\"" + Result.Target + "\"" : Result.Target) + " ";
            Active->ScrollOffset = 0;
            Active->SendText(Reference, Now_, StatusCallback());
            return;
        }
    }
}

void App::DrawSearch(bool Confirm)
{
    const float Scale = UiScale_;
    ImGui::TextDisabled("Search projects, sessions and files");
    ImGui::SetNextItemWidth(-1);
    if (SearchFocus_ == true || ImGui::IsWindowAppearing() == true)
    {
        ImGui::SetKeyboardFocusHere();
        SearchFocus_ = false;
    }
    ImGui::InputTextWithHint("##search", "Type a name or part of a path...", SearchBuffer_, sizeof SearchBuffer_);

    const bool QueryChanged = SearchLastQuery_ != SearchBuffer_;
    const size_t IndexSize = FileIndex_.Size();
    if (QueryChanged == true || IndexSize != SearchLastIndexSize_)
    {
        SearchLastQuery_ = SearchBuffer_;
        SearchLastIndexSize_ = IndexSize;
        RefreshSearchResults();
        if (QueryChanged == true)
        {
            SearchSelected_ = 0;
        }
    }

    const int ResultCount = static_cast<int>(SearchResults_.size());
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) == true && SearchSelected_ + 1 < ResultCount)
    {
        SearchSelected_++;
        SearchScrollToSelected_ = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) == true && SearchSelected_ > 0)
    {
        SearchSelected_--;
        SearchScrollToSelected_ = true;
    }
    SearchSelected_ = std::clamp(SearchSelected_, 0, std::max(0, ResultCount - 1));

    int ClickedIndex = -1;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, PaneBgColorV4_);
    ImGui::PushStyleColor(ImGuiCol_Border, PaneBorderColorV4_);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f * Scale);
    ImGui::BeginChild("##searchResults", ImVec2(-1, 300.0f * Scale), ImGuiChildFlags_Borders);
    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    const float RowHeight = ImGui::GetFrameHeight();
    const float TagWidth = 64.0f * Scale;
    for (int Index = 0; Index < ResultCount; Index++)
    {
        const SearchResult& Entry = SearchResults_[static_cast<size_t>(Index)];
        ImGui::PushID(Index);
        const ImVec2 RowPos = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", Index == SearchSelected_, ImGuiSelectableFlags_None, ImVec2(0, RowHeight)) == true)
        {
            ClickedIndex = Index;
        }
        if (SearchScrollToSelected_ == true && Index == SearchSelected_)
        {
            ImGui::SetScrollHereY();
            SearchScrollToSelected_ = false;
        }
        const char* const Tag = Entry.Type == SearchResult::Kind::Project ? "Project" : (Entry.Type == SearchResult::Kind::Session ? "Session" : "File");
        const float TextY = RowPos.y + (RowHeight - ImGui::GetFontSize()) * 0.5f;
        DrawList->AddText(ImVec2(RowPos.x + 8.0f * Scale, TextY), TextDisabledColorU32_, Tag);
        DrawList->AddText(ImVec2(RowPos.x + TagWidth + 8.0f * Scale, TextY), TextColorU32_, Entry.Label.c_str());
        const float LabelWidth = ImGui::CalcTextSize(Entry.Label.c_str()).x;
        DrawList->AddText(ImVec2(RowPos.x + TagWidth + 18.0f * Scale + LabelWidth, TextY), TextDisabledColorU32_, Entry.Detail.c_str());
        ImGui::PopID();
    }
    if (ResultCount == 0)
    {
        ImGui::TextDisabled(SearchBuffer_[0] == 0 ? "Nothing to show yet." : "No matches.");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    std::string Footer = "Up/Down to move, Enter to open, Esc to close";
    if (FileIndex_.Scanning() == true)
    {
        Footer += "   -   indexing files (" + std::to_string(IndexSize) + ")...";
    }
    ImGui::TextDisabled("%s", Footer.c_str());
    ImGui::TextDisabled("Files are added to the active session as an @ reference.");

    if (ClickedIndex < 0 && Confirm == true && ResultCount > 0)
    {
        ClickedIndex = SearchSelected_;
    }
    if (ClickedIndex >= 0)
    {
        const SearchResult Picked = SearchResults_[static_cast<size_t>(ClickedIndex)];
        ActivateSearchResult(Picked);
    }
}
