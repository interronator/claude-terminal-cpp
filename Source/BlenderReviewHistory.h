#pragma once
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "BlenderReviewBatch.h"
#include "nlohmann/json.hpp"

// The reviews already sent for one session, kept on disk next to its screenshots, so a later review can say "round 3" and remind
// the receiver what was complained about before.
class BlenderReviewHistory
{
public:
    static constexpr size_t ROUNDS_SHOWN = 3;
    static constexpr size_t NOTES_SHOWN_CHARACTERS = 1500;

    struct Round
    {
        int Number = 0;
        std::time_t SentAt = 0;
        std::string Notes;  // every snapshot's numbered notes, in order
        std::vector<std::string> MarkedPaths;
    };

    void Load(const std::string& OwnDirectory, const std::string& Tag)
    {
        Rounds_.clear();
        Path_.clear();
        if (OwnDirectory.empty() == true || Tag.empty() == true)
        {
            return;
        }
        Path_ = (std::filesystem::path(OwnDirectory) / (Tag + "_review_history.json")).string();
        std::ifstream Stream(Path_, std::ios::binary);
        if (Stream.good() == false)
        {
            return;
        }
        const nlohmann::json Root = nlohmann::json::parse(Stream, nullptr, false);
        if (Root.is_array() == false)
        {
            return;
        }
        for (const nlohmann::json& Entry : Root)
        {
            if (Entry.is_object() == false)
            {
                continue;
            }
            Round Loaded;
            Loaded.Number = Entry.value("Number", 0);
            Loaded.SentAt = static_cast<std::time_t>(Entry.value("SentAt", static_cast<long long>(0)));
            Loaded.Notes = Entry.value("Notes", "");
            if (Entry.contains("MarkedPaths") == true && Entry["MarkedPaths"].is_array() == true)
            {
                for (const nlohmann::json& Path : Entry["MarkedPaths"])
                {
                    if (Path.is_string() == true)
                    {
                        Loaded.MarkedPaths.push_back(Path.get<std::string>());
                    }
                }
            }
            Rounds_.push_back(Loaded);
        }
    }

    int CurrentRound() const
    {
        return Rounds_.empty() == true ? 1 : Rounds_.back().Number + 1;
    }

    void Add(const std::vector<BlenderReviewBatch::Snapshot>& Snapshots)
    {
        Round Added;
        Added.Number = CurrentRound();
        Added.SentAt = std::time(nullptr);
        for (const BlenderReviewBatch::Snapshot& Item : Snapshots)
        {
            Added.Notes += Item.Notes;
            Added.MarkedPaths.push_back(Item.MarkedPath);
        }
        Rounds_.push_back(Added);
        Save();
    }

    // The "round N" sentence and the notes of the last few earlier rounds; empty for a first review.
    std::string Describe() const
    {
        std::string Text = "This is round " + std::to_string(CurrentRound()) + " of this review.\n";
        if (Rounds_.empty() == true)
        {
            return Text;
        }
        Text += "Earlier rounds were already sent. Check those problems are still fixed and say if any have come back (mark numbers restart each round):\n";
        const size_t First = Rounds_.size() > ROUNDS_SHOWN ? Rounds_.size() - ROUNDS_SHOWN : 0;
        for (size_t Index = First; Index < Rounds_.size(); Index++)
        {
            const Round& Past = Rounds_[Index];
            std::string Notes = Past.Notes;
            if (Notes.size() > NOTES_SHOWN_CHARACTERS)
            {
                Notes = Notes.substr(0, NOTES_SHOWN_CHARACTERS) + "...\n";
            }
            Text += "Round " + std::to_string(Past.Number) + " (" + TimeText(Past.SentAt) + "):\n" + Notes;
        }
        return Text;
    }

private:
    std::string Path_;
    std::vector<Round> Rounds_;

    static std::string TimeText(const std::time_t Moment)
    {
        std::tm Local{};
#ifdef _WIN32
        localtime_s(&Local, &Moment);
#else
        localtime_r(&Moment, &Local);
#endif
        char Buffer[32];
        std::strftime(Buffer, sizeof Buffer, "%Y-%m-%d %H:%M", &Local);
        return Buffer;
    }

    void Save() const
    {
        if (Path_.empty() == true)
        {
            return;
        }
        nlohmann::json Root = nlohmann::json::array();
        for (const Round& Past : Rounds_)
        {
            nlohmann::json Entry;
            Entry["Number"] = Past.Number;
            Entry["SentAt"] = static_cast<long long>(Past.SentAt);
            Entry["Notes"] = Past.Notes;
            Entry["MarkedPaths"] = Past.MarkedPaths;
            Root.push_back(Entry);
        }
        std::error_code ErrorCode;
        std::filesystem::create_directories(std::filesystem::path(Path_).parent_path(), ErrorCode);
        std::ofstream Stream(Path_, std::ios::binary | std::ios::trunc);
        Stream << Root.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    }
};
