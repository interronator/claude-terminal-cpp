#pragma once
#include <array>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "PeerVeins.h"

// Gives each conversation its own edge of both panes, so a session talking to up to four others never has two
// veins leaving from the same edge. A session with a single peer always uses the middle of its top edge.
class PeerAnchors
{
public:
    void AddLink(const std::string& FromId, const std::string& ToId)
    {
        Peers_[FromId].insert(ToId);
        Peers_[ToId].insert(FromId);
    }

    // Both directions of a pair share one route, reversed; the lane offset in the vein keeps them apart.
    std::vector<ImVec2> Pick(const std::string& FromId, const PeerVeins::Box& From, const float FromInflate, const std::string& ToId, const PeerVeins::Box& To,
                             const float ToInflate, const float MinLength)
    {
        const bool Swapped = ToId < FromId;
        const std::pair<std::string, std::string> Key = Swapped == true ? std::make_pair(ToId, FromId) : std::make_pair(FromId, ToId);
        const std::map<std::pair<std::string, std::string>, std::pair<int, int>>::const_iterator Found = Chosen_.find(Key);
        if (Found != Chosen_.end())
        {
            const int FromEdge = Swapped == true ? Found->second.second : Found->second.first;
            const int ToEdge = Swapped == true ? Found->second.first : Found->second.second;
            return PeerVeins::Route(From, FromInflate, FromEdge, To, ToInflate, ToEdge);
        }

        int BestFrom = PeerVeins::TOP;
        int BestTo = PeerVeins::TOP;
        float BestCost = std::numeric_limits<float>::max();
        const int FromLast = HasSinglePeer(FromId) == true ? PeerVeins::TOP : PeerVeins::EDGE_COUNT - 1;
        const int ToLast = HasSinglePeer(ToId) == true ? PeerVeins::TOP : PeerVeins::EDGE_COUNT - 1;
        for (int FromEdge = 0; FromEdge <= FromLast; FromEdge++)
        {
            for (int ToEdge = 0; ToEdge <= ToLast; ToEdge++)
            {
                const float Length = PeerVeins::RouteLength(PeerVeins::Route(From, FromInflate, FromEdge, To, ToInflate, ToEdge));
                // Anchors that meet across a gap would give an invisible vein, which is worse than sharing an edge.
                const float Cost = Length + (Length < MinLength ? COINCIDENT_COST : 0.0f) +
                                   SHARED_COST * static_cast<float>(Usage_[FromId][FromEdge] + Usage_[ToId][ToEdge]);
                if (Cost >= BestCost)
                {
                    continue;
                }
                BestCost = Cost;
                BestFrom = FromEdge;
                BestTo = ToEdge;
            }
        }
        Usage_[FromId][BestFrom]++;
        Usage_[ToId][BestTo]++;
        Chosen_[Key] = Swapped == true ? std::make_pair(BestTo, BestFrom) : std::make_pair(BestFrom, BestTo);
        return PeerVeins::Route(From, FromInflate, BestFrom, To, ToInflate, BestTo);
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
    std::map<std::string, std::array<int, PeerVeins::EDGE_COUNT>> Usage_;
    std::map<std::pair<std::string, std::string>, std::pair<int, int>> Chosen_;
};
