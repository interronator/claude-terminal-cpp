#include "App.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "BlenderMarkupExport.h"
#include "ImageCache.h"
#include "Platform.h"
#include "UiHelpers.h"

namespace fs = std::filesystem;

std::string App::BlenderOwnDirectory(const Session& Target)
{
    const std::string Tag = ScreenshotFinder::SessionTag(Target.Rec.Id);
    if (Tag.empty() == true)
    {
        return std::string();
    }
    return ScreenshotFinder::SessionShotDirectory(SessionProjectDir(Target), Tag).string();
}

bool App::DoesMeshWork(const Session& Target)
{
    if (IsBlenderReviewer(Target) == true)
    {
        return false;
    }
    const std::map<std::string, std::shared_ptr<TranscriptScanner>>::const_iterator Scanner = TranscriptScanners_.find(Target.Rec.Id);
    if (Scanner != TranscriptScanners_.end() && Scanner->second->LastMeshWork() != 0)
    {
        return true;
    }
    return IsBlenderSession(Target);
}

Session* App::ReviewTargetFor(const Session& Root)
{
    if (IsBlenderReviewer(Root) == true)
    {
        return FindSession(Root.Rec.ParentId);
    }
    Session* const RootSession = FindSession(Root.Rec.Id);
    if (RootSession != nullptr && DoesMeshWork(Root) == true)
    {
        return RootSession;
    }
    Session* BestSession = nullptr;
    std::time_t BestTime = 0;
    std::set<std::string> Visited{Root.Rec.Id};
    std::vector<std::string> Pending{Root.Rec.Id};
    while (Pending.empty() == false)
    {
        const std::string ParentId = Pending.back();
        Pending.pop_back();
        for (const std::unique_ptr<Session>& Candidate : Sessions_)
        {
            if (Candidate->Rec.ParentId != ParentId || IsBlenderReviewer(*Candidate) == true || Visited.insert(Candidate->Rec.Id).second == false)
            {
                continue;
            }
            Pending.push_back(Candidate->Rec.Id);
            const std::map<std::string, std::shared_ptr<TranscriptScanner>>::const_iterator Scanner = TranscriptScanners_.find(Candidate->Rec.Id);
            const std::time_t WorkTime = Scanner != TranscriptScanners_.end() ? Scanner->second->LastMeshWork() : 0;
            if (WorkTime > BestTime)
            {
                BestTime = WorkTime;
                BestSession = Candidate.get();
            }
        }
    }
    return BestSession;
}

bool App::HasFreshBlenderFrame(const Session& Target)
{
    const std::string Tag = ScreenshotFinder::SessionTag(Target.Rec.Id);
    if (Tag.empty() == true)
    {
        return false;
    }
    const LiveView::Frame Live = BlenderLive::Latest(BlenderOwnDirectory(Target), Tag);
    return Live.Path.empty() == false && Live.AgeSeconds < BlenderLive::STALE_FRAME_SECONDS;
}

void App::ToggleBlenderReview(const std::string& SessionId)
{
    if (BlenderReviewId_ == SessionId)
    {
        CloseBlenderReview();
        return;
    }
    OpenBlenderReview(SessionId);
}

// Unsent snapshots stay in the batch, so closing the window never loses finished review work.
void App::CloseBlenderReview()
{
    BlenderReviewId_.clear();
    BlenderFrozenPath_.clear();
    BlenderMarkup_.Clear();
    BlenderShownSnapshot_ = -1;
    BlenderPendingId_.clear();
}

void App::OpenBlenderReview(const std::string& RequestedId)
{
    Session* Target = FindSession(RequestedId);
    if (Target != nullptr && IsBlenderReviewer(*Target) == true)
    {
        Target = FindSession(Target->Rec.ParentId);
    }
    if (Target == nullptr)
    {
        return;
    }
    const std::string SessionId = Target->Rec.Id;
    if (BlenderReviewId_ != SessionId)
    {
        BlenderFrozenPath_.clear();
        BlenderMarkup_.Clear();
        BlenderShownSnapshot_ = -1;
        BlenderRightShowsMesh_ = false;
    }
    if (BlenderBatchOwnerId_ != SessionId)
    {
        BlenderBatch_.Clear();
        BlenderBatchOwnerId_ = SessionId;
        BlenderHistory_.Load(BlenderOwnDirectory(*Target), ScreenshotFinder::SessionTag(SessionId));
    }
    BlenderPendingId_.clear();
    BlenderView_.Reset();
    EnsureBlenderReviewer(*Target);
    OpenSessionFromSidebar(SessionId);
    BlenderReviewId_ = SessionId;
    BlenderReviewFocus_ = true;
    BlenderTouchAt_ = 0;
}

void App::TouchBlenderLive(Session& Target)
{
    constexpr double TOUCH_SECONDS = 3.0;
    if (Now_ < BlenderTouchAt_)
    {
        return;
    }
    BlenderTouchAt_ = Now_ + TOUCH_SECONDS;
    const std::string OwnDirectory = BlenderOwnDirectory(Target);
    if (OwnDirectory.empty() == true)
    {
        return;
    }
    BlenderLive::TouchWanted(OwnDirectory, ScreenshotFinder::SessionTag(Target.Rec.Id));
}

void App::DrawBlenderReview()
{
    if (BlenderReviewId_.empty() == true)
    {
        return;
    }
    Session* const Target = FindSession(BlenderReviewId_);
    if (Target == nullptr)
    {
        CloseBlenderReview();
        return;
    }
    TouchBlenderLive(*Target);

    const float Scale = UiScale_;
    ImGuiViewport* const Viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1200 * Scale, Viewport->WorkSize.x * 0.92f), std::min(720 * Scale, Viewport->WorkSize.y * 0.88f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(560 * Scale, 360 * Scale), ImVec2(Viewport->WorkSize.x, Viewport->WorkSize.y));
    if (BlenderReviewFocus_ == true)
    {
        ImGui::SetNextWindowFocus();
        BlenderReviewFocus_ = false;
        FocusSession(BlenderReviewId_);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * Scale, 8 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * Scale);
    bool Open = true;
    const std::string Title = "Blender review - " + Target->Rec.Name + "###blenderReview";
    if (ImGui::Begin(Title.c_str(), &Open, ImGuiWindowFlags_NoCollapse) == true)
    {
        const ImVec2 Avail = ImGui::GetContentRegionAvail();
        const float SplitWidth = 6 * Scale;
        const float MinSide = 220 * Scale;
        const float LeftWidth = std::clamp(Avail.x * BlenderSplitFraction_, MinSide, std::max(MinSide, Avail.x - MinSide - SplitWidth));
        DrawBlenderLeftTabs(*Target, LeftWidth, Avail.y);

        ImGui::SameLine(0, 0);
        ImGui::InvisibleButton("##blenderSplit", ImVec2(SplitWidth, Avail.y));
        if (ImGui::IsItemHovered() == true || ImGui::IsItemActive() == true)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsItemActive() == true && Avail.x > 0)
        {
            BlenderSplitFraction_ = std::clamp(BlenderSplitFraction_ + ImGui::GetIO().MouseDelta.x / Avail.x, 0.2f, 0.85f);
        }
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::IsItemHovered() == true || ImGui::IsItemActive() == true ? AccentColor_ : PaneBorderColorU32_);

        ImGui::SameLine(0, 0);
        Session* const Reviewer = FindBlenderReviewer(*Target);
        Session* const Shown = Reviewer != nullptr && BlenderRightShowsMesh_ == false ? Reviewer : Target;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##blenderRight", ImVec2(0, Avail.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawBlenderRightHeader(*Target, Reviewer);
        ImGui::SetCursorPos(ImVec2(0, ImGui::GetCursorPosY() + 3 * Scale));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(TerminalBgColorU32_));
        ImGui::BeginChild("##blenderTerminal", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) == true && ImGui::IsMouseClicked(ImGuiMouseButton_Left) == true)
        {
            FocusSession(Shown->Rec.Id);
        }
        if (Shown != Target && Shown->IsRunning() == false)
        {
            ImGui::SetCursorPos(ImVec2(12 * Scale, 12 * Scale));
            ImGui::TextDisabled("Starting %s...", Shown->Rec.Name.c_str());
        }
        else
        {
            ImGui::PushID("blenderTerminalView");
            DrawTerminal(*Shown, ImGui::GetContentRegionAvail(), State_.Ui.ActiveId == Shown->Rec.Id);
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (Open == false)
    {
        CloseBlenderReview();
    }
}
