#pragma once
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "PeerVeins.h"

// Gives each conversation its own corner of both panes, so a session talking to up to four others never has two
// veins leaving from the same corner. A session with a single peer always uses its top-left corner.
class PeerCorners
{
public:
    struct Choice
    {
        int FromCorner = PeerVeins::TOP_LEFT;
        int ToCorner = PeerVeins::TOP_LEFT;
    };

    void AddLink(const std::string& FromId, const std::string& ToId)
    {
        Peers_[FromId].insert(ToId);
        Peers_[ToId].insert(FromId);
    }

    // Both directions of a pair share one choice; the lane offset in the vein keeps them apart.
    Choice Pick(const std::string& FromId, const PeerVeins::Box& From, const float FromInflate, const std::string& ToId, const PeerVeins::Box& To,
                const float ToInflate, const float MinLength)
    {
        const bool Swapped = ToId < FromId;
        const std::pair<std::string, std::string> Key = Swapped == true ? std::make_pair(ToId, FromId) : std::make_pair(FromId, ToId);
        const std::map<std::pair<std::string, std::string>, Choice>::const_iterator Found = Chosen_.find(Key);
        if (Found != Chosen_.end())
        {
            return Swapped == true ? Choice{Found->second.ToCorner, Found->second.FromCorner} : Found->second;
        }

        Choice Best;
        float BestCost = std::numeric_limits<float>::max();
        const int FromLast = HasSinglePeer(FromId) == true ? PeerVeins::TOP_LEFT : PeerVeins::CORNER_COUNT - 1;
        const int ToLast = HasSinglePeer(ToId) == true ? PeerVeins::TOP_LEFT : PeerVeins::CORNER_COUNT - 1;
        for (int FromCorner = 0; FromCorner <= FromLast; FromCorner++)
        {
            for (int ToCorner = 0; ToCorner <= ToLast; ToCorner++)
            {
                const ImVec2 Start = PeerVeins::CornerOf(From, FromInflate, FromCorner);
                const ImVec2 End = PeerVeins::CornerOf(To, ToInflate, ToCorner);
                const float Distance = std::fabs(End.x - Start.x) + std::fabs(End.y - Start.y);
                // A vein between corners that meet in the gap would be invisible, which is worse than sharing a corner.
                const float Cost = Distance + (Distance < MinLength ? COINCIDENT_COST : 0.0f) +
                                   SHARED_COST * static_cast<float>(Usage_[FromId][FromCorner] + Usage_[ToId][ToCorner]);
                if (Cost >= BestCost)
                {
                    continue;
                }
                BestCost = Cost;
                Best = {FromCorner, ToCorner};
            }
        }
        Usage_[FromId][Best.FromCorner]++;
        Usage_[ToId][Best.ToCorner]++;
        Chosen_[Key] = Swapped == true ? Choice{Best.ToCorner, Best.FromCorner} : Best;
        return Best;
    }

private:
    static constexpr float SHARED_COST = 1.0e6f;
    static constexpr float COINCIDENT_COST = 1.0e8f;

    bool HasSinglePeer(const std::string& Id) const
    {
        const std::map<std::string, std::set<std::string>>::const_iterator Found = Peers_.find(Id);
        return Found == Peers_.end() || Found->second.size() <= 1;
    }

    std::map<std::string, std::set<std::string>> Peers_;
    std::map<std::string, std::array<int, PeerVeins::CORNER_COUNT>> Usage_;
    std::map<std::pair<std::string, std::string>, Choice> Chosen_;
};
