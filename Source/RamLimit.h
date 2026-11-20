#pragma once
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Platform.h"
#include "Session.h"
#include "Store.h"

// Caps the RAM that Claude sessions started by this app may use together, as a percent of the PC's RAM or a fixed
// amount. Running sessions are never killed; going over the cap only stops further sessions from starting.
class RamLimit
{
public:
    void Refresh(const Settings& AppSettings, const std::vector<std::unique_ptr<Session>>& Sessions, const double Now)
    {
        if (Now - LastSampleAt_ < SAMPLE_SECONDS)
        {
            return;
        }
        LastSampleAt_ = Now;
        TotalBytes_ = Platform::TotalRamBytes();
        LimitBytes_ = 0;
        if (AppSettings.RamLimitMode == 1 && TotalBytes_ > 0)
        {
            LimitBytes_ = TotalBytes_ / 100 * static_cast<unsigned long long>(AppSettings.RamLimitPercent);
        }
        else if (AppSettings.RamLimitMode == 2)
        {
            LimitBytes_ = static_cast<unsigned long long>(AppSettings.RamLimitGb * BYTES_PER_GB);
        }
        UsedBytes_ = 0;
        for (const std::unique_ptr<Session>& SessionPtr : Sessions)
        {
            UsedBytes_ += SessionPtr->MemoryBytes();
        }
    }

    void Invalidate() { LastSampleAt_ = -1.0e9; }
    bool HasLimit() const { return LimitBytes_ > 0; }
    bool IsOver() const { return LimitBytes_ > 0 && UsedBytes_ >= LimitBytes_; }
    double UsedGb() const { return static_cast<double>(UsedBytes_) / BYTES_PER_GB; }
    double LimitGb() const { return static_cast<double>(LimitBytes_) / BYTES_PER_GB; }
    double TotalGb() const { return static_cast<double>(TotalBytes_) / BYTES_PER_GB; }

    std::string Summary() const
    {
        char Text[96];
        std::snprintf(Text, sizeof Text, "RAM %.1f / %.1f GB", UsedGb(), LimitGb());
        return Text;
    }

    std::string BlockedMessage() const
    {
        char Text[160];
        std::snprintf(Text, sizeof Text, "the RAM limit is reached (sessions use %.1f of %.1f GB). Close a session or raise the limit in Settings.", UsedGb(),
                      LimitGb());
        return Text;
    }

private:
    static constexpr double SAMPLE_SECONDS = 2.0;
    static constexpr double BYTES_PER_GB = 1024.0 * 1024.0 * 1024.0;

    double LastSampleAt_ = -1.0e9;
    unsigned long long TotalBytes_ = 0;
    unsigned long long LimitBytes_ = 0;
    unsigned long long UsedBytes_ = 0;
};
