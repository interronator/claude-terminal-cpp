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


void App::DrawNightlyBuild()
{
    // Taken even while the window is closed, so the result of an action started just before closing still shows.
    std::string Message;
    bool IsError = false;
    if (NightlyBuild_.TakeMessage(Message, IsError) == true)
    {
        ShowToast(Message, IsError);
    }
    if (NightlyBuildId_.empty() == true)
    {
        return;
    }
    const Project* const Owner = FindProject(NightlyBuildId_);
    if (Owner == nullptr)
    {
        NightlyBuildId_.clear();
        return;
    }
    constexpr double REFRESH_SECONDS = 5.0;
    if (Now_ >= NightlyBuildCheckAt_)
    {
        NightlyBuildCheckAt_ = Now_ + REFRESH_SECONDS;
        NightlyBuild_.Refresh(Owner->Path);
    }

    const float Scale = UiScale_;
    ImGuiViewport* const Viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(820 * Scale, Viewport->WorkSize.x * 0.9f), std::min(600 * Scale, Viewport->WorkSize.y * 0.85f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(420 * Scale, 300 * Scale), ImVec2(Viewport->WorkSize.x, Viewport->WorkSize.y));
    if (NightlyBuildFocus_ == true)
    {
        ImGui::SetNextWindowFocus();
        NightlyBuildFocus_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * Scale, 12 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * Scale);
    bool Open = true;
    const std::string Title = "Nightly Build - " + Owner->Name + "###nightlyBuild";
    if (ImGui::Begin(Title.c_str(), &Open, ImGuiWindowFlags_NoCollapse) == true)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * Scale, 6 * Scale));
        ImGui::TextDisabled("Builds run through Windows Task Scheduler while you are signed in. Files live in %s",
                            NightlyBuild::JobsRoot(Owner->Path).string().c_str());
        if (NightlyBuild_.Busy() == true)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(BusyColor_), "Working...");
        }
        if (ImGui::BeginTabBar("##nightlyTabs") == true)
        {
            if (ImGui::BeginTabItem("Scheduled builds") == true)
            {
                DrawNightlyBuildJobs(*Owner);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Set up a new build") == true)
            {
                DrawNightlyBuildSetup(*Owner);
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
        NightlyBuildId_.clear();
    }
}

void App::DrawNightlyBuildJobs(const Project& Owner)
{
    const std::vector<NightlyBuild::Job> Jobs = NightlyBuild_.Jobs(Owner.Path);
    if (Jobs.empty() == true)
    {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", NightlyBuild_.Loaded(Owner.Path) == true ? "No nightly builds are set up for this project yet. Use the Set up a new build tab."
                                                                           : "Loading...");
        return;
    }
    const ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##nightlyJobs", 5, Flags) == false)
    {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Build", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("When", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("Last run", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    const bool Busy = NightlyBuild_.Busy();
    for (const NightlyBuild::Job& Item : Jobs)
    {
        ImGui::PushID(Item.Name.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(Item.Name.c_str());
        const std::string What = NightlyBuild::KindLabel(Item.What) + (Item.What == NightlyBuild::Kind::BuildScript ? "" : " (" + Item.Config + ")");
        ImGui::TextDisabled("%s", What.c_str());

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(NightlyBuild::ScheduleText(Item).c_str());
        if (Item.IsPaused() == false && Item.NextRun.empty() == false)
        {
            ImGui::TextDisabled("Next: %s", Item.NextRun.c_str());
        }

        ImGui::TableNextColumn();
        if (Item.TaskState.empty() == true)
        {
            ImGui::TextColored(WaitingColorV4_, "Not scheduled");
        }
        else if (Item.IsRunning() == true)
        {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(BusyColor_), "Building");
        }
        else if (Item.IsPaused() == true)
        {
            ImGui::TextDisabled("Paused");
        }
        else
        {
            ImGui::TextUnformatted("Scheduled");
        }

        ImGui::TableNextColumn();
        if (Item.LastResult.empty() == true)
        {
            ImGui::TextDisabled("Never run");
        }
        else
        {
            const bool Good = Item.LastResult == "Succeeded" || Item.LastResult == "Running";
            ImGui::TextColored(Good == true ? ImGui::ColorConvertU32ToFloat4(DoneColor_) : WaitingColorV4_, "%s", Item.LastResult.c_str());
            ImGui::TextDisabled("%s", Item.LastTime.c_str());
        }

        ImGui::TableNextColumn();
        ImGui::BeginDisabled(Busy == true);
        if (Item.IsRunning() == true)
        {
            if (ImGui::SmallButton("Cancel build") == true)
            {
                NightlyBuild_.Cancel(Owner.Path, Item.Name);
            }
        }
        else
        {
            ImGui::BeginDisabled(Item.TaskState.empty() == true);
            ImGui::BeginDisabled(Item.IsPaused() == true);
            if (ImGui::SmallButton("Run now") == true)
            {
                NightlyBuild_.RunNow(Owner.Path, Item.Name);
            }
            ImGui::EndDisabled();
            if (Item.IsPaused() == true && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) == true)
            {
                ImGui::SetTooltip("Resume the build first to run it.");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(Item.IsPaused() == true ? "Resume" : "Pause") == true)
            {
                NightlyBuild_.SetPaused(Owner.Path, Item.Name, Item.IsPaused() == false);
            }
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete") == true)
        {
            const std::string ProjectDir = Owner.Path, Name = Item.Name;
            ConfirmTitle_ = "Delete nightly build \"" + Name + "\"?";
            ConfirmText_ = "This removes the scheduled task and its settings" + std::string(Item.IsRunning() == true ? ", and cancels the build that is running now" : "") +
                           ".\nBuilds it already made in its Output folder are kept.";
            ConfirmYesAction_ = [this, ProjectDir, Name] { NightlyBuild_.Delete(ProjectDir, Name); };
            OpenModal(Modal::Confirm);
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(Item.HasLog == false);
        if (ImGui::SmallButton("Open log") == true)
        {
            Platform::Reveal(NightlyBuild::LogPath(Owner.Path, Item.Name).string());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(Item.HasOutput == false);
        if (ImGui::SmallButton("Open output") == true)
        {
            Platform::Reveal(Item.OutputDir);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void App::DrawNightlyBuildSetup(const Project& Owner)
{
    const float Scale = UiScale_;
    const bool IsUnreal = NightlyIsUnreal_;
    const bool HasScript = NightlyHasScript_;
    const float LabelWidth = 130 * Scale;
    const float FieldWidth = std::max(200 * Scale, ImGui::GetContentRegionAvail().x - LabelWidth);
    ImGui::Spacing();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine(LabelWidth);
    ImGui::SetNextItemWidth(FieldWidth);
    ImGui::InputText("##nightlyName", NightlyNameBuffer_, sizeof NightlyNameBuffer_);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("What to build");
    ImGui::SameLine(LabelWidth);
    ImGui::SetNextItemWidth(FieldWidth);
    if (ImGui::BeginCombo("##nightlyKind", NightlyBuild::KindLabel(NightlyDraft_.What).c_str()) == true)
    {
        for (const NightlyBuild::Kind Candidate : {NightlyBuild::Kind::PackageGame, NightlyBuild::Kind::CompileEditor, NightlyBuild::Kind::BuildScript})
        {
            const bool Available = Candidate == NightlyBuild::Kind::BuildScript ? HasScript : IsUnreal;
            if (Available == false)
            {
                continue;
            }
            if (ImGui::Selectable(NightlyBuild::KindLabel(Candidate).c_str(), Candidate == NightlyDraft_.What) == true)
            {
                NightlyDraft_.What = Candidate;
                if (NightlyBuild::ConfigAllowed(Candidate, NightlyDraft_.Config) == false)
                {
                    NightlyDraft_.Config = NightlyBuild::CONFIGS[0];
                }
            }
        }
        ImGui::EndCombo();
    }
    const bool UsesEngine = NightlyDraft_.What != NightlyBuild::Kind::BuildScript;
    if (UsesEngine == true)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Configuration");
        ImGui::SameLine(LabelWidth);
        ImGui::SetNextItemWidth(FieldWidth);
        if (ImGui::BeginCombo("##nightlyConfig", NightlyDraft_.Config.c_str()) == true)
        {
            for (const char* const Config : NightlyBuild::CONFIGS)
            {
                if (NightlyBuild::ConfigAllowed(NightlyDraft_.What, Config) == false)
                {
                    continue;
                }
                if (ImGui::Selectable(Config, NightlyDraft_.Config == Config) == true)
                {
                    NightlyDraft_.Config = Config;
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Start time");
    ImGui::SameLine(LabelWidth);
    ImGui::SetNextItemWidth(90 * Scale);
    if (ImGui::InputInt("##nightlyHour", &NightlyDraft_.Hour) == true)
    {
        NightlyDraft_.Hour = ((NightlyDraft_.Hour % 24) + 24) % 24;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(":");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * Scale);
    if (ImGui::InputInt("##nightlyMinute", &NightlyDraft_.Minute, 5) == true)
    {
        NightlyDraft_.Minute = ((NightlyDraft_.Minute % 60) + 60) % 60;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(24-hour clock)");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Days");
    ImGui::SameLine(LabelWidth);
    for (int Index = 0; Index < NightlyBuild::DAY_COUNT; Index++)
    {
        if (Index > 0)
        {
            ImGui::SameLine();
        }
        bool Checked = NightlyDraft_.Days[Index];
        if (ImGui::Checkbox(NightlyBuild::DAY_LABELS[Index], &Checked) == true)
        {
            NightlyDraft_.Days[Index] = Checked;
        }
    }

    if (NightlyDraft_.What == NightlyBuild::Kind::PackageGame)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Output folder");
        ImGui::SameLine(LabelWidth);
        ImGui::SetNextItemWidth(FieldWidth);
        const std::string Hint = NightlyBuild::DefaultOutputDir(Owner.Path, NightlyNameBuffer_);
        ImGui::InputTextWithHint("##nightlyOutput", Hint.c_str(), NightlyOutputBuffer_, sizeof NightlyOutputBuffer_);
    }
    if (UsesEngine == true)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Engine folder");
        ImGui::SameLine(LabelWidth);
        ImGui::SetNextItemWidth(FieldWidth);
        ImGui::InputTextWithHint("##nightlyEngine", "Found automatically from the project's engine version", NightlyEngineBuffer_, sizeof NightlyEngineBuffer_);
    }

    ImGui::Spacing();
    if (NightlyBuildError_.empty() == false)
    {
        ImGui::TextColored(WaitingColorV4_, "%s", NightlyBuildError_.c_str());
    }
    ImGui::BeginDisabled(NightlyBuild_.Busy() == true);
    if (ImGui::Button("Set up nightly build") == true)
    {
        NightlyBuild::Job Item = NightlyDraft_;
        Item.Name = NightlyNameBuffer_;
        Item.OutputDir = NightlyDraft_.What == NightlyBuild::Kind::PackageGame ? std::string(NightlyOutputBuffer_) : std::string();
        Item.EngineDir = UsesEngine == true ? std::string(NightlyEngineBuffer_) : std::string();
        NightlyBuildError_.clear();
        if (NightlyBuild_.Create(Owner.Path, Item, NightlyBuildError_) == true)
        {
            NightlyNameBuffer_[0] = 0;
        }
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Tip: use Run now on the Scheduled builds tab to check it works before leaving it overnight.");
}
