#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "UiHelpers.h"

// Draws a pulsing "vein" through the gaps between two session panes while they message each other. The vein runs
// from a corner of the sender to a corner of the receiver, and its bulges travel from sender to receiver.
class PeerVeins
{
public:
    struct Box
    {
        ImVec2 Min;
        ImVec2 Max;
    };

    struct Flow
    {
        ImU32 Color = 0;
        float Strength = 0;
        std::vector<float> Packets;
        bool Waiting = false;
    };

    void BeginFrame()
    {
        Panes_.clear();
        Chips_.clear();
    }
    void AddPane(const std::string& Id, const ImVec2 Min, const ImVec2 Max) { Panes_[Id] = {Min, Max}; }
    void AddChip(const std::string& Id, const ImVec2 Min, const ImVec2 Max) { Chips_[Id] = {Min, Max}; }
    const Box* FindPane(const std::string& Id) const
    {
        const std::map<std::string, Box>::const_iterator Found = Panes_.find(Id);
        return Found != Panes_.end() ? &Found->second : nullptr;
    }
    const Box* FindChip(const std::string& Id) const
    {
        const std::map<std::string, Box>::const_iterator Found = Chips_.find(Id);
        return Found != Chips_.end() ? &Found->second : nullptr;
    }

    static constexpr int TOP_LEFT = 0;
    static constexpr int TOP_RIGHT = 1;
    static constexpr int BOTTOM_RIGHT = 2;
    static constexpr int BOTTOM_LEFT = 3;
    static constexpr int CORNER_COUNT = 4;

    // Inflate moves the corner out from the box so the vein runs in the gap beside its border.
    static ImVec2 CornerOf(const Box& Source, const float Inflate, const int Corner)
    {
        const bool Right = Corner == TOP_RIGHT || Corner == BOTTOM_RIGHT;
        const bool Bottom = Corner == BOTTOM_RIGHT || Corner == BOTTOM_LEFT;
        return ImVec2(Right == true ? Source.Max.x + Inflate : Source.Min.x - Inflate, Bottom == true ? Source.Max.y + Inflate : Source.Min.y - Inflate);
    }

    // From the sender's corner along its own row or column line, then along the receiver's, to the receiver's corner.
    // Grid gaps are continuous along those lines, so the vein never crosses a pane.
    void Draw(ImDrawList* const DrawList, const Box& From, const float FromInflate, const int FromCorner, const Box& To, const float ToInflate, const int ToCorner,
              const Flow& Current, const double Time, const float Scale) const
    {
        const ImVec2 Start = CornerOf(From, FromInflate, FromCorner);
        const ImVec2 End = CornerOf(To, ToInflate, ToCorner);
        const ImVec2 Bend = IsSideways(From, To) == true ? ImVec2(End.x, Start.y) : ImVec2(Start.x, End.y);
        DrawBranch(DrawList, {Start, Bend, End}, Current, Time, Scale);
    }

private:
    static constexpr float STEP = 4.0f;
    static constexpr float LANE_OFFSET = 1.5f;  // keeps A->B and B->A apart: each runs on the right of its own travel
    static constexpr float WAVE_LENGTH = 150.0f;
    static constexpr float WAVE_SPEED = 210.0f;
    static constexpr float PACKET_SPREAD = 34.0f;
    static constexpr float DASH_ON = 8.0f;
    static constexpr float DASH_PERIOD = 14.0f;
    static constexpr float DASH_CRAWL = 14.0f;
    static constexpr float TWO_PI = 6.2831853f;

    struct Sample
    {
        ImVec2 Position;
        ImVec2 Normal;  // to the right of the direction of travel
        float Distance = 0;
    };

    static bool IsSideways(const Box& From, const Box& To)
    {
        const bool RowsOverlap = From.Min.y < To.Max.y && To.Min.y < From.Max.y;
        const bool ColumnsOverlap = From.Min.x < To.Max.x && To.Min.x < From.Max.x;
        if (RowsOverlap == true)
        {
            return true;
        }
        if (ColumnsOverlap == true)
        {
            return false;
        }
        const float DeltaX = (To.Min.x + To.Max.x - From.Min.x - From.Max.x) * 0.5f;
        const float DeltaY = (To.Min.y + To.Max.y - From.Min.y - From.Max.y) * 0.5f;
        return std::fabs(DeltaX) > std::fabs(DeltaY);
    }

    static std::vector<Sample> Resample(const std::vector<ImVec2>& Points, const float Step)
    {
        std::vector<Sample> Samples;
        float Distance = 0;
        for (size_t Index = 1; Index < Points.size(); Index++)
        {
            const ImVec2 From = Points[Index - 1];
            const ImVec2 To = Points[Index];
            const float Length = std::sqrt((To.x - From.x) * (To.x - From.x) + (To.y - From.y) * (To.y - From.y));
            if (Length < 0.01f)
            {
                continue;
            }
            const int Count = std::max(1, static_cast<int>(std::ceil(Length / Step)));
            for (int Part = 0; Part < Count; Part++)
            {
                const float Amount = static_cast<float>(Part) / static_cast<float>(Count);
                Samples.push_back({ImVec2(From.x + (To.x - From.x) * Amount, From.y + (To.y - From.y) * Amount), ImVec2(0, 0), Distance + Length * Amount});
            }
            Distance += Length;
        }
        if (Samples.empty() == true)
        {
            return Samples;
        }
        Samples.push_back({Points.back(), ImVec2(0, 0), Distance});
        // Normals from the neighbours on both sides, so the offset strands bend round corners instead of jumping.
        for (size_t Index = 0; Index < Samples.size(); Index++)
        {
            const ImVec2 Before = Samples[Index > 0 ? Index - 1 : 0].Position;
            const ImVec2 After = Samples[std::min(Index + 1, Samples.size() - 1)].Position;
            const float DeltaX = After.x - Before.x;
            const float DeltaY = After.y - Before.y;
            const float Length = std::max(0.001f, std::sqrt(DeltaX * DeltaX + DeltaY * DeltaY));
            Samples[Index].Normal = ImVec2(-DeltaY / Length, DeltaX / Length);
        }
        return Samples;
    }

    // Brightness at a point of the vein: bulges travelling toward the receiver while the pair is talking, plus a
    // brighter pulse for each recent message that fades as it arrives.
    static float Intensity(const float Distance, const float Length, const Flow& Current, const double Time, const float Scale)
    {
        float Value = 0;
        if (Current.Strength > 0.0f)
        {
            const float Phase = (Distance - static_cast<float>(Time) * WAVE_SPEED * Scale) / (WAVE_LENGTH * Scale);
            const float Wave = std::pow(0.5f + 0.5f * std::sin(Phase * TWO_PI), 6.0f);
            Value += Current.Strength * (0.3f + 0.7f * Wave);
        }
        for (const float Progress : Current.Packets)
        {
            const float Offset = (Distance - Progress * Length) / (PACKET_SPREAD * Scale);
            const float Arriving = Progress < 0.85f ? 1.0f : (1.0f - Progress) / 0.15f;
            Value += 1.2f * Arriving * std::exp(-Offset * Offset);
        }
        return std::min(Value, 1.6f);
    }

    static ImVec2 Offset(const Sample& Point, const float Amount)
    {
        return ImVec2(Point.Position.x + Point.Normal.x * Amount, Point.Position.y + Point.Normal.y * Amount);
    }

    static void DrawBranch(ImDrawList* const DrawList, const std::vector<ImVec2>& Points, const Flow& Current, const double Time, const float Scale)
    {
        const std::vector<Sample> Samples = Resample(Points, STEP * Scale);
        if (Samples.size() < 2)
        {
            return;
        }
        const float Length = Samples.back().Distance;
        const float Lane = LANE_OFFSET * Scale;
        const float Clock = static_cast<float>(Time);

        if (Current.Waiting == true)
        {
            const float Breath = 0.5f + 0.5f * std::sin(Clock * 2.0f);
            const ImU32 Dash = UiHelpers::WithAlpha(Current.Color, static_cast<int>(70 + 70 * Breath));
            const ImU32 DashGlow = UiHelpers::WithAlpha(Current.Color, static_cast<int>(18 + 22 * Breath));
            for (size_t Index = 1; Index < Samples.size(); Index++)
            {
                float Phase = std::fmod(Samples[Index].Distance - Clock * DASH_CRAWL * Scale, DASH_PERIOD * Scale);
                if (Phase < 0.0f)
                {
                    Phase += DASH_PERIOD * Scale;
                }
                if (Phase > DASH_ON * Scale)
                {
                    continue;
                }
                const ImVec2 From = Offset(Samples[Index - 1], Lane);
                const ImVec2 To = Offset(Samples[Index], Lane);
                DrawList->AddLine(From, To, DashGlow, 4.0f * Scale);
                DrawList->AddLine(From, To, Dash, 1.5f * Scale);
            }
        }

        std::vector<float> Brightness(Samples.size());
        std::vector<ImVec2> MainStrand(Samples.size());
        std::vector<ImVec2> SideStrand(Samples.size());
        for (size_t Index = 0; Index < Samples.size(); Index++)
        {
            const float Distance = Samples[Index].Distance;
            Brightness[Index] = Intensity(Distance, Length, Current, Time, Scale);
            // Two strands twisting round each other, swelling where the pulse is, give the vein look.
            const float Swell = (0.8f + 1.2f * Brightness[Index]) * Scale;
            MainStrand[Index] = Offset(Samples[Index], Lane + Swell * std::sin(Distance * 0.045f / Scale + Clock * 1.7f));
            SideStrand[Index] = Offset(Samples[Index], Lane - Swell * std::sin(Distance * 0.071f / Scale - Clock * 1.1f + 2.0f));
        }
        for (size_t Index = 1; Index < Samples.size(); Index++)
        {
            const float Glow = (Brightness[Index - 1] + Brightness[Index]) * 0.5f;
            if (Glow < 0.02f)
            {
                continue;
            }
            DrawList->AddLine(Offset(Samples[Index - 1], Lane), Offset(Samples[Index], Lane), UiHelpers::WithAlpha(Current.Color, std::min(255, static_cast<int>(55 * Glow))),
                              (3.0f + 7.0f * Glow) * Scale);
        }
        for (size_t Index = 1; Index < Samples.size(); Index++)
        {
            const float Glow = (Brightness[Index - 1] + Brightness[Index]) * 0.5f;
            if (Glow < 0.02f)
            {
                continue;
            }
            const float Hot = std::min(1.0f, Glow);
            const ImU32 Core = UiHelpers::LerpColor(Current.Color, IM_COL32_WHITE, 0.55f * Hot);
            DrawList->AddLine(SideStrand[Index - 1], SideStrand[Index], UiHelpers::WithAlpha(Current.Color, static_cast<int>(50 + 140 * Hot)), (0.7f + 1.0f * Glow) * Scale);
            DrawList->AddLine(MainStrand[Index - 1], MainStrand[Index], UiHelpers::WithAlpha(Core, static_cast<int>(70 + 185 * Hot)), (1.0f + 1.6f * Glow) * Scale);
        }
    }

    std::map<std::string, Box> Panes_;
    std::map<std::string, Box> Chips_;
};
