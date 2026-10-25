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


void App::PollScreenshots(Session& CurrentSession)
{
    constexpr double POLL_INTERVAL_SECONDS = 1.5;
    constexpr double FIRST_POLL_STAGGER_SECONDS = 0.05;
    const std::string& Id = CurrentSession.Rec.Id;
    std::map<std::string, double>::iterator NextPoll = ScreenshotPollAt_.find(Id);
    if (NextPoll == ScreenshotPollAt_.end())
    {
        ScreenshotPollAt_[Id] = Now_ + FIRST_POLL_STAGGER_SECONDS * static_cast<double>(ScreenshotPollAt_.size());
        return;
    }
    if (Now_ < NextPoll->second)
    {
        return;
    }
    NextPoll->second = Now_ + POLL_INTERVAL_SECONDS;

    const std::string Tag = ScreenshotFinder::SessionTag(Id);
    const std::string OwnDirectory = Tag.empty() == true ? std::string() : ScreenshotFinder::SessionShotDirectory(SessionProjectDir(CurrentSession), Tag).string();
    TranscriptScanner& Scanner = TranscriptScanners_[Id];
    const std::vector<TranscriptScanner::Shot> Shots = Scanner.Poll(CurrentSession.Rec.ClaudeSessionId, CurrentSession.Rec.Cwd, OwnDirectory, Tag, Now_);
    CurrentSession.BackgroundTasks = CurrentSession.IsRunning() == true ? Scanner.RunningBackgroundTasks(CurrentSession.LaunchedAt) : 0;
    RunningCommands_[Id] = CurrentSession.IsRunning() == true ? Scanner.RunningCommands(CurrentSession.LaunchedAt) : std::vector<TranscriptScanner::ActiveCommand>();
    for (const TranscriptScanner::Shot& Found : Shots)
    {
        // A session can mention another session's screenshot; the tag in the path says who really made it.
        const Session* const TagOwner = FindSessionByTag(ScreenshotFinder::TagIn(Found.Path));
        const std::string& OwnerId = TagOwner != nullptr ? TagOwner->Rec.Id : Id;
        if (ScreenshotFeed_.Add(OwnerId, Found.Path, Found.ModifiedAt, TagOwner != nullptr) == true && State_.Ui.ScreenshotPanelOpen == false)
        {
            UnseenScreenshots_++;
        }
    }
}

void App::DrawScreenshotPanel(const float Width)
{
    const float Scale = UiScale_;
    const bool ProjectSelected = ProjectScopeId_.empty() == false;

    std::vector<const ScreenshotFeed::Entry*> Visible;
    std::set<std::string> VisiblePaths;
    for (const ScreenshotFeed::Entry& Item : ScreenshotFeed_.Entries())
    {
        if (ProjectSelected == true)
        {
            const Session* const Owner = FindSession(Item.SessionId);
            if (Owner == nullptr || Owner->Rec.ProjectId != ProjectScopeId_)
            {
                continue;
            }
        }
        else if (Item.SessionId != State_.Ui.ActiveId)
        {
            continue;
        }
        if (State_.Ui.HiddenScreenshots.count(Item.Path) > 0)
        {
            continue;
        }
        if (VisiblePaths.insert(Item.Path).second == false)
        {
            continue;
        }
        Visible.push_back(&Item);
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, SurfaceColorV4_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * Scale, 8 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * Scale, 6 * Scale));
    ImGui::BeginChild("##shots", ImVec2(Width, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Screenshots");
    ImGui::SameLine();
    ImGui::TextDisabled("%d", static_cast<int>(Visible.size()));
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetFrameHeight() - 8 * Scale);
    if (ImGui::Button(">", ImVec2(ImGui::GetFrameHeight(), 0)) == true)
    {
        State_.Ui.ScreenshotPanelOpen = false;
        MarkDirty();
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Hide screenshots");
    }

    ImGui::TextDisabled("%s", ProjectSelected == true ? "From every session in this project" : "From the selected session");

    if (State_.Ui.HiddenScreenshots.empty() == false)
    {
        const std::string RestoreLabel = "Restore " + std::to_string(State_.Ui.HiddenScreenshots.size()) + " hidden###shotRestore";
        if (ImGui::SmallButton(RestoreLabel.c_str()) == true)
        {
            State_.Ui.HiddenScreenshots.clear();
            MarkDirty();
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Show every screenshot you've hidden again");
        }
        ImGui::SameLine();
        const std::string DeleteHiddenLabel = "Delete " + std::to_string(State_.Ui.HiddenScreenshots.size()) + " hidden###shotDeleteHidden";
        if (ImGui::SmallButton(DeleteHiddenLabel.c_str()) == true)
        {
            const std::vector<std::string> Paths(State_.Ui.HiddenScreenshots.begin(), State_.Ui.HiddenScreenshots.end());
            ConfirmTitle_ = "Delete " + std::to_string(Paths.size()) + " hidden photos?";
            ConfirmText_ = "This permanently deletes every screenshot you've hidden from disk.";
            ConfirmYesAction_ = [this, Paths]
            {
                for (const std::string& Path : Paths)
                {
                    std::error_code ErrorCode;
                    std::filesystem::remove(Path, ErrorCode);
                    ScreenshotFeed_.RemovePath(Path);
                    ShotAspects_.erase(Path);
                    State_.Ui.HiddenScreenshots.erase(Path);
                }
                MarkDirty();
            };
            OpenModal(Modal::Confirm);
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Permanently delete the files of every hidden screenshot");
        }
        ImGui::SameLine();
    }
    if (Visible.empty() == false)
    {
        const std::string DeleteLabel = "Delete " + std::to_string(Visible.size()) + " photos###shotDelete";
        if (ImGui::SmallButton(DeleteLabel.c_str()) == true)
        {
            std::vector<std::string> Paths;
            for (const ScreenshotFeed::Entry* const Item : Visible)
            {
                Paths.push_back(Item->Path);
            }
            ConfirmTitle_ = "Delete " + std::to_string(Paths.size()) + " photos?";
            ConfirmText_ = "This permanently deletes the image files shown in this panel from disk.";
            ConfirmYesAction_ = [this, Paths]
            {
                for (const std::string& Path : Paths)
                {
                    std::error_code ErrorCode;
                    std::filesystem::remove(Path, ErrorCode);
                    ScreenshotFeed_.RemovePath(Path);
                    State_.Ui.HiddenScreenshots.erase(Path);
                }
                MarkDirty();
            };
            OpenModal(Modal::Confirm);
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Permanently delete the files listed below");
        }
    }

    ImGui::BeginChild("##shotList", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (Visible.empty() == true)
    {
        ImGui::TextDisabled("%s", ProjectSelected == true ? "No screenshots from this project's sessions yet." : "No screenshots from the selected session yet.");
    }
    const float CardWidth = ImGui::GetContentRegionAvail().x;
    std::vector<std::string> MissingPaths;
    for (const ScreenshotFeed::Entry* const Item : Visible)
    {
        if (DrawScreenshotCard(*Item, CardWidth) == false)
        {
            MissingPaths.push_back(Item->Path);
        }
    }
    for (const std::string& Path : MissingPaths)
    {
        ScreenshotFeed_.RemovePath(Path);
        ShotAspects_.erase(Path);
    }
    ImGui::EndChild();

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

bool App::DrawScreenshotCard(const ScreenshotFeed::Entry& Item, const float Width)
{
    const float Scale = UiScale_;
    const float Pad = 8.0f * Scale;
    const float Rounding = 6.0f * Scale;
    const float StripeWidth = 4.0f * Scale;
    const float MaxImageHeight = 260.0f * Scale;
    const float HeaderHeight = ImGui::GetFrameHeight();
    const float CaptionHeight = ImGui::GetTextLineHeight();

    const Session* const Owner = FindSession(Item.SessionId);
    const ImU32 Tint = UiHelpers::SessionTint(Item.SessionId);
    const float AvailableWidth = Width - StripeWidth - Pad * 2;
    constexpr float DEFAULT_ASPECT = 0.5625f;
    const std::map<std::string, float>::const_iterator KnownAspect = ShotAspects_.find(Item.Path);
    float Aspect = KnownAspect != ShotAspects_.end() ? KnownAspect->second : DEFAULT_ASPECT;

    // Only cards on screen ask the cache for a texture; asking for all of them every frame would
    // thrash the cache and make cards flip between "Loading..." and their image.
    const ImVec2 CardTopLeft = ImGui::GetCursorScreenPos();
    const float EstimatedHeight = HeaderHeight + Pad + std::min(AvailableWidth * Aspect, MaxImageHeight) + Pad * 0.5f + CaptionHeight + Pad;
    const bool OnScreen = ImGui::IsRectVisible(CardTopLeft, ImVec2(CardTopLeft.x + Width, CardTopLeft.y + EstimatedHeight));
    const ImageCache::Texture* const Image = OnScreen == true ? Images_->Get(Item.Path, Item.Time) : nullptr;
    const bool HasImage = Image != nullptr && Image->Failed == false;
    if (Image != nullptr && Image->Failed == true)
    {
        std::error_code ExistsError;
        if (std::filesystem::exists(Item.Path, ExistsError) == false)
        {
            return false;
        }
    }
    if (HasImage == true)
    {
        Aspect = static_cast<float>(Image->Height) / static_cast<float>(Image->Width);
        ShotAspects_[Item.Path] = Aspect;
    }

    const float ImageHeight = std::min(AvailableWidth * Aspect, MaxImageHeight);
    const float ImageWidth = HasImage == true ? ImageHeight / Aspect : AvailableWidth;
    const float CardHeight = HeaderHeight + Pad + ImageHeight + Pad * 0.5f + CaptionHeight + Pad;

    ImGui::PushID(Item.SessionId.c_str());
    ImGui::PushID(Item.Path.c_str());
    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    const ImVec2 Origin = ImGui::GetCursorScreenPos();
    const ImVec2 CardMax(Origin.x + Width, Origin.y + CardHeight);

    DrawList->AddRectFilled(Origin, CardMax, PaneBgColorU32_, Rounding);
    const bool IsHighlighted = Owner != nullptr && (State_.Ui.ActiveId == Item.SessionId || Owner->CurrentStatus == Status::Waiting);
    const ImU32 BorderColor = Owner != nullptr && Owner->CurrentStatus == Status::Waiting ? WaitingColor_
                              : IsHighlighted == true                                     ? AccentColor_
                                                                                          : UiHelpers::WithAlpha(Tint, 110);
    DrawList->AddRectFilled(Origin, ImVec2(Origin.x + Width, Origin.y + HeaderHeight), UiHelpers::WithAlpha(Tint, IsHighlighted == true ? 110 : 60), Rounding,
                            ImDrawFlags_RoundCornersTop);
    DrawList->AddRectFilled(Origin, ImVec2(Origin.x + StripeWidth, CardMax.y), Tint, Rounding, ImDrawFlags_RoundCornersLeft);
    DrawList->AddRect(Origin, CardMax, BorderColor, Rounding, 0, IsHighlighted == true ? 2.0f * Scale : 1.0f);

    const float CloseWidth = HeaderHeight;
    const std::string AgoText = UiHelpers::FormatAgo(Item.Time);
    const float AgoWidth = ImGui::CalcTextSize(AgoText.c_str()).x + CloseWidth;
    const float TextY = Origin.y + (HeaderHeight - ImGui::GetTextLineHeight()) * 0.5f;
    const float TextLeft = Origin.x + StripeWidth + Pad;
    const float DotRadius = 4.5f * Scale;
    if (Owner != nullptr)
    {
        DrawStatusDot(DrawList, ImVec2(TextLeft + DotRadius, Origin.y + HeaderHeight * 0.5f), DotRadius, *Owner);
    }
    else
    {
        DrawList->AddCircle(ImVec2(TextLeft + DotRadius, Origin.y + HeaderHeight * 0.5f), DotRadius, Tint, 0, 1.5f * Scale);
    }

    const Project* const OwnerProject = Owner != nullptr ? FindProject(Owner->Rec.ProjectId) : nullptr;
    const std::string SessionName = Owner != nullptr ? Owner->Rec.Name : std::string("(closed session)");
    const std::string ProjectName = OwnerProject != nullptr ? OwnerProject->Name : std::string();
    const float NameX = TextLeft + DotRadius * 2 + 6 * Scale;
    DrawList->PushClipRect(ImVec2(NameX, Origin.y), ImVec2(Origin.x + Width - AgoWidth - Pad * 2, CardMax.y), true);
    DrawList->AddText(ImVec2(NameX, TextY), Tint, SessionName.c_str());
    if (ProjectName.empty() == false)
    {
        DrawList->AddText(ImVec2(NameX + ImGui::CalcTextSize(SessionName.c_str()).x + 8 * Scale, TextY), TextDisabledColorU32_, ProjectName.c_str());
    }
    DrawList->PopClipRect();
    DrawList->AddText(ImVec2(Origin.x + Width - AgoWidth - Pad, TextY), TextDisabledColorU32_, AgoText.c_str());

    ImGui::SetCursorScreenPos(Origin);
    if (ImGui::InvisibleButton("##header", ImVec2(Width - CloseWidth, HeaderHeight)) == true && Owner != nullptr)
    {
        OpenSessionFromSidebar(Item.SessionId);
    }
    if (ImGui::IsItemHovered() == true)
    {
        DrawList->AddRectFilled(Origin, ImVec2(Origin.x + Width - CloseWidth, Origin.y + HeaderHeight), IM_COL32(255, 255, 255, 18), Rounding, ImDrawFlags_RoundCornersTopLeft);
        ImGui::SetTooltip(Owner != nullptr ? "Go to this session" : "This session was closed");
    }

    ImGui::SetCursorScreenPos(ImVec2(Origin.x + Width - CloseWidth, Origin.y));
    const bool Dismissed = ImGui::InvisibleButton("##hide", ImVec2(CloseWidth, HeaderHeight));
    const bool CloseHovered = ImGui::IsItemHovered();
    const ImVec2 CloseCenter(Origin.x + Width - CloseWidth * 0.5f, Origin.y + HeaderHeight * 0.5f);
    const float CrossSize = 4.0f * Scale;
    const ImU32 CrossColor = CloseHovered == true ? TextColorU32_ : TextDisabledColorU32_;
    DrawList->AddLine(ImVec2(CloseCenter.x - CrossSize, CloseCenter.y - CrossSize), ImVec2(CloseCenter.x + CrossSize, CloseCenter.y + CrossSize), CrossColor, 1.5f * Scale);
    DrawList->AddLine(ImVec2(CloseCenter.x - CrossSize, CloseCenter.y + CrossSize), ImVec2(CloseCenter.x + CrossSize, CloseCenter.y - CrossSize), CrossColor, 1.5f * Scale);
    if (CloseHovered == true)
    {
        ImGui::SetTooltip("Hide this screenshot");
    }

    const ImVec2 ImageMin(Origin.x + StripeWidth + Pad + (AvailableWidth - ImageWidth) * 0.5f, Origin.y + HeaderHeight + Pad);
    const ImVec2 ImageMax(ImageMin.x + ImageWidth, ImageMin.y + ImageHeight);
    if (HasImage == true)
    {
        DrawList->AddImage(Image->Id, ImageMin, ImageMax);
    }
    else
    {
        DrawList->AddRectFilled(ImageMin, ImageMax, SurfaceColorU32_, 4 * Scale);
        const char* const Message = Image == nullptr ? "Loading..." : "Can't preview this file";
        const ImVec2 MessageSize = ImGui::CalcTextSize(Message);
        DrawList->AddText(ImVec2(ImageMin.x + (ImageWidth - MessageSize.x) * 0.5f, ImageMin.y + (ImageHeight - MessageSize.y) * 0.5f), TextDisabledColorU32_, Message);
    }
    ImGui::SetCursorScreenPos(ImageMin);
    if (ImGui::InvisibleButton("##image", ImVec2(ImageWidth, ImageHeight)) == true)
    {
        Platform::Reveal(Item.Path);
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s\nClick to open", Item.Path.c_str());
    }
    bool DismissedSession = false;
    if (ImGui::BeginPopupContextItem("##imageMenu") == true)
    {
        if (ImGui::MenuItem("Select this session", nullptr, false, Owner != nullptr) == true)
        {
            OpenSessionFromSidebar(Item.SessionId);
        }
        if (ImGui::MenuItem("Show in Explorer") == true)
        {
            Platform::ShowInFolder(Item.Path);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete all from session") == true)
        {
            DismissedSession = true;
        }
        ImGui::EndPopup();
    }

    const std::string FileName = std::filesystem::path(Item.Path).filename().string();
    DrawList->PushClipRect(ImVec2(ImageMin.x, ImageMax.y), ImVec2(Origin.x + Width - Pad, CardMax.y), true);
    DrawList->AddText(ImVec2(Origin.x + StripeWidth + Pad, ImageMax.y + Pad * 0.5f), TextDisabledColorU32_, FileName.c_str());
    DrawList->PopClipRect();

    ImGui::SetCursorScreenPos(Origin);
    ImGui::Dummy(ImVec2(Width, CardHeight));
    ImGui::PopID();
    ImGui::PopID();
    if (Dismissed == true)
    {
        State_.Ui.HiddenScreenshots.insert(Item.Path);
        MarkDirty();
    }
    if (DismissedSession == true)
    {
        for (const ScreenshotFeed::Entry& Other : ScreenshotFeed_.Entries())
        {
            if (Other.SessionId != Item.SessionId)
            {
                continue;
            }
            State_.Ui.HiddenScreenshots.insert(Other.Path);
        }
        MarkDirty();
    }
    return true;
}
