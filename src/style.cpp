#include "style.h"

#include <algorithm>
#include <cmath>

ImVec4 toImVec4(const ThemeColor& c) { return ImVec4(c.r, c.g, c.b, c.a); }

ImVec4 shadeColor(const ThemeColor& c, float amount) {
  return ImVec4(std::clamp(c.r + amount, 0.0f, 1.0f), std::clamp(c.g + amount, 0.0f, 1.0f),
                std::clamp(c.b + amount, 0.0f, 1.0f), c.a);
}

platform::RgbColor toRgbColor(const ThemeColor& c) {
  auto b = [](float v) { return (unsigned char)std::clamp((int)std::round(v * 255.0f), 0, 255); };
  return platform::RgbColor{b(c.r), b(c.g), b(c.b)};
}

void applyStyle(float scale, const ThemeColors& th) {
  ImGuiStyle& st = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  st.WindowRounding = 8;
  st.FrameRounding = 6;
  st.ChildRounding = 6;
  st.PopupRounding = 8;
  st.GrabRounding = 6;
  st.FramePadding = ImVec2(9, 5);
  st.ItemSpacing = ImVec2(8, 6);
  st.WindowBorderSize = 1;
  st.FrameBorderSize = 0;
  ImVec4* c = st.Colors;
  c[ImGuiCol_Text] = toImVec4(th.text);
  c[ImGuiCol_TextDisabled] = toImVec4(th.textDisabled);
  c[ImGuiCol_WindowBg] = toImVec4(th.background);
  c[ImGuiCol_PopupBg] = ImVec4(th.surface.r, th.surface.g, th.surface.b, 0.99f);
  c[ImGuiCol_Border] = toImVec4(th.paneBorder);
  c[ImGuiCol_FrameBg] = toImVec4(th.background);
  c[ImGuiCol_FrameBgHovered] = shadeColor(th.background, 0.05f);
  c[ImGuiCol_FrameBgActive] = shadeColor(th.background, 0.08f);
  c[ImGuiCol_Button] = shadeColor(th.surface, 0.03f);
  c[ImGuiCol_ButtonHovered] = shadeColor(th.surface, 0.08f);
  c[ImGuiCol_ButtonActive] = shadeColor(th.surface, 0.12f);
  c[ImGuiCol_Header] = ImVec4(th.accent.r, th.accent.g, th.accent.b, 0.22f);
  c[ImGuiCol_HeaderHovered] = shadeColor(th.surface, 0.10f);
  c[ImGuiCol_HeaderActive] = ImVec4(th.accent.r, th.accent.g, th.accent.b, 0.35f);
  c[ImGuiCol_CheckMark] = toImVec4(th.accent);
  c[ImGuiCol_SliderGrab] = toImVec4(th.accent);
  c[ImGuiCol_SliderGrabActive] = shadeColor(th.accent, 0.1f);
  c[ImGuiCol_Separator] = toImVec4(th.paneBorder);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = shadeColor(th.surface, 0.1f);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
  st.ScaleAllSizes(scale);
}
