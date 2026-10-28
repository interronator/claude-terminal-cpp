#include "App.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "PeerAnchors.h"
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
    PeerLinks_.Note(CurrentSession.Rec.Id, Messages, LocalByClaudeId);
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

// Drawn in a child window over the panes that takes no mouse input, so the veins sit on top of the pane borders
// without getting in the way of clicks.
void App::DrawPeerOverlay(const ImVec2 PanesMin, const ImVec2 PanesMax, const float Gap)
{
    const double Wall = PeerLinks::WallNow();
    std::vector<PeerLinks::View> Links = PeerLinks_.Visible(Wall);
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

    struct Planned
    {
        const PeerLinks::View* Link = nullptr;
        PeerVeins::Box From{};
        PeerVeins::Box To{};
        float FromInflate = 0;
        float ToInflate = 0;
        bool OffScreen = false;
    };
    std::vector<Planned> Plans;
    PeerAnchors Anchors;

    for (PeerLinks::View& Link : Links)
    {
        // A sender that went back to work isn't waiting on anyone, whatever the message history says.
        Link.Waiting = Link.Waiting == true && IsPeerBusy(Link.FromId) == false;
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

        // A peer without a pane leads to its toolbar chip, or, when it has none either, off the top of the pane area.
        const PeerVeins::Box* const Shown = FromPane != nullptr ? FromPane : ToPane;
        const std::string& MissingId = FromPane != nullptr ? Link.ToId : Link.FromId;
        PeerVeins::Box Missing{};
        float MissingInflate = 2 * Scale;
        Planned Plan;
        Plan.Link = &Link;
        if (FromPane == nullptr || ToPane == nullptr)
        {
            const PeerVeins::Box* const Chip = PeerVeins_.FindChip(MissingId);
            if (Chip != nullptr)
            {
                Missing = *Chip;
            }
            else
            {
                const float CenterX = (Shown->Min.x + Shown->Max.x) * 0.5f;
                Missing = {ImVec2(CenterX - 1, PanesMin.y - 80 * Scale), ImVec2(CenterX + 1, PanesMin.y - 60 * Scale)};
                MissingInflate = 0;
                Plan.OffScreen = true;
            }
        }
        Plan.From = FromPane != nullptr ? *FromPane : Missing;
        Plan.To = ToPane != nullptr ? *ToPane : Missing;
        Plan.FromInflate = FromPane != nullptr ? Gap * 0.5f : MissingInflate;
        Plan.ToInflate = ToPane != nullptr ? Gap * 0.5f : MissingInflate;
        Anchors.AddLink(Link.FromId, Link.ToId);
        Plans.push_back(Plan);
    }

    for (const Planned& Plan : Plans)
    {
        const PeerLinks::View& Link = *Plan.Link;
        const std::vector<ImVec2> Route = Anchors.Pick(Link.FromId, Plan.From, Plan.FromInflate, Link.ToId, Plan.To, Plan.ToInflate, 24 * Scale);
        PeerVeins::Flow Current;
        Current.Color = UiHelpers::SessionTint(Link.FromId);
        Current.Strength = Link.Strength;
        Current.Packets = Link.Packets;
        Current.Waiting = Link.Waiting;
        if (Plan.OffScreen == true)
        {
            DrawList->PushClipRect(PanesMin, PanesMax, true);
        }
        PeerVeins_.Draw(DrawList, Route, Current, Now_, Scale);
        if (Plan.OffScreen == true)
        {
            DrawList->PopClipRect();
        }
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
