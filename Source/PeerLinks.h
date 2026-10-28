#pragma once
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "TranscriptScanner.h"

// Who is messaging whom between Claude sessions, built from the messages each session's transcript shows.
// Ends are this app's session ids, or "peer:<name>" for a Claude session running outside the app.
class PeerLinks
{
public:
    struct View
    {
        std::string FromId;
        std::string ToId;
        float Strength = 0;          // 1 while the pair is talking, fading to 0 after the last message
        std::vector<float> Packets;  // 0..1 progress of each recent message's pulse along the vein
        bool Waiting = false;        // the sender's last message has had no reply yet
    };

    static double WallNow()
    {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static bool IsExternal(const std::string& Id) { return Id.rfind(EXTERNAL_PREFIX, 0) == 0; }
    static std::string ExternalName(const std::string& Id) { return IsExternal(Id) == true ? Id.substr(std::string(EXTERNAL_PREFIX).size()) : Id; }

    // LocalByClaudeId maps each app session's Claude session id to its app session id.
    void Note(const std::string& LocalId, const std::vector<TranscriptScanner::PeerMessage>& Messages, const std::map<std::string, std::string>& LocalByClaudeId)
    {
        const double Now = WallNow();
        if (Now - RegistryReadAt_ > EXTERNAL_STATUS_REFRESH_SECONDS && WaitsOnExternal(Now) == true)
        {
            ReadRegistry(Now);
        }
        for (const TranscriptScanner::PeerMessage& Message : Messages)
        {
            const std::string Peer = Resolve(Message, LocalByClaudeId, Now);
            if (Peer.empty() == true || Peer == LocalId)
            {
                continue;
            }
            const std::pair<std::string, std::string> Key = Message.Outgoing == true ? std::make_pair(LocalId, Peer) : std::make_pair(Peer, LocalId);
            Link& Current = Links_[Key];
            Current.LastSentAt = std::max(Current.LastSentAt, Message.SentAt);
            RememberSent(Current, Message.SentAt);
            // Both ends' transcripts report the same message a moment apart; one pulse is enough.
            if (Now - static_cast<double>(Message.SentAt) > PACKET_FRESH_SECONDS || Now - Current.LastPacketAt < PACKET_MERGE_SECONDS)
            {
                continue;
            }
            Current.LastPacketAt = Now;
            Current.Packets.push_back(Now);
            while (Current.Packets.size() > MAX_PACKETS)
            {
                Current.Packets.pop_front();
            }
        }
        Prune(Now);
    }

    void Forget(const std::string& LocalId)
    {
        for (std::map<std::pair<std::string, std::string>, Link>::iterator Item = Links_.begin(); Item != Links_.end();)
        {
            if (Item->first.first == LocalId || Item->first.second == LocalId)
            {
                Item = Links_.erase(Item);
                continue;
            }
            ++Item;
        }
    }

    std::vector<View> Visible(const double Now) const
    {
        std::vector<View> Views;
        for (const std::pair<const std::pair<std::string, std::string>, Link>& Item : Links_)
        {
            View Current;
            Current.FromId = Item.first.first;
            Current.ToId = Item.first.second;
            const Link& Data = Item.second;
            const double Age = Now - static_cast<double>(Data.LastSentAt);
            Current.Strength = static_cast<float>(std::clamp(1.0 - (Age - TALK_HOLD_SECONDS) / TALK_FADE_SECONDS, 0.0, 1.0));
            for (const double StartedAt : Data.Packets)
            {
                const double Progress = (Now - StartedAt) / PACKET_SECONDS;
                if (Progress >= 0.0 && Progress < 1.0)
                {
                    Current.Packets.push_back(static_cast<float>(Progress));
                }
            }
            Current.Waiting = IsWaiting(Item.first, Data, Now);
            if (Current.Strength <= 0.0f && Current.Packets.empty() == true && Current.Waiting == false)
            {
                continue;
            }
            Views.push_back(Current);
        }
        return Views;
    }

    // Whether anything is pulsing right now, as opposed to only slowly crawling "waiting" lines.
    bool IsPulsing(const double Now) const
    {
        for (const std::pair<const std::pair<std::string, std::string>, Link>& Item : Links_)
        {
            if (Now - static_cast<double>(Item.second.LastSentAt) < TALK_HOLD_SECONDS + TALK_FADE_SECONDS)
            {
                return true;
            }
            if (Item.second.Packets.empty() == false && Now - Item.second.Packets.back() < PACKET_SECONDS)
            {
                return true;
            }
        }
        return false;
    }

    bool AnyWaiting(const double Now) const
    {
        for (const std::pair<const std::pair<std::string, std::string>, Link>& Item : Links_)
        {
            if (IsWaiting(Item.first, Item.second, Now) == true)
            {
                return true;
            }
        }
        return false;
    }

    // Claude reports each session's state in its registry; used for sessions running outside the app.
    bool IsExternalBusy(const std::string& Id) const
    {
        const std::map<std::string, Registered>::const_iterator Found = ByName_.find(ExternalName(Id));
        return Found != ByName_.end() && Found->second.Busy == true;
    }

private:
    static constexpr const char* EXTERNAL_PREFIX = "peer:";
    static constexpr double PACKET_SECONDS = 1.8;
    static constexpr double PACKET_FRESH_SECONDS = 20.0;
    static constexpr double PACKET_MERGE_SECONDS = 4.0;
    static constexpr size_t MAX_PACKETS = 4;
    static constexpr double TALK_HOLD_SECONDS = 30.0;
    static constexpr double TALK_FADE_SECONDS = 20.0;
    static constexpr double WAIT_TIMEOUT_SECONDS = 20.0 * 60.0;
    static constexpr double FORGET_AFTER_SECONDS = 2.0 * 3600.0;
    static constexpr double REGISTRY_REFRESH_SECONDS = 2.0;
    static constexpr double EXTERNAL_STATUS_REFRESH_SECONDS = 5.0;

    static constexpr std::time_t SAME_MESSAGE_SECONDS = 3;
    static constexpr size_t MAX_SENT_TIMES = 16;

    struct Link
    {
        std::time_t LastSentAt = 0;
        std::vector<std::time_t> SentTimes;  // recent sends, oldest first, for working out who still owes a reply
        double LastPacketAt = 0;
        std::deque<double> Packets;  // wall-clock start of each message's pulse
    };

    // Both ends' transcripts list the same message with timestamps a moment apart, so near-equal times are one send.
    static void RememberSent(Link& Current, const std::time_t SentAt)
    {
        for (const std::time_t Known : Current.SentTimes)
        {
            if (std::abs(static_cast<long long>(Known - SentAt)) <= SAME_MESSAGE_SECONDS)
            {
                return;
            }
        }
        Current.SentTimes.insert(std::upper_bound(Current.SentTimes.begin(), Current.SentTimes.end(), SentAt), SentAt);
        if (Current.SentTimes.size() > MAX_SENT_TIMES)
        {
            Current.SentTimes.erase(Current.SentTimes.begin());
        }
    }

    struct Registered
    {
        std::string ClaudeSessionId;
        double StartedAt = 0;
        bool Busy = false;
    };

    bool IsWaiting(const std::pair<std::string, std::string>& Key, const Link& Data, const double Now) const
    {
        if (Now - static_cast<double>(Data.LastSentAt) > WAIT_TIMEOUT_SECONDS)
        {
            return false;
        }
        const std::map<std::pair<std::string, std::string>, Link>::const_iterator Reverse = Links_.find(std::make_pair(Key.second, Key.first));
        if (Reverse == Links_.end())
        {
            return true;
        }
        // Replays the pair's messages in order: a message to someone who was waiting on you answers them and ends the
        // exchange; any other message starts a new wait. Without this, every answer would look like a new question.
        std::vector<std::pair<std::time_t, bool>> Events;  // (time, sent by Key.first)
        for (const std::time_t SentAt : Data.SentTimes)
        {
            Events.push_back({SentAt, true});
        }
        for (const std::time_t SentAt : Reverse->second.SentTimes)
        {
            Events.push_back({SentAt, false});
        }
        std::stable_sort(Events.begin(), Events.end(), [](const std::pair<std::time_t, bool>& Left, const std::pair<std::time_t, bool>& Right) { return Left.first < Right.first; });
        enum class Pending { Nobody, First, Second };
        Pending Waiter = Pending::Nobody;
        for (const std::pair<std::time_t, bool>& Event : Events)
        {
            const Pending Sender = Event.second == true ? Pending::First : Pending::Second;
            const bool Answers = Waiter != Pending::Nobody && Waiter != Sender;
            Waiter = Answers == true ? Pending::Nobody : Sender;
        }
        return Waiter == Pending::First;
    }

    bool WaitsOnExternal(const double Now) const
    {
        for (const std::pair<const std::pair<std::string, std::string>, Link>& Item : Links_)
        {
            if ((IsExternal(Item.first.first) == true || IsExternal(Item.first.second) == true) && IsWaiting(Item.first, Item.second, Now) == true)
            {
                return true;
            }
        }
        return false;
    }

    void Prune(const double Now)
    {
        for (std::map<std::pair<std::string, std::string>, Link>::iterator Item = Links_.begin(); Item != Links_.end();)
        {
            if (Now - static_cast<double>(Item->second.LastSentAt) > FORGET_AFTER_SECONDS)
            {
                Item = Links_.erase(Item);
                continue;
            }
            ++Item;
        }
    }

    // Outgoing messages name a session that must be in Claude's registry (anything else is a subagent or teammate,
    // not another session); incoming ones always come from a session, even one that has since closed.
    std::string Resolve(const TranscriptScanner::PeerMessage& Message, const std::map<std::string, std::string>& LocalByClaudeId, const double Now)
    {
        const Registered* Found = Lookup(Message);
        std::string Local = LocalFor(Found, LocalByClaudeId);
        if (Local.empty() == true && Now - RegistryReadAt_ > REGISTRY_REFRESH_SECONDS)
        {
            ReadRegistry(Now);
            Found = Lookup(Message);
            Local = LocalFor(Found, LocalByClaudeId);
        }
        if (Local.empty() == false)
        {
            return Local;
        }
        if (Message.PeerName.empty() == true || (Message.Outgoing == true && Found == nullptr))
        {
            return std::string();
        }
        return EXTERNAL_PREFIX + Message.PeerName;
    }

    static std::string LocalFor(const Registered* const Found, const std::map<std::string, std::string>& LocalByClaudeId)
    {
        if (Found == nullptr)
        {
            return std::string();
        }
        const std::map<std::string, std::string>::const_iterator Local = LocalByClaudeId.find(Found->ClaudeSessionId);
        return Local != LocalByClaudeId.end() ? Local->second : std::string();
    }

    const Registered* Lookup(const TranscriptScanner::PeerMessage& Message) const
    {
        if (Message.PeerAddress.empty() == false)
        {
            const std::map<std::string, Registered>::const_iterator Found = ByAddress_.find(Message.PeerAddress);
            if (Found != ByAddress_.end())
            {
                return &Found->second;
            }
        }
        if (Message.PeerName.empty() == false)
        {
            const std::map<std::string, Registered>::const_iterator Found = ByName_.find(Message.PeerName);
            if (Found != ByName_.end())
            {
                return &Found->second;
            }
        }
        return nullptr;
    }

    // ~/.claude/sessions/<pid>.json: one file per running Claude process with its session id, peer name and message pipe.
    void ReadRegistry(const double Now)
    {
        RegistryReadAt_ = Now;
        const std::filesystem::path Root = TranscriptScanner::ProjectsRoot();
        if (Root.empty() == true)
        {
            return;
        }
        std::error_code ErrorCode;
        std::filesystem::directory_iterator Iterator(Root.parent_path() / "sessions", ErrorCode);
        if (ErrorCode)
        {
            return;
        }
        ByName_.clear();
        ByAddress_.clear();
        for (const std::filesystem::directory_entry& Item : Iterator)
        {
            if (Item.path().extension() != ".json")
            {
                continue;
            }
            std::ifstream Stream(Item.path(), std::ios::binary);
            std::stringstream Buffer;
            Buffer << Stream.rdbuf();
            const nlohmann::json Entry = nlohmann::json::parse(Buffer.str(), nullptr, false);
            if (Entry.is_object() == false)
            {
                continue;
            }
            Registered Current;
            Current.ClaudeSessionId = Entry.value("sessionId", "");
            Current.StartedAt = Entry.value("startedAt", 0.0);
            Current.Busy = Entry.value("status", "") == "busy";
            if (Current.ClaudeSessionId.empty() == true)
            {
                continue;
            }
            Remember(ByName_, Entry.value("name", ""), Current);
            Remember(ByAddress_, Entry.value("messagingSocketPath", ""), Current);
        }
    }

    // Files of crashed processes linger, so the newest process wins a shared name.
    static void Remember(std::map<std::string, Registered>& Index, const std::string& Key, const Registered& Current)
    {
        if (Key.empty() == true)
        {
            return;
        }
        const std::map<std::string, Registered>::iterator Existing = Index.find(Key);
        if (Existing != Index.end() && Existing->second.StartedAt > Current.StartedAt)
        {
            return;
        }
        Index[Key] = Current;
    }

    std::map<std::pair<std::string, std::string>, Link> Links_;  // (sender, receiver) -> activity
    std::map<std::string, Registered> ByName_;
    std::map<std::string, Registered> ByAddress_;
    double RegistryReadAt_ = -1.0e9;
};
