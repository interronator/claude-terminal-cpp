#include "App.h"

#include <algorithm>

#include "ScreenshotFeed.h"
#include "UiHelpers.h"

void App::PollChildProgress()
{
    for (const std::string& Tag : ChildProgress_.Poll())
    {
        const Session* const Child = FindSessionByTag(Tag);
        const ChildProgress::Entry* const Report = ChildProgress_.Find(Tag);
        if (Child == nullptr || Report == nullptr || Child->Rec.ParentId.empty() == true)
        {
            continue;
        }
        const Session* const Parent = FindSession(Child->Rec.ParentId);
        if (Parent == nullptr)
        {
            continue;
        }
        ShowToast(Child->Rec.Name + " (for " + Parent->Rec.Name + "): " + std::to_string(Report->Percent) + "%" +
                  (Report->Note.empty() == true ? "" : " - " + Report->Note));
    }
}

float App::DrawChildBars(const Session& CurrentSession, const ImVec2 PaneTopLeft, const ImVec2 PaneSize, const float HeaderHeight, const float TopY)
{
    std::vector<const Session*> Children;
    for (const std::unique_ptr<Session>& Candidate : Sessions_)
    {
        if (Candidate->Rec.ParentId == CurrentSession.Rec.Id && Candidate->Rec.Id != CurrentSession.Rec.Id)
        {
            Children.push_back(Candidate.get());
        }
    }
    if (Children.empty() == true)
    {
        return TopY;
    }
    const float Scale = UiScale_;
    const float Margin = 8 * Scale;
    const float Pad = 6 * Scale;
    const float Width = std::min(PaneSize.x * 0.4f, 320 * Scale);
    if (Width < 180 * Scale)
    {
        return TopY;
    }
    const float RowHeight = ImGui::GetTextLineHeight() + 6 * Scale;
    const float PanelHeight = RowHeight * static_cast<float>(Children.size()) + Pad * 2;
    const float MaxHeight = (PaneTopLeft.y + PaneSize.y) - TopY - Margin * 2;
    if (PanelHeight > MaxHeight)
    {
        return TopY;
    }
    const ImVec2 Min(PaneTopLeft.x + PaneSize.x - Width - Margin, TopY + Margin);
    const ImVec2 Max(Min.x + Width, Min.y + PanelHeight);
    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    DrawList->AddRectFilled(Min, Max, UiHelpers::WithAlpha(SurfaceColorU32_, 225), 6 * Scale);
    DrawList->AddRect(Min, Max, PaneBorderColorU32_, 6 * Scale);

    const std::string ParentTag = ScreenshotFinder::SessionTag(CurrentSession.Rec.Id);
    const float NameWidth = Width * 0.38f;
    const float BarHeight = 6 * Scale;
    float RowTop = Min.y + Pad;
    for (const Session* const Child : Children)
    {
        const std::string Tag = ScreenshotFinder::SessionTag(Child->Rec.Id);
        const ChildProgress::Entry* const Report = ChildProgress_.Find(Tag);
        const int Percent = Report != nullptr ? Report->Percent : -1;
        const float RowMiddle = RowTop + RowHeight * 0.5f;

        DrawStatusDot(DrawList, ImVec2(Min.x + Pad + 4 * Scale, RowMiddle), 3.5f * Scale, *Child);
        const float NameX = Min.x + Pad + 14 * Scale;
        DrawList->PushClipRect(ImVec2(NameX, RowTop), ImVec2(NameX + NameWidth - 14 * Scale, RowTop + RowHeight), true);
        DrawList->AddText(ImVec2(NameX, RowMiddle - ImGui::GetTextLineHeight() * 0.5f), TextColorU32_, Child->Rec.Name.c_str());
        DrawList->PopClipRect();

        const std::string Label = Percent >= 0 ? std::to_string(Percent) + "%" : std::string("--");
        const float LabelWidth = ImGui::CalcTextSize("100%").x;
        const float BarLeft = Min.x + Pad + NameWidth;
        const float BarRight = Max.x - Pad - LabelWidth - 6 * Scale;
        const float BarTop = RowMiddle - BarHeight * 0.5f;
        DrawList->AddRectFilled(ImVec2(BarLeft, BarTop), ImVec2(BarRight, BarTop + BarHeight), PaneBorderColorU32_, BarHeight * 0.5f);
        if (Percent > 0)
        {
            const float FillRight = BarLeft + std::max(BarHeight, (BarRight - BarLeft) * static_cast<float>(Percent) / 100.0f);
            const ImU32 FillColor = Percent >= 100 ? IM_COL32(80, 200, 110, 255) : AccentColor_;
            DrawList->AddRectFilled(ImVec2(BarLeft, BarTop), ImVec2(FillRight, BarTop + BarHeight), FillColor, BarHeight * 0.5f);
        }
        DrawList->AddText(ImVec2(Max.x - Pad - LabelWidth, RowMiddle - ImGui::GetTextLineHeight() * 0.5f), TextColorU32_, Label.c_str());

        ImGui::SetCursorScreenPos(ImVec2(Min.x, RowTop));
        ImGui::PushID(Child->Rec.Id.c_str());
        const bool Clicked = ImGui::InvisibleButton("##childRow", ImVec2(Width, RowHeight));
        ImGui::PopID();
        if (ImGui::IsItemHovered() == true)
        {
            const std::string Note = Report != nullptr && Report->Note.empty() == false ? Report->Note : std::string("No progress reported yet");
            ImGui::SetTooltip("%s\nClick to open %s\nCtrl+click to ask for a progress update", Note.c_str(), Child->Rec.Name.c_str());
        }
        if (Clicked == true)
        {
            const bool CtrlHeld = ImGui::GetIO().KeyCtrl;
            if (CtrlHeld == false)
            {
                OpenSessionFromSidebar(Child->Rec.Id);
                break;
            }
            Session* const Target = FindSession(Child->Rec.Id);
            if (Target != nullptr && Target->IsRunning() == true)
            {
                SendWhenSafe(*Target, ChildProgress::CheckInPrompt(ParentTag));
                ShowToast("Asked " + Target->Rec.Name + " for a progress update.");
            }
        }
        RowTop += RowHeight;
    }
    return Max.y;
}
