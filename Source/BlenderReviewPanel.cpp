#include "App.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>

#include "BlenderMarkupExport.h"
#include "BlenderSnapshotState.h"
#include "nlohmann/json.hpp"
#include "ImageCache.h"
#include "PeerNames.h"
#include "UiHelpers.h"

namespace fs = std::filesystem;

void App::StartBlenderMarking(const BlenderMarkup::Tool Kind)
{
    BlenderShownSnapshot_ = -1;
    BlenderMarkup_.Clear();
    BlenderMarkup_.SetTool(Kind);
    BlenderMarkup_.SetFirstNumber(BlenderBatch_.NextNumber());
}

void App::FreezeBlenderFromLive(const Session& Target, const BlenderMarkup::Tool Kind, const std::string& Id)
{
    const std::string OwnDirectory = BlenderOwnDirectory(Target);
    const std::string Tag = ScreenshotFinder::SessionTag(Target.Rec.Id);
    const LiveView::Frame Live = BlenderLive::Latest(OwnDirectory, Tag);
    if (Live.Path.empty() == true)
    {
        ShowToast("No live Blender picture yet. Use Connect Blender first.", true);
        return;
    }
    const fs::path Frozen = fs::path(OwnDirectory) / (Tag + "_review_" + Id + "_src.png");
    std::error_code ErrorCode;
    fs::copy_file(Live.Path, Frozen, fs::copy_options::overwrite_existing, ErrorCode);
    if (ErrorCode.value() != 0)
    {
        ShowToast("Could not save the picture: " + ErrorCode.message(), true);
        return;
    }
    BlenderFrozenPath_ = Frozen.string();
    BlenderFrozenId_ = Id;
    BlenderFrozenStatePath_.clear();
    BlenderFrozenBlendPath_.clear();
    StartBlenderMarking(Kind);
}

// With the v3 script Blender takes the picture itself, together with the camera, scene facts and an optional .blend copy, so all of them
// describe the same moment; older scripts fall back to copying the latest live picture.
void App::BeginBlenderMarking(const Session& Target, const BlenderMarkup::Tool Kind)
{
    if (BlenderPendingId_.empty() == false)
    {
        return;
    }
    const std::string OwnDirectory = BlenderOwnDirectory(Target);
    const std::string Tag = ScreenshotFinder::SessionTag(Target.Rec.Id);
    const std::chrono::system_clock::time_point Moment = std::chrono::system_clock::now();
    const std::time_t Current = std::chrono::system_clock::to_time_t(Moment);
    const long long Millis = std::chrono::duration_cast<std::chrono::milliseconds>(Moment.time_since_epoch()).count() % 1000;
    char Stamp[32];
    std::tm Local{};
#ifdef _WIN32
    localtime_s(&Local, &Current);
#else
    localtime_r(&Current, &Local);
#endif
    std::strftime(Stamp, sizeof Stamp, "%H%M%S", &Local);
    const std::string Id = std::string(Stamp) + "_" + std::to_string(Millis);

    const bool CanSnapshot = BlenderView_.ScriptVersion(OwnDirectory, Tag, Now_) >= BlenderLive::SCRIPT_VERSION;
    if (CanSnapshot == false)
    {
        FreezeBlenderFromLive(Target, Kind, Id);
        return;
    }
    nlohmann::json Request;
    Request["Id"] = Id;
    Request["SaveBlend"] = BlenderSaveBlend_;
    std::error_code ErrorCode;
    fs::create_directories(OwnDirectory, ErrorCode);
    const fs::path Final = fs::path(OwnDirectory) / (Tag + "_snap_request.json");
    fs::path Pending = Final;
    Pending += ".tmp";
    {
        std::ofstream Stream(Pending, std::ios::binary | std::ios::trunc);
        Stream << Request.dump();
    }
    fs::rename(Pending, Final, ErrorCode);
    if (ErrorCode.value() != 0)
    {
        FreezeBlenderFromLive(Target, Kind, Id);
        return;
    }
    BlenderTouchAt_ = 0;
    BlenderPendingId_ = Id;
    BlenderPendingAt_ = Now_;
    BlenderPendingTool_ = Kind;
}

void App::PollBlenderCapture(const Session& Target)
{
    if (BlenderPendingId_.empty() == true)
    {
        return;
    }
    const std::string OwnDirectory = BlenderOwnDirectory(Target);
    const std::string Tag = ScreenshotFinder::SessionTag(Target.Rec.Id);
    const std::string Id = BlenderPendingId_;
    const std::string StatePath = (fs::path(OwnDirectory) / (Tag + "_snap_" + Id + ".json")).string();
    std::error_code ErrorCode;
    if (fs::exists(StatePath, ErrorCode) == false)
    {
        if (Now_ - BlenderPendingAt_ > BlenderLive::SNAPSHOT_TIMEOUT_SECONDS)
        {
            BlenderPendingId_.clear();
            ShowToast("Blender did not answer in time; using the latest live picture without scene details.", true);
            FreezeBlenderFromLive(Target, BlenderPendingTool_, Id);
        }
        return;
    }
    BlenderPendingId_.clear();
    BlenderSnapshotState State;
    if (State.Load(StatePath) == false || State.Image().empty() == true || fs::exists(State.Image(), ErrorCode) == false)
    {
        FreezeBlenderFromLive(Target, BlenderPendingTool_, Id);
        return;
    }
    BlenderFrozenPath_ = State.Image();
    BlenderFrozenId_ = Id;
    BlenderFrozenStatePath_ = StatePath;
    BlenderFrozenBlendPath_ = State.BlendCopy();
    StartBlenderMarking(BlenderPendingTool_);
}

void App::FinishBlenderSnapshot()
{
    if (BlenderFrozenPath_.empty() == true || BlenderMarkup_.HasMarks() == false)
    {
        ShowToast("Mark something on the picture first.", true);
        return;
    }
    const fs::path Folder = fs::path(BlenderFrozenPath_).parent_path();
    const std::string Tag = ScreenshotFinder::SessionTag(BlenderReviewId_);
    const std::string Base = Tag + "_review_" + BlenderFrozenId_;
    const std::string MarkedPath = (Folder / (Base + ".png")).string();
    const int FirstNumber = BlenderBatch_.NextNumber();
    std::string Error;
    if (BlenderMarkupExport::Save(BlenderFrozenPath_, BlenderMarkup_.Marks(), FirstNumber, MarkedPath, Error) == false)
    {
        ShowToast(Error, true);
        return;
    }
    std::vector<std::string> Extras;
    for (size_t Index = 0; Index < BlenderMarkup_.Marks().size(); Index++)
    {
        const BlenderMarkup::Mark& Item = BlenderMarkup_.Marks()[Index];
        const std::string Number = std::to_string(FirstNumber + static_cast<int>(Index));
        std::string Extra;
        const std::string CropPath = (Folder / (Base + "_mark" + Number + ".png")).string();
        std::string CropError;
        if (BlenderMarkupExport::SaveCrop(BlenderFrozenPath_, Item, FirstNumber + static_cast<int>(Index), CropPath, CropError) == true)
        {
            Extra += "   Zoomed crop of this mark: " + CropPath + "\n";
        }
        if (Item.ReferencePath.empty() == false)
        {
            const std::string ReferencePath = (Folder / (Base + "_ref" + Number + fs::path(Item.ReferencePath).extension().string())).string();
            std::error_code ErrorCode;
            fs::copy_file(Item.ReferencePath, ReferencePath, fs::copy_options::overwrite_existing, ErrorCode);
            if (ErrorCode.value() == 0)
            {
                Extra += "   Reference image (the user wants this part to look like it): " + ReferencePath + "\n";
            }
        }
        Extras.push_back(Extra);
    }
    BlenderReviewBatch::Snapshot Item;
    Item.MarkedPath = MarkedPath;
    Item.SourcePath = BlenderFrozenPath_;
    Item.Notes = BlenderMarkup_.Describe(Extras);
    Item.FirstNumber = FirstNumber;
    Item.Count = BlenderMarkup_.Count();
    BlenderSnapshotState State;
    const Session* const Mesh = FindSession(BlenderReviewId_);
    if (BlenderFrozenStatePath_.empty() == false && State.Load(BlenderFrozenStatePath_) == true)
    {
        Item.StatePath = BlenderFrozenStatePath_;
        Item.SceneSummary = State.Summary();
        Item.BlendPath = BlenderFrozenBlendPath_;
        if (Mesh != nullptr)
        {
            const std::string ShotPath = (Folder / (Base + "_after.png")).string();
            Item.RestoreLine = BlenderSnapshotState::RestoreLine(BlenderLive::ScriptPath(SessionProjectDir(*Mesh)).string(), BlenderFrozenStatePath_, ShotPath);
        }
    }
    BlenderBatch_.Add(Item);
    ShowToast("Saved snapshot " + std::to_string(BlenderBatch_.Snapshots().size()) + " with " + std::to_string(Item.Count) + " marks", false);
    BlenderFrozenPath_.clear();
    BlenderMarkup_.Clear();
}

void App::SendBlenderReview(Session& Target, const bool ToReviewer, const std::string& GeneratorNote)
{
    if (BlenderBatch_.Empty() == true)
    {
        ShowToast("Mark something and press Done first.", true);
        return;
    }
    Session* const Reviewer = ToReviewer == true ? EnsureBlenderReviewer(Target) : nullptr;
    if (ToReviewer == true && Reviewer == nullptr)
    {
        return;
    }
    if (Reviewer != nullptr && GeneratorNote.empty() == false)
    {
        const std::string GeneratorClosing = "Open each picture and judge the generated model it shows. These are automatic renders of a finished mesh, not marks to forward to anyone.";
        SendToBlenderReviewer(*Reviewer, BlenderBatch_.BuildPrompt("the generated 3D model", BlenderHistory_.Describe(), GeneratorClosing + GeneratorNote));
        ShowToast("Sent the review to " + Reviewer->Rec.Name, false);
    }
    else if (Reviewer != nullptr)
    {
        const std::string MeshPeer = PeerNames::For(Target.Rec.Name, Target.Rec.Id);
        const std::string Closing = "Open each marked picture, look at every numbered mark, and turn each number into one precise instruction for '" + MeshPeer +
                                    "' (the session that owns this model; tag " + ScreenshotFinder::SessionTag(Target.Rec.Id) +
                                    "). Send all of them in one SendMessage, keeping the user's numbers. When it reports back, check the new live "
                                    "picture and tell the user per number what is fixed and what is still wrong.";
        const std::string ClosingWithExtras = Closing + " Forward anything useful from the extra lines (scene facts, .blend copy, the line that restores the view, zoomed crops, "
                                                        "reference images) to '" + MeshPeer + "', since it cannot see this message.";
        SendToBlenderReviewer(*Reviewer, BlenderBatch_.BuildPrompt("the 3D model", BlenderHistory_.Describe(), ClosingWithExtras));
        ShowToast("Sent the review to " + Reviewer->Rec.Name, false);
    }
    else
    {
        const std::string Closing = "Open each marked picture (and the zoomed crops and reference images when listed), find each numbered area in the scene, and fix what "
                                    "its note asks for. Then take a new screenshot of the viewport to check, and tell me briefly what you changed for each number.";
        SendWhenSafe(Target, BlenderBatch_.BuildPrompt("your 3D model", BlenderHistory_.Describe(), Closing));
        ShowToast("Sent the review to " + Target.Rec.Name, false);
    }
    BlenderHistory_.Add(BlenderBatch_.Snapshots());
    BlenderBatch_.Clear();
    BlenderShownSnapshot_ = -1;
}

void App::HandleBlenderViewInput(const ImVec2 Min, const ImVec2 Max, const bool Enabled)
{
    ImGui::SetCursorScreenPos(Min);
    ImGui::InvisibleButton("##blenderView", ImVec2(Max.x - Min.x, Max.y - Min.y),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    if (Enabled == false)
    {
        return;
    }
    const ImGuiIO& Io = ImGui::GetIO();
    if (ImGui::IsItemHovered() == true || ImGui::IsItemActive() == true)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    if (ImGui::IsItemActive() == true)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) == true)
        {
            BlenderView_.Orbit(Io.MouseDelta.x, Io.MouseDelta.y);
        }
        else
        {
            BlenderView_.Pan(Io.MouseDelta.x, Io.MouseDelta.y);
        }
    }
    if (ImGui::IsItemHovered() == true && Io.MouseWheel != 0.0f)
    {
        BlenderView_.Zoom(Io.MouseWheel);
    }
}

void App::DrawBlenderLiveToolbar(Session& Target, const bool LiveFresh, const bool HasPicture)
{
    const std::string OwnDirectory = BlenderOwnDirectory(Target);
    const std::string Tag = ScreenshotFinder::SessionTag(Target.Rec.Id);
    if (ImGui::Button(LiveFresh == true ? "Reconnect Blender" : "Connect Blender") == true)
    {
        const std::string ProjectDir = SessionProjectDir(Target);
        BlenderLive::EnsureScript(ProjectDir);
        BlenderLive::TouchWanted(OwnDirectory, Tag);
        SendWhenSafe(Target, BlenderLive::ConnectPrompt(ProjectDir, OwnDirectory, Tag));
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Asks this session to start the live picture inside Blender");
    }
    const int ScriptVersion = BlenderView_.ScriptVersion(OwnDirectory, Tag, Now_);
    const bool CanSteer = ScriptVersion >= BlenderLive::STEERING_SCRIPT_VERSION;
    const bool CanSnapshot = ScriptVersion >= BlenderLive::SCRIPT_VERSION;
    ImGui::SameLine();
    ImGui::BeginDisabled(CanSteer == false || LiveFresh == false);
    if (ImGui::Button("Frame all") == true)
    {
        BlenderView_.FrameAll();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(CanSnapshot == false);
    ImGui::Checkbox("Save .blend copy", &BlenderSaveBlend_);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) == true)
    {
        ImGui::SetTooltip("Each snapshot also saves a copy of the scene so the session can compare with or go back to it.\nBig scenes can pause Blender for a moment.");
    }
    ImGui::SameLine();
    const ImVec4 DotColor = LiveFresh == true ? ImVec4(0.9f, 0.2f, 0.2f, 1.0f) : TextDisabledColorV4_;
    ImGui::TextColored(DotColor, "%s", LiveFresh == true ? "LIVE" : "not connected");
    if (LiveFresh == true)
    {
        ImGui::SameLine();
        if (CanSnapshot == true)
        {
            ImGui::TextDisabled("Drag to rotate, right-drag to pan, scroll to zoom");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.96f, 0.62f, 0.04f, 1.0f), CanSteer == true ? "Reconnect Blender to add scene details to snapshots" : "Press Reconnect Blender to enable rotating");
        }
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Highlight:");
    for (int Index = 0; Index < BlenderMarkup::TOOL_COUNT; Index++)
    {
        const BlenderMarkup::Tool Kind = static_cast<BlenderMarkup::Tool>(Index);
        ImGui::SameLine();
        ImGui::BeginDisabled(HasPicture == false);
        if (ImGui::Button(BlenderMarkup::ToolLabel(Kind)) == true)
        {
            BeginBlenderMarking(Target, Kind);
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Locks the view while you mark");
    ImGui::SameLine();
    DrawBlenderExistingShotPicker(Target, OwnDirectory);
}

void App::UseBlenderExistingShot(const Session& Target, const std::string& SourcePath)
{
    const std::string OwnDirectory = BlenderOwnDirectory(Target);
    const std::string Tag = ScreenshotFinder::SessionTag(Target.Rec.Id);
    const std::string Id = "shot" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() % 100000000);
    const fs::path Frozen = fs::path(OwnDirectory) / (Tag + "_review_" + Id + "_src.png");
    std::error_code ErrorCode;
    fs::copy_file(SourcePath, Frozen, fs::copy_options::overwrite_existing, ErrorCode);
    if (ErrorCode.value() != 0)
    {
        ShowToast("Could not use that screenshot: " + ErrorCode.message(), true);
        return;
    }
    BlenderFrozenPath_ = Frozen.string();
    BlenderFrozenId_ = Id;
    BlenderFrozenStatePath_.clear();
    BlenderFrozenBlendPath_.clear();
    StartBlenderMarking(BlenderMarkup::Tool::Rectangle);
}

void App::DrawBlenderExistingShotPicker(Session& Target, const std::string& OwnDirectory)
{
    if (ImGui::Button("Use a screenshot...") == true)
    {
        ImGui::OpenPopup("##blenderShotPicker");
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Mark up a screenshot the session already took, instead of the live view");
    }
    if (ImGui::BeginPopup("##blenderShotPicker") == false)
    {
        return;
    }
    std::vector<std::pair<fs::file_time_type, fs::path>> Shots;
    std::error_code ErrorCode;
    for (const fs::directory_entry& Item : fs::directory_iterator(OwnDirectory, ErrorCode))
    {
        const std::string Name = Item.path().filename().string();
        const std::string Extension = Item.path().extension().string();
        if (Extension != ".png" && Extension != ".jpg" && Extension != ".jpeg")
        {
            continue;
        }
        if (Name.find("_live") != std::string::npos || Name.find("_snap_") != std::string::npos || Name.find("_review_") != std::string::npos)
        {
            continue;
        }
        Shots.emplace_back(Item.last_write_time(ErrorCode), Item.path());
    }
    std::sort(Shots.begin(), Shots.end(), [](const auto& Left, const auto& Right) { return Left.first > Right.first; });
    if (Shots.empty() == true)
    {
        ImGui::TextDisabled("No screenshots from this session yet.");
    }
    const size_t MaxShown = 15;
    for (size_t Index = 0; Index < Shots.size() && Index < MaxShown; Index++)
    {
        const std::string Name = Shots[Index].second.filename().string();
        if (ImGui::Selectable(Name.c_str()) == true)
        {
            UseBlenderExistingShot(Target, Shots[Index].second.string());
        }
    }
    ImGui::EndPopup();
}

void App::DrawBlenderMarkToolbar()
{
    ImGui::BeginDisabled(BlenderMarkup_.HasMarks() == false);
    if (ImGui::Button("Done") == true)
    {
        FinishBlenderSnapshot();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) == true)
    {
        ImGui::SetTooltip("Save this picture with its marks and unlock the view");
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard") == true)
    {
        BlenderFrozenPath_.clear();
        BlenderMarkup_.Clear();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("View locked. Drag on the picture; each mark gets a number and asks what is wrong");
    BlenderMarkup_.DrawToolbar(UiScale_);
}

void App::DrawBlenderSnapshotStrip(Session& Target)
{
    const float Scale = UiScale_;
    const std::vector<BlenderReviewBatch::Snapshot>& Snapshots = BlenderBatch_.Snapshots();
    ImGui::BeginChild("##blenderSnapshots", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Snapshots (%d), round %d", static_cast<int>(Snapshots.size()), BlenderHistory_.CurrentRound());
    ImGui::SameLine();
    const bool HasReviewer = FindBlenderReviewer(Target) != nullptr;
    if (ImGui::Button(HasReviewer == true ? "Send to reviewer" : "Send to session") == true)
    {
        SendBlenderReview(Target, HasReviewer);
        ImGui::EndChild();
        return;
    }
    if (ImGui::IsItemHovered() == true && HasReviewer == true)
    {
        ImGui::SetTooltip("The Opus reviewer reads your marks, turns them into instructions and sends them to %s", Target.Rec.Name.c_str());
    }
    if (HasReviewer == true)
    {
        ImGui::SameLine();
        if (ImGui::Button("Send straight to model session") == true)
        {
            SendBlenderReview(Target, false);
            ImGui::EndChild();
            return;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear all") == true)
    {
        BlenderBatch_.Clear();
        BlenderShownSnapshot_ = -1;
        ImGui::EndChild();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Click one to view it, right-click to remove");

    const float ThumbHeight = std::max(30.0f * Scale, ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ScrollbarSize - ImGui::GetStyle().ItemSpacing.y);
    int RemoveIndex = -1;
    for (size_t Index = 0; Index < Snapshots.size(); Index++)
    {
        const BlenderReviewBatch::Snapshot& Item = Snapshots[Index];
        ImGui::PushID(static_cast<int>(Index));
        if (Index > 0)
        {
            ImGui::SameLine();
        }
        const ImageCache::Texture* const Thumb = Images_->Get(Item.MarkedPath, static_cast<std::time_t>(1));
        const bool HasThumb = Thumb != nullptr && Thumb->Failed == false && Thumb->Width > 0 && Thumb->Height > 0;
        const float ThumbWidth = HasThumb == true ? ThumbHeight * static_cast<float>(Thumb->Width) / static_cast<float>(Thumb->Height) : ThumbHeight * 1.5f;
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##thumb", ImVec2(ThumbWidth, ThumbHeight)) == true)
        {
            BlenderShownSnapshot_ = BlenderShownSnapshot_ == static_cast<int>(Index) ? -1 : static_cast<int>(Index);
        }
        ImDrawList* const DrawList = ImGui::GetWindowDrawList();
        const ImVec2 Corner(Origin.x + ThumbWidth, Origin.y + ThumbHeight);
        if (HasThumb == true)
        {
            DrawList->AddImage(Thumb->Id, Origin, Corner);
        }
        else
        {
            DrawList->AddRectFilled(Origin, Corner, IM_COL32(40, 40, 46, 255));
        }
        const bool Selected = BlenderShownSnapshot_ == static_cast<int>(Index);
        DrawList->AddRect(Origin, Corner, Selected == true ? AccentColor_ : IM_COL32(110, 114, 125, 255), 0.0f, 0, Selected == true ? 3.0f * Scale : 1.0f);
        char Label[32];
        if (Item.Count == 1)
        {
            std::snprintf(Label, sizeof Label, "%d", Item.FirstNumber);
        }
        else
        {
            std::snprintf(Label, sizeof Label, "%d-%d", Item.FirstNumber, Item.FirstNumber + Item.Count - 1);
        }
        const ImVec2 LabelSize = ImGui::CalcTextSize(Label);
        DrawList->AddRectFilled(Origin, ImVec2(Origin.x + LabelSize.x + 8.0f * Scale, Origin.y + LabelSize.y + 4.0f * Scale), IM_COL32(0, 0, 0, 190));
        DrawList->AddText(ImVec2(Origin.x + 4.0f * Scale, Origin.y + 2.0f * Scale), IM_COL32(255, 255, 255, 255), Label);
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("%s", Item.Notes.c_str());
        }
        if (ImGui::BeginPopupContextItem("##snapshotMenu") == true)
        {
            if (ImGui::MenuItem("Remove snapshot") == true)
            {
                RemoveIndex = static_cast<int>(Index);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (RemoveIndex >= 0)
    {
        BlenderBatch_.Remove(static_cast<size_t>(RemoveIndex));
        BlenderShownSnapshot_ = -1;
    }
    ImGui::EndChild();
}

void App::DrawBlenderReviewLeft(Session& Target, const float Width, const float Height)
{
    const float Scale = UiScale_;
    const std::string OwnDirectory = BlenderOwnDirectory(Target);
    const std::string Tag = ScreenshotFinder::SessionTag(Target.Rec.Id);
    const bool Marking = BlenderFrozenPath_.empty() == false;
    const bool ShowingSnapshot = Marking == false && BlenderShownSnapshot_ >= 0 && BlenderShownSnapshot_ < static_cast<int>(BlenderBatch_.Snapshots().size());
    const LiveView::Frame Live = BlenderLive::Latest(OwnDirectory, Tag);
    const bool LiveFresh = Live.Path.empty() == false && Live.AgeSeconds < BlenderLive::STALE_FRAME_SECONDS;
    const bool CanSteer = BlenderView_.ScriptVersion(OwnDirectory, Tag, Now_) >= BlenderLive::STEERING_SCRIPT_VERSION;

    BlenderView_.Flush(OwnDirectory, Tag, Now_);
    PollBlenderCapture(Target);
    const bool Capturing = BlenderPendingId_.empty() == false;

    ImGui::BeginChild("##blenderLeft", ImVec2(Width, Height), ImGuiChildFlags_None);

    if (Marking == true)
    {
        DrawBlenderMarkToolbar();
    }
    else if (ShowingSnapshot == true)
    {
        if (ImGui::Button("Back to live") == true)
        {
            BlenderShownSnapshot_ = -1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Showing a saved snapshot");
    }
    else
    {
        DrawBlenderLiveToolbar(Target, LiveFresh, Live.Path.empty() == false && Capturing == false);
    }

    const float BottomHeight = Marking == true ? std::min(170.0f * Scale, Height * 0.3f) : BlenderBatch_.Empty() == true ? 0.0f : std::min(150.0f * Scale, Height * 0.28f);
    const ImVec2 Area = ImGui::GetContentRegionAvail();
    const float PictureHeight = std::max(60.0f * Scale, Area.y - BottomHeight - (BottomHeight > 0.0f ? ImGui::GetStyle().ItemSpacing.y : 0.0f));
    const bool LiveShown = Marking == false && ShowingSnapshot == false;
    std::string PicturePath = Live.Path;
    long long PictureKey = Live.Key;
    if (Marking == true)
    {
        PicturePath = BlenderFrozenPath_;
        PictureKey = 1;
    }
    else if (ShowingSnapshot == true)
    {
        PicturePath = BlenderBatch_.Snapshots()[static_cast<size_t>(BlenderShownSnapshot_)].MarkedPath;
        PictureKey = 1;
    }

    ImGui::BeginChild("##blenderPicture", ImVec2(Area.x, PictureHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImageCache::Texture* const Image = PicturePath.empty() == false ? Images_->Get(PicturePath, static_cast<std::time_t>(PictureKey)) : nullptr;
    const ImVec2 Inner = ImGui::GetContentRegionAvail();
    if (Image != nullptr && Image->Failed == false && Image->Width > 0 && Image->Height > 0)
    {
        const float Aspect = static_cast<float>(Image->Height) / static_cast<float>(Image->Width);
        float DrawWidth = Inner.x;
        float DrawHeight = DrawWidth * Aspect;
        if (DrawHeight > Inner.y)
        {
            DrawHeight = Inner.y;
            DrawWidth = DrawHeight / Aspect;
        }
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const ImVec2 Min(Origin.x + (Inner.x - DrawWidth) * 0.5f, Origin.y + (Inner.y - DrawHeight) * 0.5f);
        const ImVec2 Max(Min.x + DrawWidth, Min.y + DrawHeight);
        ImGui::GetWindowDrawList()->AddImage(Image->Id, Min, Max);
        if (Marking == true)
        {
            BlenderMarkup_.DrawCanvas(Min, Max, Scale);
        }
        else if (LiveShown == true)
        {
            HandleBlenderViewInput(Min, Max, CanSteer == true && LiveFresh == true && Capturing == false);
            if (Capturing == true)
            {
                ImGui::GetWindowDrawList()->AddText(ImVec2(Min.x + 10.0f * Scale, Min.y + 8.0f * Scale), IM_COL32(255, 255, 255, 230), "Capturing the view...");
            }
        }
    }
    else
    {
        const char* const Message = PicturePath.empty() == true ? "No live picture yet.\nPress Connect Blender, and make sure Blender is open." : "Loading...";
        ImGui::TextDisabled("%s", Message);
    }
    ImGui::EndChild();

    if (Marking == true)
    {
        ImGui::BeginChild("##blenderNotes", ImVec2(0, 0), ImGuiChildFlags_Borders);
        if (BlenderMarkup_.HasMarks() == false)
        {
            ImGui::TextDisabled("No marks yet. Pick Draw, Rectangle or Circle and drag on the picture; each mark gets a number and a note.");
        }
        BlenderMarkup_.DrawNotes(Scale);
        ImGui::EndChild();
    }
    else if (BlenderBatch_.Empty() == false)
    {
        DrawBlenderSnapshotStrip(Target);
    }
    ImGui::EndChild();
}
