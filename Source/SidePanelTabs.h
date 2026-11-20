#pragma once

#include <string>
#include <vector>

#include "imgui.h"

// A row of text tabs with an accent underline on the active one, for the side panels.
class SidePanelTabs
{
public:
    struct Tab
    {
        std::string Id;
        std::string Label;
        int Badge = 0;
    };

    // Draws the tabs on the current line and returns the id of the tab clicked this frame, or an empty string.
    static std::string Draw(const std::vector<Tab>& Tabs, const std::string& ActiveId, const ImU32 AccentColor, const float Scale)
    {
        std::string Clicked;
        for (size_t Index = 0; Index < Tabs.size(); Index++)
        {
            const Tab& Current = Tabs[Index];
            if (Index > 0)
            {
                ImGui::SameLine(0, 4 * Scale);
            }
            const std::string Text = Current.Badge > 0 ? Current.Label + "  " + std::to_string(Current.Badge) : Current.Label;
            const ImVec2 TextSize = ImGui::CalcTextSize(Text.c_str());
            const ImVec2 Size(TextSize.x + 16 * Scale, ImGui::GetFrameHeight());
            const ImVec2 Min = ImGui::GetCursorScreenPos();
            ImGui::PushID(Current.Id.c_str());
            if (ImGui::InvisibleButton("##tab", Size) == true)
            {
                Clicked = Current.Id;
            }
            ImGui::PopID();
            const bool Active = Current.Id == ActiveId;
            const bool Hovered = ImGui::IsItemHovered();
            ImDrawList* const DrawList = ImGui::GetWindowDrawList();
            const ImU32 TextColor = Active == true || Hovered == true ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
            DrawList->AddText(ImVec2(Min.x + 8 * Scale, Min.y + (Size.y - TextSize.y) * 0.5f), TextColor, Text.c_str());
            if (Active == true)
            {
                DrawList->AddRectFilled(ImVec2(Min.x, Min.y + Size.y - 2 * Scale), ImVec2(Min.x + Size.x, Min.y + Size.y), AccentColor);
            }
        }
        return Clicked;
    }
};
