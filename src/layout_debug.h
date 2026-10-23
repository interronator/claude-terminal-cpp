#pragma once
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"
#include "store.h"

// Set CT_DEBUG_LAYOUT=1 to log every change of a sampled layout value, with the mouse position and the
// window under the mouse, to layout-debug.log next to state.json. Used to track down layout jitter.
class LayoutDebug
{
public:
    LayoutDebug()
    {
        const char* const Flag = std::getenv("CT_DEBUG_LAYOUT");
        Enabled_ = Flag != nullptr && std::string(Flag) == "1";
        if (Enabled_ == false)
        {
            return;
        }
        const std::filesystem::path LogPath = std::filesystem::path(Store::DefaultPath()).parent_path() / "layout-debug.log";
        Log_ = std::fopen(LogPath.string().c_str(), "w");
    }

    ~LayoutDebug()
    {
        if (Log_ != nullptr)
        {
            std::fclose(Log_);
        }
    }

    LayoutDebug(const LayoutDebug&) = delete;
    LayoutDebug& operator=(const LayoutDebug&) = delete;

    void Sample(const char* const Label, const float Value)
    {
        if (Enabled_ == false || Log_ == nullptr)
        {
            return;
        }
        std::map<std::string, float>::iterator Found = Last_.find(Label);
        if (Found != Last_.end() && Found->second == Value)
        {
            return;
        }
        const bool IsFirst = Found == Last_.end();
        const float Previous = IsFirst == true ? Value : Found->second;
        Last_[Label] = Value;
        const ImVec2 Mouse = ImGui::GetIO().MousePos;
        const ImGuiWindow* const Hovered = GImGui != nullptr ? GImGui->HoveredWindow : nullptr;
        std::fprintf(Log_, "frame=%d %s %.2f -> %.2f  mouse=(%.0f,%.0f) hovered=%s\n", ImGui::GetFrameCount(), Label, Previous, Value, Mouse.x, Mouse.y,
                     Hovered != nullptr ? Hovered->Name : "none");
        std::fflush(Log_);
    }

private:
    bool Enabled_ = false;
    std::FILE* Log_ = nullptr;
    std::map<std::string, float> Last_;
};
