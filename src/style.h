#pragma once
#include "imgui.h"
#include "platform.h"
#include "store.h"

ImVec4 toImVec4(const ThemeColor& c);
ImVec4 shadeColor(const ThemeColor& c, float amount);
platform::RgbColor toRgbColor(const ThemeColor& c);
void applyStyle(float scale, const ThemeColors& theme);
