#include "App.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "UiHelpers.h"

void App::NotePeerMessages(const Session& CurrentSession, TranscriptScanner& Scanner)
{
    // Called even with no new messages so the tracker can refresh the busy state of sessions outside the app.
    const std::vector<TranscriptScanner::PeerMessage> Messages = Scanner.TakePeerMessages();
    std::map<std::string, std::string> LocalByClaudeId;
    for (const std::unique_ptr<Session>& Candidate : Sessions_)
    {
        if (Messages.empty() == false && Candidate->Rec.ClaudeSessionId.empty() == false)
        {
            LocalByClaudeId[Candidate->Rec.ClaudeSessionId] = Candidate->Rec.Id;
        }
    }
    PeerLinks_.Note(CurrentSession.Rec.Id, Messages, LocalByClaudeId, Scanner.LastUserPromptAt());
}

std::string App::PeerDisplayName(const std::string& Id) const
{
    const Session* const Local = FindSession(Id);
    return Local != nullptr ? Local->Rec.Name : PeerLinks::ExternalName(Id);
}

bool App::IsPeerBusy(const std::string& Id) const
{
    const Session* const Local = FindSession(Id);
    if (Local == nullptr)
    {
        return PeerLinks_.IsExternalBusy(Id);
    }
    return Local->CurrentStatus == Status::Busy || Local->CurrentStatus == Status::Starting;
}

// A receiver that worked on a message and settled has dealt with it, even when it answered in its own window
// instead of sending a message back, so the sender is no longer shown as waiting.
bool App::ReplyHandled(const PeerLinks::View& Link)
{
    constexpr double TRACK_EXPIRE_SECONDS = 6.0 * 3600.0;
    const double Wall = PeerLinks::WallNow();
    for (std::map<std::string, ReplyTrack>::iterator Entry = ReplyTracks_.begin(); Entry != ReplyTracks_.end();)
    {
        if (Wall - Entry->second.LastSeenAt > TRACK_EXPIRE_SECONDS)
        {
            Entry = ReplyTracks_.erase(Entry);
            continue;
        }
        ++Entry;
    }

    ReplyTrack& Track = ReplyTracks_[Link.FromId + "|" + Link.ToId];
    Track.LastSeenAt = Wall;
    const bool Busy = IsPeerBusy(Link.ToId);
    if (Track.MessageAt != Link.LastSentAt)
    {
        Track.MessageAt = Link.LastSentAt;
        Track.ReceiverWorked = false;
        Track.ReceiverSettled = Busy == false;
    }
    if (Busy == true)
    {
        // Work already under way when the message arrived is unrelated until the receiver has settled and started again.
        Track.ReceiverWorked = Track.ReceiverWorked == true || Track.ReceiverSettled == true;
        return false;
    }
    Track.ReceiverSettled = true;
    return Track.ReceiverWorked;
}

// Drawn in a child window over the panes that takes no mouse input, so the veins sit on top of the panes
// without getting in the way of clicks.
void App::DrawPeerOverlay(const ImVec2 PanesMin, const ImVec2 PanesMax, const float Gap)
{
    const double Wall = PeerLinks::WallNow();
    std::vector<PeerLinks::View> Links = PeerLinks_.Visible(Wall);
    const bool HasMentionHover = MentionHoverFromId_.empty() == false && MentionHoverToId_.empty() == false;
    if (HasMentionHover == true)
    {
        PeerLinks::View Hover;
        Hover.FromId = MentionHoverFromId_;
        Hover.ToId = MentionHoverToId_;
        Hover.Strength = 1.0f;
        Links.push_back(Hover);
    }
    const std::string HoverToId = MentionHoverToId_;
    MentionHoverFromId_.clear();
    MentionHoverToId_.clear();
    if (Links.empty() == true)
    {
        return;
    }
    const float Scale = UiScale_;
    const ImVec2 OverlayPos = ImGui::GetWindowPos();
    ImGui::SetCursorScreenPos(OverlayPos);
    ImGui::BeginChild("##peerVeins", ImGui::GetWindowSize(), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    std::map<std::string, std::vector<std::pair<std::string, ImU32>>> Pills;  // pane session id -> labels

    const float TopY = PeerVeins_.TopOfPanes(PanesMin.y) - Gap * 0.5f;
    for (PeerLinks::View& Link : Links)
    {
        // A sender that went back to work isn't waiting on anyone, whatever the message history says.
        Link.Waiting = Link.Waiting == true && IsPeerBusy(Link.FromId) == false;
        Link.Waiting = Link.Waiting == true && ReplyHandled(Link) == false;
        if (Link.Strength <= 0.0f && Link.Packets.empty() == true && Link.Waiting == false)
        {
            continue;
        }
        const PeerVeins::Box* const FromPane = PeerVeins_.FindPane(Link.FromId);
        const PeerVeins::Box* const ToPane = PeerVeins_.FindPane(Link.ToId);
        if (FromPane == nullptr && ToPane == nullptr)
        {
            continue;
        }
        bool ToSidebar = false;
        if (Link.Waiting == true)
        {
            const ImU32 Tint = UiHelpers::SessionTint(Link.FromId);
            if (FromPane != nullptr)
            {
                Pills[Link.FromId].push_back({"waiting on " + PeerDisplayName(Link.ToId), Tint});
            }
            if (ToPane != nullptr)
            {
                Pills[Link.ToId].push_back({PeerDisplayName(Link.FromId) + " is waiting for a reply", Tint});
            }
        }

        std::vector<ImVec2> Route;
        bool OffScreen = false;
        if (FromPane != nullptr && ToPane != nullptr)
        {
            // Both directions of a pair share one curve, reversed; the lane offset in the vein keeps them apart.
            const bool Swapped = Link.ToId < Link.FromId;
            Route = PeerVeins::Curve(PeerVeins::TopMiddle(Swapped == true ? *ToPane : *FromPane), PeerVeins::TopMiddle(Swapped == true ? *FromPane : *ToPane), false,
                                     PanesMin.y, Scale);
            if (Swapped == true)
            {
                std::reverse(Route.begin(), Route.end());
            }
        }
        else
        {
            // A peer without a pane leads to its toolbar chip, or, when it has none either, off the top of the pane area.
            const PeerVeins::Box& Shown = FromPane != nullptr ? *FromPane : *ToPane;
            const std::string& HiddenId = FromPane != nullptr ? Link.ToId : Link.FromId;
            const PeerVeins::Box* const Row = PeerVeins_.FindRow(HiddenId);
            const PeerVeins::Box* const Chip = PeerVeins_.FindChip(HiddenId);
            const ImVec2 Start = PeerVeins::TopMiddle(Shown);
            ImVec2 End(Start.x, PanesMin.y - 70 * Scale);
            if (Row != nullptr)
            {
                End = ImVec2(Row->Max.x, (Row->Min.y + Row->Max.y) * 0.5f);
                ToSidebar = true;
            }
            else if (Chip != nullptr)
            {
                End = ImVec2((Chip->Min.x + Chip->Max.x) * 0.5f, Chip->Max.y + 2 * Scale);
            }
            else
            {
                OffScreen = true;
            }
            Route = ToSidebar == true ? PeerVeins::SideCurve(Start, End, Scale) : PeerVeins::Curve(Start, End, true, std::numeric_limits<float>::lowest(), Scale);
            if (FromPane == nullptr)
            {
                std::reverse(Route.begin(), Route.end());
            }
        }

        PeerVeins::Flow Current;
        Current.Color = UiHelpers::SessionTint(Link.FromId);
        Current.Strength = Link.Strength;
        Current.Packets = Link.Packets;
        Current.Waiting = Link.Waiting;
        if (OffScreen == true)
        {
            DrawList->PushClipRect(ImVec2(PanesMin.x, TopY - Gap), PanesMax, true);
        }
        if (ToSidebar == true)
        {
            DrawList->PushClipRectFullScreen();
        }
        PeerVeins_.Draw(DrawList, Route, Current, Now_, Scale);
        if (ToSidebar == true)
        {
            DrawList->PopClipRect();
        }
        if (OffScreen == true)
        {
            DrawList->PopClipRect();
        }
    }

    const PeerVeins::Box* const HoverPane = HasMentionHover == true ? PeerVeins_.FindPane(HoverToId) : nullptr;
    if (HoverPane != nullptr)
    {
        const float Pulse = 0.5f + 0.5f * static_cast<float>(std::sin(Now_ * 5.0));
        DrawList->AddRect(HoverPane->Min, HoverPane->Max, UiHelpers::WithAlpha(UiHelpers::SessionTint(HoverToId), static_cast<int>(110 + 130 * Pulse)), 6 * Scale, 0, 2.5f * Scale);
    }
    for (const std::pair<const std::string, std::vector<std::pair<std::string, ImU32>>>& Pane : Pills)
    {
        DrawWaitingPills(DrawList, *PeerVeins_.FindPane(Pane.first), Pane.second);
    }
    ImGui::EndChild();
}

// Small labels stacked up from the bottom-right corner of a pane, with dots that tick while the wait goes on.
void App::DrawWaitingPills(ImDrawList* const DrawList, const PeerVeins::Box& Pane, const std::vector<std::pair<std::string, ImU32>>& Labels)
{
    const float Scale = UiScale_;
    const float PadX = 8 * Scale;
    const float PadY = 3 * Scale;
    const float DotRadius = 3.5f * Scale;
    const float Margin = 10 * Scale;
    const int Dots = static_cast<int>(Now_ * 2.0) % 4;
    const std::string Trail = std::string(static_cast<size_t>(Dots), '.') + std::string(static_cast<size_t>(3 - Dots), ' ');
    float Bottom = Pane.Max.y - Margin;
    for (const std::pair<std::string, ImU32>& Label : Labels)
    {
        const std::string Text = TruncateToWidth(Label.first, (Pane.Max.x - Pane.Min.x) * 0.6f) + Trail;
        const ImVec2 TextSize = ImGui::CalcTextSize(Text.c_str());
        const float Width = PadX * 2 + DotRadius * 2 + 6 * Scale + TextSize.x;
        const float Height = PadY * 2 + TextSize.y;
        const ImVec2 Min(Pane.Max.x - Margin - Width, Bottom - Height);
        const ImVec2 Max(Pane.Max.x - Margin, Bottom);
        if (Min.x < Pane.Min.x + Margin || Min.y < Pane.Min.y + Margin)
        {
            break;
        }
        DrawList->AddRectFilled(Min, Max, IM_COL32(12, 14, 18, 230), Height * 0.5f);
        DrawList->AddRect(Min, Max, UiHelpers::WithAlpha(Label.second, 200), Height * 0.5f, 0, 1.2f * Scale);
        const float Breath = 0.5f + 0.5f * static_cast<float>(std::sin(Now_ * 2.0));
        DrawList->AddCircleFilled(ImVec2(Min.x + PadX + DotRadius, (Min.y + Max.y) * 0.5f), DotRadius, UiHelpers::WithAlpha(Label.second, static_cast<int>(120 + 135 * Breath)));
        DrawList->AddText(ImVec2(Min.x + PadX + DotRadius * 2 + 6 * Scale, Min.y + PadY), TextColorU32_, Text.c_str());
        Bottom = Min.y - 4 * Scale;
    }
}
