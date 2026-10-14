#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <string>

#include "imgui.h"

class UiHelpers
{
public:
    static constexpr ImU32 RgbCol(const uint32_t Value, const int Alpha = 255)
    {
        return IM_COL32((Value >> 16) & 0xFF, (Value >> 8) & 0xFF, Value & 0xFF, Alpha);
    }

    static constexpr ImU32 WithAlpha(const ImU32 Color, const int Alpha)
    {
        return (Color & 0x00FFFFFF) | ((ImU32)Alpha << 24);
    }

    static ImU32 LerpColor(const ImU32 From, const ImU32 To, const float Amount)
    {
        const float Clamped = Amount < 0.0f ? 0.0f : (Amount > 1.0f ? 1.0f : Amount);
        const int Red = (int)(((From >> IM_COL32_R_SHIFT) & 0xFF) * (1.0f - Clamped) + ((To >> IM_COL32_R_SHIFT) & 0xFF) * Clamped);
        const int Green = (int)(((From >> IM_COL32_G_SHIFT) & 0xFF) * (1.0f - Clamped) + ((To >> IM_COL32_G_SHIFT) & 0xFF) * Clamped);
        const int Blue = (int)(((From >> IM_COL32_B_SHIFT) & 0xFF) * (1.0f - Clamped) + ((To >> IM_COL32_B_SHIFT) & 0xFF) * Clamped);
        return IM_COL32(Red, Green, Blue, 255);
    }

    // A stable color per session, used on its pane header and on its screenshot cards so the two read as a pair.
    static ImU32 SessionTint(const std::string& SessionId)
    {
        static constexpr uint32_t PALETTE[] = {0xE5735C, 0x5CB8E5, 0x8FD16A, 0xE5C15C, 0xB57CE5, 0x5CE5C1, 0xE57CB8, 0x7C95E5};
        const size_t Hash = std::hash<std::string>{}(SessionId);
        return RgbCol(PALETTE[Hash % (sizeof(PALETTE) / sizeof(PALETTE[0]))]);
    }

    static std::string FormatAgo(const std::time_t Then)
    {
        const long long Seconds = static_cast<long long>(std::difftime(std::time(nullptr), Then));
        if (Seconds < 10)
        {
            return "just now";
        }
        if (Seconds < 60)
        {
            return std::to_string(Seconds) + "s ago";
        }
        if (Seconds < 3600)
        {
            return std::to_string(Seconds / 60) + "m ago";
        }
        if (Seconds < 86400)
        {
            return std::to_string(Seconds / 3600) + "h ago";
        }
        return std::to_string(Seconds / 86400) + "d ago";
    }

    static constexpr float USAGE_LABEL_GAP = 6.0f;
    static constexpr long long SESSION_WINDOW_SECONDS = 5LL * 3600;
    static constexpr long long WEEKLY_WINDOW_SECONDS = 7LL * 24 * 3600;
    static constexpr double USAGE_RAMP_START_PCT = 30.0;
    static constexpr double USAGE_WARNING_PCT = 65.0;
    static constexpr double USAGE_FULL_PCT = 100.0;

    // Base color until the ramp starts, Warning by 65%, Full by 100% (and it stays Full while locked out).
    static ImU32 UsageColor(const ImU32 BaseColor, const ImU32 WarningColor, const ImU32 FullColor, const double Pct)
    {
        if (Pct <= USAGE_RAMP_START_PCT)
        {
            return BaseColor;
        }
        if (Pct <= USAGE_WARNING_PCT)
        {
            return LerpColor(BaseColor, WarningColor, (float)((Pct - USAGE_RAMP_START_PCT) / (USAGE_WARNING_PCT - USAGE_RAMP_START_PCT)));
        }
        return LerpColor(WarningColor, FullColor, (float)((Pct - USAGE_WARNING_PCT) / (USAGE_FULL_PCT - USAGE_WARNING_PCT)));
    }

    // While locked out (full), how far the reset countdown has progressed: 0 right after the lockout, 1 at the reset. 0 when not full.
    static float UsageResetProgress(const double Pct, const long long SecondsUntilReset, const long long WindowSeconds)
    {
        if (Pct < USAGE_FULL_PCT || SecondsUntilReset < 0 || WindowSeconds <= 0)
        {
            return 0.0f;
        }
        const float Progress = 1.0f - (float)SecondsUntilReset / (float)WindowSeconds;
        return Progress < 0.0f ? 0.0f : (Progress > 1.0f ? 1.0f : Progress);
    }

    // Seconds from now until an ISO-8601 timestamp (never negative), or -1 if it cannot be parsed.
    static long long SecondsUntil(const std::string& Iso8601)
    {
        int Year = 0, Month = 0, Day = 0, Hour = 0, Minute = 0, Second = 0;
        if (std::sscanf(Iso8601.c_str(), "%d-%d-%dT%d:%d:%d", &Year, &Month, &Day, &Hour, &Minute, &Second) != 6)
        {
            return -1;
        }
        std::tm Utc = {};
        Utc.tm_year = Year - 1900;
        Utc.tm_mon = Month - 1;
        Utc.tm_mday = Day;
        Utc.tm_hour = Hour;
        Utc.tm_min = Minute;
        Utc.tm_sec = Second;
#ifdef _WIN32
        std::time_t Target = _mkgmtime(&Utc);
#else
        std::time_t Target = timegm(&Utc);
#endif
        const size_t OffsetPos = Iso8601.find_first_of("+-", 19);
        int OffsetHours = 0, OffsetMinutes = 0;
        if (OffsetPos != std::string::npos && std::sscanf(Iso8601.c_str() + OffsetPos + 1, "%d:%d", &OffsetHours, &OffsetMinutes) == 2)
        {
            const long OffsetSeconds = OffsetHours * 3600L + OffsetMinutes * 60L;
            Target += Iso8601[OffsetPos] == '+' ? -OffsetSeconds : OffsetSeconds;
        }
        const long long Remaining = (long long)std::difftime(Target, std::time(nullptr));
        return Remaining < 0 ? 0 : Remaining;
    }

    // Formats the time remaining until an ISO-8601 timestamp, e.g. "1d 4h 12m". Empty if unparseable.
    static std::string FormatTimeUntil(const std::string& Iso8601)
    {
        const long long Remaining = SecondsUntil(Iso8601);
        if (Remaining < 0)
        {
            return {};
        }
        const long long TotalMinutes = (Remaining + 59) / 60;
        const long long Days = TotalMinutes / (60 * 24);
        const long long Hours = (TotalMinutes / 60) % 24;
        const long long Minutes = TotalMinutes % 60;
        std::string Text;
        if (Days > 0)
        {
            Text += std::to_string(Days) + "d ";
        }
        if (Days > 0 || Hours > 0)
        {
            Text += std::to_string(Hours) + "h ";
        }
        return Text + std::to_string(Minutes) + "m";
    }

    static bool FileExists(const std::string& Path)
    {
        std::error_code ErrorCode;
        return std::filesystem::exists(Path, ErrorCode);
    }

    static std::string FirstExisting(const std::initializer_list<std::string> Paths)
    {
        for (const std::string& Path : Paths)
        {
            if (Path.empty() == false && FileExists(Path) == true)
            {
                return Path;
            }
        }
        return {};
    }

    static std::string EnvOr(const char* const Name)
    {
        const char* const Value = std::getenv(Name);
        return Value != nullptr ? Value : "";
    }

    // Arm weights for light/heavy box-drawing characters: {left, right, up, down}; 0 none, 1 light, 2 heavy.
    static bool BoxArms(const uint32_t CodePoint, int& Left, int& Right, int& Up, int& Down)
    {
        Left = Right = Up = Down = 0;
        switch (CodePoint)
        {
            case 0x2500: Left = Right = 1; return true;
            case 0x2501: Left = Right = 2; return true;
            case 0x2502: Up = Down = 1; return true;
            case 0x2503: Up = Down = 2; return true;
            case 0x250C: case 0x256D: Right = Down = 1; return true;
            case 0x250F: Right = Down = 2; return true;
            case 0x2510: case 0x256E: Left = Down = 1; return true;
            case 0x2513: Left = Down = 2; return true;
            case 0x2514: case 0x2570: Right = Up = 1; return true;
            case 0x2517: Right = Up = 2; return true;
            case 0x2518: case 0x256F: Left = Up = 1; return true;
            case 0x251B: Left = Up = 2; return true;
            case 0x251C: Up = Down = Right = 1; return true;
            case 0x2523: Up = Down = Right = 2; return true;
            case 0x2524: Up = Down = Left = 1; return true;
            case 0x252B: Up = Down = Left = 2; return true;
            case 0x252C: Left = Right = Down = 1; return true;
            case 0x2533: Left = Right = Down = 2; return true;
            case 0x2534: Left = Right = Up = 1; return true;
            case 0x253B: Left = Right = Up = 2; return true;
            case 0x253C: Left = Right = Up = Down = 1; return true;
            case 0x254B: Left = Right = Up = Down = 2; return true;
            case 0x2550: Left = Right = 2; return true;  // double lines drawn heavy
            case 0x2551: Up = Down = 2; return true;
            case 0x2554: Right = Down = 2; return true;
            case 0x2557: Left = Down = 2; return true;
            case 0x255A: Right = Up = 2; return true;
            case 0x255D: Left = Up = 2; return true;
            case 0x2574: Left = 1; return true;
            case 0x2575: Up = 1; return true;
            case 0x2576: Right = 1; return true;
            case 0x2577: Down = 1; return true;
            default: return false;
        }
    }
};
