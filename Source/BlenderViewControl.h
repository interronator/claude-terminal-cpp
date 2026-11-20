#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "nlohmann/json.hpp"

// Steers the 3D viewport inside Blender from the review window. The app keeps running totals of what the user did (orbit, zoom,
// pan, "frame all") and writes them to <tag>_live_view.json; the capture script in Blender applies the difference since the last
// totals it saw. Totals instead of deltas mean a missed or repeated read never moves the view twice.
class BlenderViewControl
{
public:
    static constexpr double FLUSH_INTERVAL_SECONDS = 0.06;
    static constexpr double INFO_CACHE_SECONDS = 1.0;
    static constexpr float ORBIT_RADIANS_PER_PIXEL = 0.01f;
    static constexpr float ZOOM_PER_WHEEL_STEP = 0.15f;
    static constexpr float PAN_PER_PIXEL = 0.0016f;

    static std::filesystem::path ViewPath(const std::string& OwnDirectory, const std::string& Tag)
    {
        return std::filesystem::path(OwnDirectory) / (Tag + "_live_view.json");
    }

    static std::filesystem::path InfoPath(const std::string& OwnDirectory, const std::string& Tag)
    {
        return std::filesystem::path(OwnDirectory) / (Tag + "_live_info.json");
    }

    // A fresh id tells the script to start counting from zero again.
    void Reset()
    {
        Sequence_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        Yaw_ = 0.0f;
        Pitch_ = 0.0f;
        Zoom_ = 0.0f;
        PanX_ = 0.0f;
        PanY_ = 0.0f;
        FrameAllCount_ = 0;
        Dirty_ = false;
        LastWriteAt_ = 0.0;
    }

    void Orbit(const float PixelsRight, const float PixelsDown)
    {
        Yaw_ += PixelsRight * ORBIT_RADIANS_PER_PIXEL;
        Pitch_ += PixelsDown * ORBIT_RADIANS_PER_PIXEL;
        Dirty_ = true;
    }

    void Zoom(const float WheelSteps)
    {
        Zoom_ += WheelSteps * ZOOM_PER_WHEEL_STEP;
        Dirty_ = true;
    }

    void Pan(const float PixelsRight, const float PixelsDown)
    {
        PanX_ += PixelsRight * PAN_PER_PIXEL;
        PanY_ += PixelsDown * PAN_PER_PIXEL;
        Dirty_ = true;
    }

    void FrameAll()
    {
        FrameAllCount_++;
        Dirty_ = true;
    }

    // Writes the totals when something changed, at most every FLUSH_INTERVAL_SECONDS.
    void Flush(const std::string& OwnDirectory, const std::string& Tag, const double Now)
    {
        if (Dirty_ == false || OwnDirectory.empty() == true || Tag.empty() == true || Now - LastWriteAt_ < FLUSH_INTERVAL_SECONDS)
        {
            return;
        }
        nlohmann::json Body;
        Body["Seq"] = Sequence_;
        Body["Yaw"] = Yaw_;
        Body["Pitch"] = Pitch_;
        Body["Zoom"] = Zoom_;
        Body["PanX"] = PanX_;
        Body["PanY"] = PanY_;
        Body["FrameAll"] = FrameAllCount_;
        std::error_code ErrorCode;
        std::filesystem::create_directories(OwnDirectory, ErrorCode);
        const std::filesystem::path Final = ViewPath(OwnDirectory, Tag);
        std::filesystem::path Pending = Final;
        Pending += ".tmp";
        {
            std::ofstream Stream(Pending, std::ios::binary | std::ios::trunc);
            Stream << Body.dump();
        }
        std::filesystem::rename(Pending, Final, ErrorCode);
        if (ErrorCode.value() != 0)
        {
            return;
        }
        Dirty_ = false;
        LastWriteAt_ = Now;
    }

    // The script version the running Blender reported when it connected; 0 when it never did (or runs an older script).
    int ScriptVersion(const std::string& OwnDirectory, const std::string& Tag, const double Now)
    {
        if (Now < InfoCheckedAt_ + INFO_CACHE_SECONDS)
        {
            return CachedVersion_;
        }
        InfoCheckedAt_ = Now;
        CachedVersion_ = 0;
        if (OwnDirectory.empty() == true || Tag.empty() == true)
        {
            return 0;
        }
        std::ifstream Stream(InfoPath(OwnDirectory, Tag), std::ios::binary);
        if (Stream.good() == false)
        {
            return 0;
        }
        const nlohmann::json Info = nlohmann::json::parse(Stream, nullptr, false);
        if (Info.is_object() == true)
        {
            CachedVersion_ = Info.value("Version", 0);
        }
        return CachedVersion_;
    }

private:
    std::int64_t Sequence_ = 0;
    float Yaw_ = 0.0f;
    float Pitch_ = 0.0f;
    float Zoom_ = 0.0f;
    float PanX_ = 0.0f;
    float PanY_ = 0.0f;
    int FrameAllCount_ = 0;
    bool Dirty_ = false;
    double LastWriteAt_ = 0.0;
    double InfoCheckedAt_ = -1000.0;
    int CachedVersion_ = 0;
};
