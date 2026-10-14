#pragma once
#include <algorithm>
#include <cmath>
#include <functional>

#include "imgui.h"
#include "platform.h"
#include "store.h"

class Style
{
public:
    static ImVec4 ToImVec4(const ThemeColor& CurrentColor)
    {
        return ImVec4(CurrentColor.Red, CurrentColor.Green, CurrentColor.Blue, CurrentColor.Alpha);
    }

    static ImVec4 ShadeColor(const ThemeColor& CurrentColor, float Amount)
    {
        return ImVec4(std::clamp(CurrentColor.Red + Amount, 0.0f, 1.0f), std::clamp(CurrentColor.Green + Amount, 0.0f, 1.0f),
                      std::clamp(CurrentColor.Blue + Amount, 0.0f, 1.0f), CurrentColor.Alpha);
    }

    static Platform::RgbColor ToRgbColor(const ThemeColor& CurrentColor)
    {
        const std::function<unsigned char(float)> ToByte = [](float Value) { return (unsigned char)std::clamp((int)std::round(Value * 255.0f), 0, 255); };
        return Platform::RgbColor{ToByte(CurrentColor.Red), ToByte(CurrentColor.Green), ToByte(CurrentColor.Blue)};
    }

    static void Apply(float Scale, const ThemeColors& CurrentTheme)
    {
        ImGuiStyle& CurrentStyle = ImGui::GetStyle();
        CurrentStyle = ImGuiStyle();
        ImGui::StyleColorsDark();
        CurrentStyle.WindowRounding = 8;
        CurrentStyle.FrameRounding = 6;
        CurrentStyle.ChildRounding = 6;
        CurrentStyle.PopupRounding = 8;
        CurrentStyle.GrabRounding = 6;
        CurrentStyle.FramePadding = ImVec2(9, 5);
        CurrentStyle.ItemSpacing = ImVec2(8, 6);
        CurrentStyle.WindowBorderSize = 1;
        CurrentStyle.FrameBorderSize = 0;
        ImVec4* const Colors = CurrentStyle.Colors;
        Colors[ImGuiCol_Text] = ToImVec4(CurrentTheme.Text);
        Colors[ImGuiCol_TextDisabled] = ToImVec4(CurrentTheme.TextDisabled);
        Colors[ImGuiCol_WindowBg] = ToImVec4(CurrentTheme.Background);
        Colors[ImGuiCol_PopupBg] = ImVec4(CurrentTheme.Surface.Red, CurrentTheme.Surface.Green, CurrentTheme.Surface.Blue, 0.99f);
        Colors[ImGuiCol_Border] = ToImVec4(CurrentTheme.PaneBorder);
        Colors[ImGuiCol_FrameBg] = ToImVec4(CurrentTheme.Background);
        Colors[ImGuiCol_FrameBgHovered] = ShadeColor(CurrentTheme.Background, 0.05f);
        Colors[ImGuiCol_FrameBgActive] = ShadeColor(CurrentTheme.Background, 0.08f);
        Colors[ImGuiCol_Button] = ShadeColor(CurrentTheme.Surface, 0.03f);
        Colors[ImGuiCol_ButtonHovered] = ShadeColor(CurrentTheme.Surface, 0.08f);
        Colors[ImGuiCol_ButtonActive] = ShadeColor(CurrentTheme.Surface, 0.12f);
        Colors[ImGuiCol_Header] = ImVec4(CurrentTheme.Accent.Red, CurrentTheme.Accent.Green, CurrentTheme.Accent.Blue, 0.22f);
        Colors[ImGuiCol_HeaderHovered] = ShadeColor(CurrentTheme.Surface, 0.10f);
        Colors[ImGuiCol_HeaderActive] = ImVec4(CurrentTheme.Accent.Red, CurrentTheme.Accent.Green, CurrentTheme.Accent.Blue, 0.35f);
        Colors[ImGuiCol_CheckMark] = ToImVec4(CurrentTheme.Accent);
        Colors[ImGuiCol_SliderGrab] = ToImVec4(CurrentTheme.Accent);
        Colors[ImGuiCol_SliderGrabActive] = ShadeColor(CurrentTheme.Accent, 0.1f);
        Colors[ImGuiCol_Separator] = ToImVec4(CurrentTheme.PaneBorder);
        Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        Colors[ImGuiCol_ScrollbarGrab] = ShadeColor(CurrentTheme.Surface, 0.1f);
        Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
        CurrentStyle.ScaleAllSizes(Scale);
    }
};
