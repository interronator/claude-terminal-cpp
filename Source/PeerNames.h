#pragma once
#include <cctype>
#include <string>

#include "ScreenshotFinder.h"

// The name a session gets in Claude's own session list (ListAgents), so sessions can find each other by the
// tag the app hands out ("fix-sidebar-cs-1a2b3c4d").
class PeerNames
{
public:
    static std::string For(const std::string& SessionName, const std::string& SessionId)
    {
        static constexpr size_t MAX_SLUG_LENGTH = 24;
        std::string Slug;
        for (const char Character : SessionName)
        {
            if (Slug.size() >= MAX_SLUG_LENGTH)
            {
                break;
            }
            if (std::isalnum(static_cast<unsigned char>(Character)) != 0)
            {
                Slug += static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
                continue;
            }
            if (Slug.empty() == false && Slug.back() != '-')
            {
                Slug += '-';
            }
        }
        while (Slug.empty() == false && Slug.back() == '-')
        {
            Slug.pop_back();
        }
        const std::string Tag = ScreenshotFinder::SessionTag(SessionId);
        if (Slug.empty() == true)
        {
            return Tag;
        }
        return Tag.empty() == true ? Slug : Slug + "-" + Tag;
    }
};
