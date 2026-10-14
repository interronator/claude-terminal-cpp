#pragma once
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "screenshot_finder.h"

// Tails one Claude Code session transcript (~/.claude/projects/<project>/<session id>.jsonl) and reports
// image files the session created or changed while working (not ones it merely mentioned), once they exist on disk.
class TranscriptScanner
{
public:
    struct Shot
    {
        std::string Path;
        std::time_t ModifiedAt = 0;
    };

    std::vector<Shot> Poll(const std::string& ClaudeSessionId, const std::string& Cwd, const double Now)
    {
        std::vector<Shot> Shots;
        if (ClaudeSessionId.empty() == true)
        {
            return Shots;
        }
        if (TranscriptPath_.empty() == true)
        {
            if (Now < NextSearchAt_)
            {
                return Shots;
            }
            NextSearchAt_ = Now + SEARCH_INTERVAL_SECONDS;
            TranscriptPath_ = Locate(ClaudeSessionId);
            if (TranscriptPath_.empty() == true)
            {
                return Shots;
            }
        }

        ReadNewLines(Cwd, Now);
        ResolvePending(Cwd, Now, Shots);
        ScanShotDirectories(Cwd, Shots);
        return Shots;
    }

private:
    struct Candidate
    {
        std::string Path;
        double SeenAt = 0;
        std::time_t NotOlderThan = 0;
    };

    static constexpr double SEARCH_INTERVAL_SECONDS = 5.0;
    static constexpr double PENDING_TIMEOUT_SECONDS = 120.0;
    static constexpr std::time_t MODIFIED_SLACK_SECONDS = 5;
    static constexpr std::time_t SHOT_DIRECTORY_LOOKBACK_SECONDS = 600;
    static constexpr std::uintmax_t MAX_INITIAL_BYTES = 6ull * 1024 * 1024;

    static std::filesystem::path Locate(const std::string& ClaudeSessionId)
    {
        std::filesystem::path Root;
        const char* const ConfigDir = std::getenv("CLAUDE_CONFIG_DIR");
        const char* const Home = std::getenv("USERPROFILE") != nullptr ? std::getenv("USERPROFILE") : std::getenv("HOME");
        if (ConfigDir != nullptr)
        {
            Root = ConfigDir;
        }
        else if (Home != nullptr)
        {
            Root = std::filesystem::path(Home) / ".claude";
        }
        else
        {
            return {};
        }
        Root /= "projects";

        std::error_code ErrorCode;
        for (const std::filesystem::directory_entry& Directory : std::filesystem::directory_iterator(Root, ErrorCode))
        {
            const std::filesystem::path Candidate = Directory.path() / (ClaudeSessionId + ".jsonl");
            if (std::filesystem::is_regular_file(Candidate, ErrorCode) == true)
            {
                return Candidate;
            }
        }
        return {};
    }

    // Parses "2026-10-11T12:34:56.789Z" (UTC) into epoch seconds, or 0 when it doesn't match.
    static std::time_t ParseTimestamp(const std::string& Text)
    {
        int Year = 0;
        int Month = 0;
        int Day = 0;
        int Hour = 0;
        int Minute = 0;
        int Second = 0;
        if (std::sscanf(Text.c_str(), "%d-%d-%dT%d:%d:%d", &Year, &Month, &Day, &Hour, &Minute, &Second) != 6)
        {
            return 0;
        }
        const int AdjustedYear = Month <= 2 ? Year - 1 : Year;
        const int Era = (AdjustedYear >= 0 ? AdjustedYear : AdjustedYear - 399) / 400;
        const int YearOfEra = AdjustedYear - Era * 400;
        const int DayOfYear = (153 * (Month + (Month > 2 ? -3 : 9)) + 2) / 5 + Day - 1;
        const int DayOfEra = YearOfEra * 365 + YearOfEra / 4 - YearOfEra / 100 + DayOfYear;
        const long long DaysSinceEpoch = static_cast<long long>(Era) * 146097 + DayOfEra - 719468;
        return static_cast<std::time_t>(DaysSinceEpoch * 86400 + Hour * 3600 + Minute * 60 + Second);
    }

    // Skips image blocks (base64) and Read calls (those view existing files rather than produce screenshots).
    static void CollectStrings(const nlohmann::json& Node, std::vector<std::string>& Strings)
    {
        if (Node.is_string() == true)
        {
            Strings.push_back(Node.get<std::string>());
            return;
        }
        if (Node.is_array() == true)
        {
            for (const nlohmann::json& Child : Node)
            {
                CollectStrings(Child, Strings);
            }
            return;
        }
        if (Node.is_object() == false)
        {
            return;
        }
        const std::string Type = Node.value("type", "");
        if (Type == "image" || Type == "thinking" || (Type == "tool_use" && Node.value("name", "") == "Read"))
        {
            return;
        }
        for (const auto& Field : Node.items())
        {
            CollectStrings(Field.value(), Strings);
        }
    }

    void ReadNewLines(const std::string& Cwd, const double Now)
    {
        std::error_code ErrorCode;
        const std::uintmax_t Size = std::filesystem::file_size(TranscriptPath_, ErrorCode);
        if (ErrorCode)
        {
            return;
        }
        if (Size < Offset_)
        {
            Offset_ = 0;
            Carry_.clear();
        }
        bool DropFirstLine = false;
        if (Started_ == false)
        {
            Started_ = true;
            if (Size > MAX_INITIAL_BYTES)
            {
                Offset_ = Size - MAX_INITIAL_BYTES;
                DropFirstLine = true;
            }
        }
        if (Size == Offset_)
        {
            return;
        }

        std::ifstream Stream(TranscriptPath_, std::ios::binary);
        if (Stream.is_open() == false)
        {
            return;
        }
        Stream.seekg(static_cast<std::streamoff>(Offset_));
        std::string Chunk(static_cast<size_t>(Size - Offset_), '\0');
        Stream.read(Chunk.data(), static_cast<std::streamsize>(Chunk.size()));
        Chunk.resize(static_cast<size_t>(Stream.gcount()));
        Offset_ += Chunk.size();

        std::string Data = Carry_ + Chunk;
        const size_t LastNewline = Data.rfind('\n');
        if (LastNewline == std::string::npos)
        {
            Carry_ = Data;
            return;
        }
        Carry_ = Data.substr(LastNewline + 1);
        Data.resize(LastNewline);

        size_t LineStart = 0;
        while (LineStart <= Data.size())
        {
            size_t LineEnd = Data.find('\n', LineStart);
            if (LineEnd == std::string::npos)
            {
                LineEnd = Data.size();
            }
            const std::string Line = Data.substr(LineStart, LineEnd - LineStart);
            LineStart = LineEnd + 1;
            if (DropFirstLine == true)
            {
                DropFirstLine = false;
                continue;
            }
            AddCandidates(Line, Now);
        }
    }

    void AddCandidates(const std::string& Line, const double Now)
    {
        if (Line.empty() == true)
        {
            return;
        }
        const nlohmann::json Entry = nlohmann::json::parse(Line, nullptr, false);
        if (Entry.is_object() == false || Entry.contains("message") == false)
        {
            return;
        }
        const std::time_t EntryTime = ParseTimestamp(Entry.value("timestamp", ""));
        if (EntryTime == 0)
        {
            return;
        }
        const nlohmann::json& Content = Entry["message"].contains("content") == true ? Entry["message"]["content"] : nlohmann::json();
        if (Content.is_string() == true)
        {
            WatchShotDirectories(Content.get<std::string>());
            return;
        }
        if (Content.is_array() == false)
        {
            return;
        }
        const bool FromUser = Entry["message"].value("role", "") == "user";
        std::vector<std::string> Strings;
        for (const nlohmann::json& Block : Content)
        {
            if (FromUser == true && Block.is_object() == true && Block.value("type", "") == "text")
            {
                WatchShotDirectories(Block.value("text", ""));
                continue;
            }
            CollectStrings(Block, Strings);
        }
        for (const std::string& Text : Strings)
        {
            WatchShotDirectories(Text);
            for (const std::string& Path : ScreenshotFinder::ExtractImagePaths(Text))
            {
                Pending_.push_back({Path, Now, EntryTime});
            }
        }
    }

    void WatchShotDirectories(const std::string& Text)
    {
        for (const std::string& Directory : ScreenshotFinder::ExtractShotDirectories(Text))
        {
            WatchedDirectories_.insert(Directory);
        }
    }

    // Reports every image in a ClaudeShots directory (the session's own, or one named in the transcript)
    // that was written since this scanner started, whether or not the transcript names the file.
    void ScanShotDirectories(const std::string& Cwd, std::vector<Shot>& Shots)
    {
        if (ScanSince_ == 0)
        {
            ScanSince_ = std::time(nullptr) - SHOT_DIRECTORY_LOOKBACK_SECONDS;
        }
        static const std::vector<std::string> EXTENSIONS = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"};

        std::set<std::filesystem::path> Directories;
        Directories.insert((std::filesystem::path(Cwd) / ScreenshotFinder::SHOT_DIRECTORY_NAME).lexically_normal());
        for (const std::string& Watched : WatchedDirectories_)
        {
            std::filesystem::path Path(Watched);
            if (Path.is_relative() == true)
            {
                Path = std::filesystem::path(Cwd) / Path;
            }
            Directories.insert(Path.lexically_normal());
        }

        for (const std::filesystem::path& Directory : Directories)
        {
            std::error_code ErrorCode;
            std::filesystem::directory_iterator Iterator(Directory, ErrorCode);
            if (ErrorCode)
            {
                continue;
            }
            for (const std::filesystem::directory_entry& Item : Iterator)
            {
                std::string Extension = Item.path().extension().string();
                std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](const unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
                if (std::find(EXTENSIONS.begin(), EXTENSIONS.end(), Extension) == EXTENSIONS.end())
                {
                    continue;
                }
                std::error_code TimeError;
                const std::filesystem::file_time_type FileTime = Item.last_write_time(TimeError);
                if (TimeError)
                {
                    continue;
                }
                const std::time_t ModifiedAt = std::time(nullptr) - static_cast<std::time_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                                        std::filesystem::file_time_type::clock::now() - FileTime).count());
                if (ModifiedAt < ScanSince_)
                {
                    continue;
                }
                const std::string Resolved = Item.path().lexically_normal().string();
                const std::map<std::string, std::time_t>::iterator Emitted = Emitted_.find(Resolved);
                if (Emitted != Emitted_.end() && Emitted->second == ModifiedAt)
                {
                    continue;
                }
                Emitted_[Resolved] = ModifiedAt;
                Shots.push_back({Resolved, ModifiedAt});
            }
        }
    }

    void ResolvePending(const std::string& Cwd, const double Now, std::vector<Shot>& Shots)
    {
        std::vector<Candidate> Remaining;
        for (const Candidate& Item : Pending_)
        {
            const std::string Resolved = ScreenshotFinder::Resolve(Item.Path, Cwd);
            if (Resolved.empty() == true)
            {
                if (Now - Item.SeenAt < PENDING_TIMEOUT_SECONDS)
                {
                    Remaining.push_back(Item);
                }
                continue;
            }

            std::error_code ErrorCode;
            const std::filesystem::file_time_type FileTime = std::filesystem::last_write_time(Resolved, ErrorCode);
            if (ErrorCode)
            {
                continue;
            }
            const std::time_t ModifiedAt = std::time(nullptr) - static_cast<std::time_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                                    std::filesystem::file_time_type::clock::now() - FileTime).count());
            const std::map<std::string, std::time_t>::iterator Emitted = Emitted_.find(Resolved);
            if (Emitted != Emitted_.end() && Emitted->second == ModifiedAt)
            {
                continue;
            }
            if (ModifiedAt + MODIFIED_SLACK_SECONDS < Item.NotOlderThan)
            {
                if (Now - Item.SeenAt < PENDING_TIMEOUT_SECONDS)
                {
                    Remaining.push_back(Item);
                }
                continue;
            }
            Emitted_[Resolved] = ModifiedAt;
            Shots.push_back({Resolved, ModifiedAt});
        }
        Pending_ = std::move(Remaining);
    }

    std::filesystem::path TranscriptPath_;
    double NextSearchAt_ = 0;
    std::uintmax_t Offset_ = 0;
    bool Started_ = false;
    std::string Carry_;
    std::vector<Candidate> Pending_;
    std::map<std::string, std::time_t> Emitted_;
    std::set<std::string> WatchedDirectories_;
    std::time_t ScanSince_ = 0;
};
