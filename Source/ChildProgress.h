#pragma once
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <string>
#include <vector>

#include "SessionRequests.h"

// Progress that started (child) sessions report by writing "<tag>.progress" into the Progress folder:
// first line a number 0-100, second line a short note about what they are doing.
class ChildProgress
{
public:
    struct Entry
    {
        int Percent = -1;
        std::string Note;
        std::filesystem::file_time_type Stamp{};
    };

    static std::filesystem::path Dir()
    {
        return SessionRequests::InboxDir() / "Progress";
    }

    static void EnsureDir()
    {
        std::error_code ErrorCode;
        std::filesystem::create_directories(Dir(), ErrorCode);
    }

    static std::string FilePathFor(const std::string& Tag)
    {
        return (Dir() / (Tag + ".progress")).string();
    }

    static std::string Guidance(const std::string& Tag)
    {
        return "Report your progress to the session that started you, so it shows as a bar in its window: whenever you reach a milestone (at least "
               "every few minutes of work), overwrite the file " + FilePathFor(Tag) + " with two lines: first line a whole number from 0 to 100 "
               "(your honest estimate of how much of the task is done), second line a short plain note on what you are doing now. Write 100 when "
               "finished. If the session that started you asks for a progress update, refresh that file and answer it with SendMessage.";
    }

    static std::string CheckInPrompt(const std::string& ParentTag)
    {
        return "Progress check-in from the session that started you (tag " + ParentTag + "): please refresh your progress file now, and reply to it "
               "with SendMessage in one or two sentences: what is done, what is next, and whether you are blocked.";
    }

    // Reads every progress file that changed since the last call and returns the tags whose entry changed.
    std::vector<std::string> Poll()
    {
        std::vector<std::string> Changed;
        const std::chrono::steady_clock::time_point Now = std::chrono::steady_clock::now();
        if (Now - LastPoll_ < std::chrono::seconds(1))
        {
            return Changed;
        }
        LastPoll_ = Now;
        std::error_code ErrorCode;
        std::filesystem::directory_iterator Iterator(Dir(), ErrorCode);
        if (ErrorCode != std::error_code())
        {
            return Changed;
        }
        std::set<std::string> Seen;
        for (const std::filesystem::directory_entry& File : Iterator)
        {
            if (File.is_regular_file(ErrorCode) == false || File.path().extension() != ".progress")
            {
                continue;
            }
            const std::string Tag = File.path().stem().string();
            Seen.insert(Tag);
            const std::filesystem::file_time_type Stamp = File.last_write_time(ErrorCode);
            const std::map<std::string, Entry>::const_iterator Known = Entries_.find(Tag);
            if (ErrorCode != std::error_code() || (Known != Entries_.end() && Known->second.Stamp == Stamp))
            {
                continue;
            }
            Entry Parsed = Read(File.path());
            Parsed.Stamp = Stamp;
            if (Parsed.Percent < 0)
            {
                continue;
            }
            Entries_[Tag] = Parsed;
            if (Primed_ == true)
            {
                Changed.push_back(Tag);
            }
        }
        Primed_ = true;
        for (std::map<std::string, Entry>::iterator Existing = Entries_.begin(); Existing != Entries_.end();)
        {
            if (Seen.count(Existing->first) == 0)
            {
                Existing = Entries_.erase(Existing);
                continue;
            }
            ++Existing;
        }
        return Changed;
    }

    void Forget(const std::string& Tag)
    {
        Entries_.erase(Tag);
        std::error_code ErrorCode;
        std::filesystem::remove(FilePathFor(Tag), ErrorCode);
    }

    const Entry* Find(const std::string& Tag) const
    {
        const std::map<std::string, Entry>::const_iterator Found = Entries_.find(Tag);
        return Found == Entries_.end() ? nullptr : &Found->second;
    }

private:
    static Entry Read(const std::filesystem::path& Path)
    {
        Entry Parsed;
        std::ifstream Stream(Path, std::ios::binary);
        std::string FirstLine;
        std::getline(Stream, FirstLine);
        std::getline(Stream, Parsed.Note);
        while (Parsed.Note.empty() == false && (Parsed.Note.back() == '\r' || Parsed.Note.back() == '\n'))
        {
            Parsed.Note.pop_back();
        }
        size_t Start = 0;
        while (Start < FirstLine.size() && (FirstLine[Start] < '0' || FirstLine[Start] > '9'))
        {
            Start++;
        }
        if (Start == FirstLine.size())
        {
            return Parsed;
        }
        int Value = 0;
        for (size_t Index = Start; Index < FirstLine.size() && FirstLine[Index] >= '0' && FirstLine[Index] <= '9'; Index++)
        {
            Value = std::min(1000, Value * 10 + (FirstLine[Index] - '0'));
        }
        Parsed.Percent = std::min(Value, 100);
        return Parsed;
    }

    std::map<std::string, Entry> Entries_;
    std::chrono::steady_clock::time_point LastPoll_{};
    bool Primed_ = false;
};
