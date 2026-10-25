#pragma once
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "RamSettings.h"
#include "Store.h"
#include "imgui.h"

// Per-project settings, opened from a project's right-click menu in the sidebar. For now it holds the RAM budget:
// how much memory the project's heavy jobs reserve so sessions running side by side don't run the PC out of memory.
class ProjectSettingsWindow
{
public:
    void Open(const std::string& ProjectId)
    {
        ProjectId_ = ProjectId;
        QueriedPath_.clear();
        Pending_.reset();
        Current_ = RamSettings::Snapshot{};
        Loaded_ = false;
        Dirty_ = false;
        Focus_ = true;
    }

    // Draws the window when it is open. Sets Message (and IsError) for the caller to show as a toast.
    void Draw(const std::vector<Project>& Projects, const float Scale, const ImVec4& Accent, std::string& Message, bool& IsError)
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
        const Project& Owner = *Found;
        if (QueriedPath_ != Owner.Path)
        {
            StartQuery(Owner.Path);
        }
        CollectQuery();

        ImGuiViewport* const Viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(560 * Scale, Viewport->WorkSize.x * 0.9f), std::min(520 * Scale, Viewport->WorkSize.y * 0.85f)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(380 * Scale, 240 * Scale), ImVec2(Viewport->WorkSize.x, Viewport->WorkSize.y));
        if (Focus_ == true)
        {
            ImGui::SetNextWindowFocus();
            Focus_ = false;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * Scale, 12 * Scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * Scale);
        bool IsOpen = true;
        const std::string Title = "Project settings - " + Owner.Name + "###projectSettings";
        if (ImGui::Begin(Title.c_str(), &IsOpen, ImGuiWindowFlags_NoCollapse) == true)
        {
            ImGui::TextDisabled("%s", Owner.Path.c_str());
            ImGui::Spacing();
            ImGui::SeparatorText("RAM");
            DrawRam(Owner, Scale, Accent, Message, IsError);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        if (IsOpen == false)
        {
            ProjectId_.clear();
        }
    }

private:
    struct PendingQuery
    {
        std::mutex Mutex;
        bool Done = false;
        RamSettings::Snapshot Result;
    };

    void StartQuery(const std::string& Path)
    {
        QueriedPath_ = Path;
        const std::shared_ptr<PendingQuery> Query = std::make_shared<PendingQuery>();
        Pending_ = Query;
        std::thread([Query, Path]
        {
            RamSettings::Snapshot Result = RamSettings::Query(Path);
            const std::lock_guard<std::mutex> Guard(Query->Mutex);
            Query->Result = std::move(Result);
            Query->Done = true;
        }).detach();
    }

    // Takes a finished query's result. The first one also sets the editable choice; later ones (after Save or Refresh) only update the numbers.
    void CollectQuery()
    {
        if (Pending_ == nullptr)
        {
            return;
        }
        {
            const std::lock_guard<std::mutex> Guard(Pending_->Mutex);
            if (Pending_->Done == false)
            {
                return;
            }
            Current_ = std::move(Pending_->Result);
        }
        Pending_.reset();
        if (Loaded_ == true || Current_.Error.empty() == false)
        {
            return;
        }
        Fixed_ = Current_.Fixed;
        FixedGb_ = static_cast<float>(Current_.FixedGb > 0 ? Current_.FixedGb : LargestEstimate());
        Loaded_ = true;
    }

    double LargestEstimate() const
    {
        double Largest = 4.0;
        for (const RamSettings::TaskEstimate& Task : Current_.Tasks)
        {
            Largest = std::max(Largest, Task.Gb);
        }
        return Largest;
    }

    void DrawRam(const Project& Owner, const float Scale, const ImVec4& Accent, std::string& Message, bool& IsError)
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Before a session in this project starts something heavy (Unreal editor, C++ build, game window, Blender), it reserves "
                            "RAM so other sessions don't start heavy work at the same moment and run the PC out of memory.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (Loaded_ == false)
        {
            if (Current_.Error.empty() == true)
            {
                ImGui::TextDisabled("Reading the current RAM estimates...");
                return;
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(0.94f, 0.36f, 0.36f, 1), "%s", Current_.Error.c_str());
            ImGui::PopTextWrapPos();
            if (Pending_ == nullptr && ImGui::Button("Try again") == true)
            {
                StartQuery(Owner.Path);
            }
            return;
        }

        if (ImGui::RadioButton("Dynamic", Fixed_ == false) == true && Fixed_ == true)
        {
            Fixed_ = false;
            Dirty_ = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("reserve what this project's recent runs actually used");
        if (Fixed_ == false)
        {
            ImGui::Indent(22 * Scale);
            DrawEstimates(Accent);
            ImGui::Unindent(22 * Scale);
        }
        ImGui::Spacing();

        if (ImGui::RadioButton("Fixed amount", Fixed_ == true) == true && Fixed_ == false)
        {
            Fixed_ = true;
            Dirty_ = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("reserve the same amount for every heavy job");
        if (Fixed_ == true)
        {
            ImGui::Indent(22 * Scale);
            const float MaxGb = Current_.TotalGb > 1 ? static_cast<float>(std::floor(Current_.TotalGb)) : 64.0f;
            ImGui::SetNextItemWidth(260 * Scale);
            if (ImGui::SliderFloat("##fixedGb", &FixedGb_, 0.5f, MaxGb, "%.1f GB", ImGuiSliderFlags_AlwaysClamp) == true)
            {
                FixedGb_ = std::max(0.5f, std::round(FixedGb_ * 2.0f) / 2.0f);
                Dirty_ = true;
            }
            ImGui::TextDisabled("Ctrl+click the slider to type a number. Jobs are still measured, so Dynamic stays up to date.");
            ImGui::Unindent(22 * Scale);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::BeginDisabled(Dirty_ == false);
        if (ImGui::Button("Save") == true)
        {
            std::string Error;
            if (RamSettings::Save(Owner.Path, Fixed_, FixedGb_, Error) == true)
            {
                Dirty_ = false;
                Message = "Saved RAM settings for " + Owner.Name + ".";
                StartQuery(Owner.Path);
            }
            else
            {
                Message = Error;
                IsError = true;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(Pending_ != nullptr);
        if (ImGui::Button("Refresh estimates") == true)
        {
            StartQuery(Owner.Path);
        }
        ImGui::EndDisabled();
        if (Current_.TotalGb > 0)
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Free right now: %.1f of %.1f GB", Current_.AvailableGb, Current_.TotalGb);
        }
        if (Dirty_ == true)
        {
            ImGui::TextDisabled("Unsaved change. Sessions keep using the saved setting until you press Save.");
        }
    }

    void DrawEstimates(const ImVec4& Accent)
    {
        if (Current_.Error.empty() == false)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(0.94f, 0.36f, 0.36f, 1), "%s", Current_.Error.c_str());
            ImGui::PopTextWrapPos();
            return;
        }
        if (Current_.Tasks.empty() == true)
        {
            ImGui::TextDisabled("No estimates available.");
            return;
        }
        ImGui::TextDisabled(Pending_ != nullptr ? "Current estimates (refreshing...):" : "Current estimates:");
        const ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##ramEstimates", 3, Flags) == false)
        {
            return;
        }
        ImGui::TableSetupColumn("Job");
        ImGui::TableSetupColumn("Reserves", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Based on", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const RamSettings::TaskEstimate& Task : Current_.Tasks)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(RamSettings::TaskLabel(Task.Name));
            ImGui::TableNextColumn();
            ImGui::Text("%.1f GB", Task.Gb);
            ImGui::TableNextColumn();
            if (Task.Source.rfind("learned", 0) == 0)
            {
                ImGui::TextColored(Accent, "%s", Task.Source.c_str());
                continue;
            }
            ImGui::TextDisabled("%s", Task.Source.c_str());
        }
        ImGui::EndTable();
        ImGui::TextDisabled("Learned values are the highest of the last 5 runs plus a safety margin, updated after each run.");
    }

    std::string ProjectId_;  // empty while the window is closed
    std::string QueriedPath_;
    std::shared_ptr<PendingQuery> Pending_;
    RamSettings::Snapshot Current_;
    bool Loaded_ = false;
    bool Fixed_ = false;
    float FixedGb_ = 8.0f;
    bool Dirty_ = false;
    bool Focus_ = false;
};
