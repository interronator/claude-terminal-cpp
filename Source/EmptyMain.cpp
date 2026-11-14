#include "App.h"

void App::DrawEmptyMain(const ImVec2& Avail)
{
    const float Scale = UiScale_;
    const float TileSize = 96.0f * Scale;
    const float LabelGap = 10.0f * Scale;
    const float HintGap = 18.0f * Scale;
    const char* const Label = "Start General Session";
    const char* const Hint = State_.Projects.empty() == true ? "Or add a project folder, then start a Claude session in it."
                                                             : "Or pick a session in the sidebar, or press + next to a project to start one.";
    const ImVec2 LabelSize = ImGui::CalcTextSize(Label);
    const ImVec2 HintSize = ImGui::CalcTextSize(Hint);
    const float BlockHeight = TileSize + LabelGap + LabelSize.y + HintGap + HintSize.y;
    const float ButtonWidth = std::max(TileSize, LabelSize.x);

    const ImVec2 Start = ImGui::GetCursorPos();
    const float Top = Start.y + std::max(0.0f, (Avail.y - BlockHeight) * 0.45f);
    ImGui::SetCursorPos(ImVec2(Start.x + (Avail.x - ButtonWidth) * 0.5f, Top));
    const ImVec2 ButtonMin = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##startgeneral", ImVec2(ButtonWidth, TileSize + LabelGap + LabelSize.y)) == true)
    {
        StartGeneralSession();
    }
    const bool Hovered = ImGui::IsItemHovered();
    const bool Held = ImGui::IsItemActive();
    if (Hovered == true)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Start Claude without picking a project. It works in %s.", GeneralFolderPath().c_str());
    }

    ImDrawList* const Draw = ImGui::GetWindowDrawList();
    const ImVec2 TileMin(ButtonMin.x + (ButtonWidth - TileSize) * 0.5f, ButtonMin.y);
    const ImVec2 TileMax(TileMin.x + TileSize, TileMin.y + TileSize);
    const float Rounding = 14.0f * Scale;
    Draw->AddRectFilled(TileMin, TileMax, Hovered == true ? SurfaceColorU32_ : PaneBgColorU32_, Rounding);
    Draw->AddRect(TileMin, TileMax, Hovered == true ? AccentColor_ : PaneBorderColorU32_, Rounding, 0, 1.5f * Scale);

    const ImVec2 Center((TileMin.x + TileMax.x) * 0.5f, (TileMin.y + TileMax.y) * 0.5f);
    const float Arm = TileSize * (Held == true ? 0.24f : 0.28f);
    const float Thickness = 6.0f * Scale;
    const ImU32 PlusColor = Hovered == true ? AccentColor_ : TextDisabledColorU32_;
    Draw->AddLine(ImVec2(Center.x - Arm, Center.y), ImVec2(Center.x + Arm, Center.y), PlusColor, Thickness);
    Draw->AddLine(ImVec2(Center.x, Center.y - Arm), ImVec2(Center.x, Center.y + Arm), PlusColor, Thickness);

    const ImVec2 LabelPos(ButtonMin.x + (ButtonWidth - LabelSize.x) * 0.5f, TileMax.y + LabelGap);
    Draw->AddText(LabelPos, Hovered == true ? AccentColor_ : TextColorU32_, Label);

    ImGui::SetCursorPos(ImVec2(Start.x + (Avail.x - HintSize.x) * 0.5f, Top + TileSize + LabelGap + LabelSize.y + HintGap));
    ImGui::TextDisabled("%s", Hint);
}
