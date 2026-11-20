#include "App.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "ImageCache.h"
#include "MeshGenerator.h"
#include "Platform.h"

namespace fs = std::filesystem;

void App::DrawBlenderLeftTabs(Session& Target, const float Width, const float Height)
{
    ImGui::BeginChild("##blenderLeftTabs", ImVec2(Width, Height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (ImGui::BeginTabBar("##blenderLeftTabBar") == true)
    {
        if (ImGui::BeginTabItem("Live view") == true)
        {
            const ImVec2 Area = ImGui::GetContentRegionAvail();
            DrawBlenderReviewLeft(Target, Area.x, Area.y);
            ImGui::EndTabItem();
        }
        const bool Busy = GeneratorProgress_.Running == true;
        const std::string GeneratorLabel = std::string(Busy == true ? "Generator *" : "Generator") + "###generatorTab";
        if (ImGui::BeginTabItem(GeneratorLabel.c_str()) == true)
        {
            DrawMeshGeneratorTab(Target);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

void App::PollMeshGenerator()
{
    if (Now_ - GeneratorPolledAt_ <= 0.5 && GeneratorPolledAt_ <= Now_)
    {
        return;
    }
    GeneratorPolledAt_ = Now_;
    GeneratorProgress_ = Generator_.Poll();
    AdvanceMeshGeneratorQueue();
    RefreshMeshGeneratorCache();
}

void App::RefreshMeshGeneratorCache()
{
    GeneratorConcepts_ = Generator_.ListConcepts();
    std::error_code ErrorCode;
    GeneratorHasMesh_ = fs::exists(Generator_.MeshPath(), ErrorCode);
    GeneratorReport_ = Generator_.ReadReport();
    GeneratorReportAt_ = Now_;
    GeneratorFileKeys_.clear();
    for (const std::string& Path : GeneratorConcepts_)
    {
        GeneratorFileKeys_[Path] = MeshGenerator::FileKey(Path);
    }
    for (const std::string& Path : GeneratorReport_.RenderPaths)
    {
        GeneratorFileKeys_[Path] = MeshGenerator::FileKey(Path);
    }
}

long long App::GeneratorFileKey(const std::string& Path) const
{
    const std::map<std::string, long long>::const_iterator Found = GeneratorFileKeys_.find(Path);
    return Found == GeneratorFileKeys_.end() ? 0 : Found->second;
}

void App::AdvanceMeshGeneratorQueue()
{
    if (GeneratorProgress_.Finished == false || GeneratorHandled_ == true)
    {
        return;
    }
    GeneratorHandled_ = true;
    const std::string Stage = MeshGenerator::StageName(GeneratorProgress_.Current);
    if (GeneratorProgress_.Cancelled == true)
    {
        Generator_.Queue.clear();
        ShowToast(Stage + " cancelled", false);
        return;
    }
    if (GeneratorProgress_.ExitCode != 0)
    {
        Generator_.Queue.clear();
        ShowToast(Stage + " failed (exit " + std::to_string(GeneratorProgress_.ExitCode) + "). See the log in the Generator tab.", true);
        return;
    }
    if (GeneratorProgress_.Current == MeshGenerator::Stage::Concepts && Generator_.Queue.empty() == false && Generator_.SelectedConcept.empty() == true)
    {
        for (const std::string& Path : Generator_.ListConcepts())
        {
            if (Generator_.RejectedConcepts.count(Path) == 0)
            {
                Generator_.SelectedConcept = Path;
                break;
            }
        }
    }
    if (Generator_.Queue.empty() == true)
    {
        ShowToast(Stage + " finished", false);
        return;
    }
    const MeshGenerator::Stage Next = Generator_.Queue.front();
    Generator_.Queue.erase(Generator_.Queue.begin());
    const std::string Error = Generator_.Start(Next);
    if (Error.empty() == false)
    {
        Generator_.Queue.clear();
        ShowToast(Error, true);
        return;
    }
    GeneratorHandled_ = false;
    GeneratorProgress_ = Generator_.Poll();
}

void App::StartMeshGeneratorStage(const MeshGenerator::Stage Kind, const bool RunAll)
{
    Generator_.Queue.clear();
    if (RunAll == true && Kind == MeshGenerator::Stage::Concepts)
    {
        Generator_.Queue = {MeshGenerator::Stage::Mesh, MeshGenerator::Stage::Cleanup};
    }
    else if (RunAll == true && Kind == MeshGenerator::Stage::Mesh)
    {
        Generator_.Queue = {MeshGenerator::Stage::Cleanup};
    }
    const std::string Error = Generator_.Start(Kind);
    if (Error.empty() == false)
    {
        Generator_.Queue.clear();
        ShowToast(Error, true);
        return;
    }
    GeneratorHandled_ = false;
    GeneratorProgress_ = Generator_.Poll();
}

void App::DrawMeshGeneratorTab(Session& Target)
{
    const float Scale = UiScale_;
    ImGui::BeginChild("##generatorBody", ImVec2(0, 0), ImGuiChildFlags_None);
    const bool Busy = GeneratorProgress_.Running == true;
    MeshGenerator::Brief& Brief = Generator_.Draft;

    if (ImGui::CollapsingHeader("Paths") == true)
    {
        MeshGenerator::Settings& Config = Generator_.Config;
        DrawMeshGeneratorPathField("Scripts folder", Config.ScriptsDir);
        DrawMeshGeneratorPathField("Image venv", Config.ImageVenvDir);
        DrawMeshGeneratorPathField("Mesh venv", Config.MeshVenvDir);
        DrawMeshGeneratorPathField("Blender exe", Config.BlenderExe);
        DrawMeshGeneratorPathField("Jobs folder", Config.JobsRoot);
        DrawMeshGeneratorPathField("RamCheck script", Config.RamCheckScript);
        if (ImGui::Button("Save paths") == true)
        {
            Generator_.SaveSettings();
        }
    }

    ImGui::SeparatorText("Brief");
    ImGui::BeginDisabled(Busy);
    ImGui::SetNextItemWidth(160 * Scale);
    ImGui::InputText("Asset name", Brief.Name, sizeof Brief.Name);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * Scale);
    const char* const Kinds[] = {"Prop", "Character"};
    ImGui::Combo("Kind", &Brief.Kind, Kinds, 2);
    ImGui::SetNextItemWidth(-130 * Scale);
    ImGui::InputText("Subject", Brief.Subject, sizeof Brief.Subject);
    ImGui::SetNextItemWidth(110 * Scale);
    ImGui::InputFloat("Size (m)", &Brief.SizeMeters, 0.1f, 0.5f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * Scale);
    ImGui::InputInt("Triangles", &Brief.TriBudget, 1000, 5000);
    ImGui::SetNextItemWidth(110 * Scale);
    const int TextureSizes[] = {1024, 2048, 4096};
    int TextureIndex = Brief.TextureSize >= 4096 ? 2 : Brief.TextureSize >= 2048 ? 1 : 0;
    const char* const TextureLabels[] = {"1024", "2048", "4096"};
    if (ImGui::Combo("Texture", &TextureIndex, TextureLabels, 3) == true)
    {
        Brief.TextureSize = TextureSizes[TextureIndex];
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * Scale);
    ImGui::InputInt("Concepts", &Brief.ConceptCount);
    ImGui::SetNextItemWidth(110 * Scale);
    ImGui::InputInt("Concept seed", &Brief.ConceptSeed, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * Scale);
    ImGui::InputInt("Mesh seed", &Brief.MeshSeed, 1, 10);
    ImGui::EndDisabled();
    Brief.SizeMeters = std::clamp(Brief.SizeMeters, 0.01f, 100.0f);
    Brief.TriBudget = std::clamp(Brief.TriBudget, 100, 2000000);
    Brief.ConceptCount = std::clamp(Brief.ConceptCount, 1, 12);

    ImGui::SeparatorText("Steps");
    const std::vector<std::string>& Concepts = GeneratorConcepts_;
    const bool HasMesh = GeneratorHasMesh_;
    ImGui::BeginDisabled(Busy);
    if (ImGui::Button("1 Concepts") == true)
    {
        StartMeshGeneratorStage(MeshGenerator::Stage::Concepts, false);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(Generator_.SelectedConcept.empty() == true);
    if (ImGui::Button("2 Mesh") == true)
    {
        StartMeshGeneratorStage(MeshGenerator::Stage::Mesh, false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(HasMesh == false);
    if (ImGui::Button("3 Cleanup") == true)
    {
        StartMeshGeneratorStage(MeshGenerator::Stage::Cleanup, false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Run all") == true)
    {
        Generator_.SelectedConcept.clear();
        StartMeshGeneratorStage(MeshGenerator::Stage::Concepts, true);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) == true)
    {
        ImGui::SetTooltip("Concepts, then the first concept picked automatically, then Mesh and Cleanup");
    }

    DrawMeshGeneratorStatus();

    ImGui::SeparatorText("Concepts");
    if (Concepts.empty() == true)
    {
        ImGui::TextDisabled("No concept pictures yet. Press Concepts.");
    }
    const float Thumb = 120.0f * Scale;
    const float Wrap = ImGui::GetContentRegionAvail().x;
    float Used = 0.0f;
    for (const std::string& Path : Concepts)
    {
        if (Used + Thumb > Wrap && Used > 0.0f)
        {
            Used = 0.0f;
        }
        else if (Used > 0.0f)
        {
            ImGui::SameLine();
        }
        Used += Thumb + ImGui::GetStyle().ItemSpacing.x;
        const bool Rejected = Generator_.RejectedConcepts.count(Path) != 0;
        const bool Selected = Generator_.SelectedConcept == Path;
        const ImageCache::Texture* const Image = Images_->Get(Path, static_cast<std::time_t>(GeneratorFileKey(Path)));
        ImGui::PushID(Path.c_str());
        ImGui::BeginGroup();
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const bool Clicked = ImGui::InvisibleButton("##concept", ImVec2(Thumb, Thumb));
        const bool RightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        ImDrawList* const Draw = ImGui::GetWindowDrawList();
        if (Image != nullptr && Image->Failed == false && Image->Width > 0 && Image->Height > 0)
        {
            const float Aspect = static_cast<float>(Image->Height) / static_cast<float>(Image->Width);
            float DrawWidth = Thumb;
            float DrawHeight = Thumb * Aspect;
            if (DrawHeight > Thumb)
            {
                DrawHeight = Thumb;
                DrawWidth = Thumb / Aspect;
            }
            const ImVec2 Min(Origin.x + (Thumb - DrawWidth) * 0.5f, Origin.y + (Thumb - DrawHeight) * 0.5f);
            Draw->AddImage(Image->Id, Min, ImVec2(Min.x + DrawWidth, Min.y + DrawHeight), ImVec2(0, 0), ImVec2(1, 1), Rejected == true ? IM_COL32(255, 255, 255, 70) : IM_COL32_WHITE);
        }
        Draw->AddRect(Origin, ImVec2(Origin.x + Thumb, Origin.y + Thumb), Selected == true ? AccentColor_ : PaneBorderColorU32_, 0.0f, 0, Selected == true ? 3.0f : 1.0f);
        if (Rejected == true)
        {
            Draw->AddText(ImVec2(Origin.x + 6.0f, Origin.y + 4.0f), IM_COL32(230, 80, 80, 255), "rejected");
        }
        ImGui::EndGroup();
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Click to choose, right-click to reject\n%s", fs::path(Path).filename().string().c_str());
        }
        if (Clicked == true && Busy == false)
        {
            Generator_.SelectedConcept = Path;
            Generator_.RejectedConcepts.erase(Path);
        }
        if (RightClicked == true && Rejected == true)
        {
            Generator_.RejectedConcepts.erase(Path);
        }
        if (RightClicked == true && Rejected == false)
        {
            Generator_.RejectedConcepts.insert(Path);
            if (Selected == true)
            {
                Generator_.SelectedConcept.clear();
            }
        }
        ImGui::PopID();
    }

    DrawMeshGeneratorResult(Target);
    ImGui::EndChild();
}

void App::DrawMeshGeneratorPathField(const char* const Label, std::string& Value)
{
    char Buffer[512];
    std::snprintf(Buffer, sizeof Buffer, "%s", Value.c_str());
    ImGui::SetNextItemWidth(-130 * UiScale_);
    if (ImGui::InputText(Label, Buffer, sizeof Buffer) == true)
    {
        Value = Buffer;
    }
}

void App::DrawMeshGeneratorStatus()
{
    const float Scale = UiScale_;
    const MeshGenerator::Progress& Progress = GeneratorProgress_;
    ImGui::SeparatorText("Job");
    if (Progress.Running == false && Progress.Finished == false)
    {
        ImGui::TextDisabled("Nothing running.");
        return;
    }
    const char* const Stage = MeshGenerator::StageName(Progress.Current);
    if (Progress.Running == true)
    {
        const char* const State = Progress.WaitingForRam == true ? "waiting for free RAM" : "running";
        ImGui::Text("%s: %s, %d s", Stage, State, static_cast<int>(Progress.ElapsedSeconds));
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel") == true)
        {
            Generator_.Queue.clear();
            Generator_.Cancel();
        }
        const ImVec2 Size(-1, 14 * Scale);
        if (Progress.Fraction >= 0.0f)
        {
            ImGui::ProgressBar(Progress.Fraction, Size);
        }
        if (Progress.Fraction < 0.0f)
        {
            ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), Size, Progress.WaitingForRam == true ? "waiting" : "working");
        }
    }
    if (Progress.Running == false)
    {
        const ImVec4 Color = Progress.ExitCode == 0 ? ImVec4(0.4f, 0.85f, 0.4f, 1.0f) : ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
        const char* const Outcome = Progress.Cancelled == true ? "cancelled" : Progress.ExitCode == 0 ? "finished" : "failed";
        ImGui::TextColored(Color, "%s %s after %d s", Stage, Outcome, static_cast<int>(Progress.ElapsedSeconds));
    }
    if (Progress.LogTail.empty() == false)
    {
        ImGui::BeginChild("##generatorLog", ImVec2(0, 110 * Scale), ImGuiChildFlags_Borders);
        ImGui::PushStyleColor(ImGuiCol_Text, TextDisabledColorV4_);
        ImGui::TextUnformatted(Progress.LogTail.c_str());
        ImGui::PopStyleColor();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f)
        {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
}

void App::DrawMeshGeneratorResult(Session& Target)
{
    const float Scale = UiScale_;
    ImGui::SeparatorText("Result");
    const MeshGenerator::Report& Report = GeneratorReport_;
    if (Report.Valid == false)
    {
        ImGui::TextDisabled("No cleanup result for '%s' yet.", Generator_.AssetName().c_str());
        DrawMeshGeneratorAdvice();
        return;
    }
    ImGui::Text("%d triangles, %.2f x %.2f x %.2f m, cleanup took %d s", Report.Tris, Report.Bounds[0], Report.Bounds[1], Report.Bounds[2], static_cast<int>(Report.Seconds));
    if (Report.Flags.empty() == true)
    {
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "All automatic checks passed.");
    }
    for (const std::string& Flag : Report.Flags)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "! %s", Flag.c_str());
    }
    const float Thumb = std::clamp((ImGui::GetContentRegionAvail().x - 3 * ImGui::GetStyle().ItemSpacing.x) / 4.0f, 60.0f * Scale, 200.0f * Scale);
    for (size_t Index = 0; Index < Report.RenderPaths.size(); Index++)
    {
        const std::string& Path = Report.RenderPaths[Index];
        const ImageCache::Texture* const Image = Images_->Get(Path, static_cast<std::time_t>(GeneratorFileKey(Path)));
        if (Index > 0)
        {
            ImGui::SameLine();
        }
        if (Image != nullptr && Image->Failed == false)
        {
            ImGui::Image(Image->Id, ImVec2(Thumb, Thumb));
        }
        else
        {
            ImGui::Dummy(ImVec2(Thumb, Thumb));
        }
    }
    if (ImGui::Button("Open folder") == true)
    {
        Platform::Reveal(Report.Folder);
    }
    ImGui::SameLine();
    if (ImGui::Button("Show .blend") == true)
    {
        Platform::ShowInFolder(Report.BlendPath);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(Report.RenderPaths.empty() == true || GeneratorProgress_.Running == true);
    if (ImGui::Button("Send result to reviewer") == true)
    {
        SendMeshGeneratorToReviewer(Target);
    }
    ImGui::EndDisabled();
    DrawMeshGeneratorAdvice();
}

void App::DrawMeshGeneratorAdvice()
{
    ImGui::SeparatorText("Apply reviewer advice");
    ImGui::TextDisabled("Paste lines such as  prompt: ...   seed: 7   tris: 8000   size: 1.2");
    ImGui::InputTextMultiline("##generatorAdvice", Generator_.AdviceText, sizeof Generator_.AdviceText, ImVec2(-1, 70 * UiScale_));
    ImGui::BeginDisabled(GeneratorProgress_.Running == true);
    if (ImGui::Button("Apply to brief") == true)
    {
        const std::string Applied = Generator_.ApplyAdvice(Generator_.AdviceText);
        ShowToast(Applied.empty() == true ? "No usable advice lines found." : "Updated the brief: " + Applied, Applied.empty() == true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply and re-run Concepts") == true)
    {
        Generator_.ApplyAdvice(Generator_.AdviceText);
        StartMeshGeneratorStage(MeshGenerator::Stage::Concepts, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply and re-run Mesh") == true)
    {
        Generator_.ApplyAdvice(Generator_.AdviceText);
        StartMeshGeneratorStage(MeshGenerator::Stage::Mesh, true);
    }
    ImGui::EndDisabled();
}
