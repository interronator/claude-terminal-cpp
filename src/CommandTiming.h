#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

// Learns how long shell commands usually take from every Claude Code transcript on disk, so a running command can show
// "usually ~3m". Scanning runs on its own thread, only reads what was appended since the last pass (offsets are cached in
// a small JSON file next to the app's state), and repeats every few minutes so new runs are learned while the app is open.
class CommandTiming
{
public:
    CommandTiming() = default;
    CommandTiming(const CommandTiming&) = delete;
    CommandTiming& operator=(const CommandTiming&) = delete;

    ~CommandTiming()
    {
        {
            std::lock_guard<std::mutex> const Guard(WakeMutex_);
            Stop_ = true;
        }
        Wake_.notify_all();
        if (Worker_.joinable() == true)
        {
            Worker_.join();
        }
    }

    void Start(const std::filesystem::path& CachePath, const std::filesystem::path& ProjectsRoot)
    {
        if (Worker_.joinable() == true)
        {
            return;
        }
        CachePath_ = CachePath;
        ProjectsRoot_ = ProjectsRoot;
        LoadCache();
        Worker_ = std::thread([this] { Run(); });
    }

    // Typical duration in seconds for a command key, or -1 when it has not finished at least twice before.
    double Typical(const std::string& Key) const
    {
        constexpr size_t MIN_SAMPLES = 2;
        if (Key.empty() == true)
        {
            return -1;
        }
        std::lock_guard<std::mutex> const Guard(Mutex_);
        const std::map<std::string, std::deque<double>>::const_iterator Found = Samples_.find(Key);
        if (Found == Samples_.end() || Found->second.size() < MIN_SAMPLES)
        {
            return -1;
        }
        std::vector<double> Sorted(Found->second.begin(), Found->second.end());
        std::sort(Sorted.begin(), Sorted.end());
        return Sorted[Sorted.size() / 2];
    }

    // Groups runs of "the same" command: the script or program name plus a few short arguments after it, so
    // "RunPlayCheck.ps1 -Map /Game/A" and "... /Game/B" match but "RamCheck.ps1 -Need CppBuild -Wait" differs from
    // "RamCheck.ps1 -Release".
    static std::string KeyFor(const std::string& Command)
    {
        constexpr size_t MAX_ARGUMENTS = 3;
        constexpr size_t MAX_ARGUMENT_LENGTH = 24;
        static const std::vector<std::string> SCRIPT_EXTENSIONS = {".ps1", ".py", ".bat", ".cmd", ".sh", ".exe", ".js", ".mjs"};
        static const std::vector<std::string> LAUNCHERS = {"powershell", "pwsh", "cmd", "python", "python3", "py", "bash", "sh", "node", "call", "start", "&", "."};

        const std::vector<std::string> Tokens = Tokenize(Command);
        size_t ProgramIndex = Tokens.size();
        for (size_t Index = 0; Index < Tokens.size(); Index++)
        {
            const std::string Lower = ToLower(Tokens[Index]);
            const bool IsScript = std::any_of(SCRIPT_EXTENSIONS.begin(), SCRIPT_EXTENSIONS.end(), [&Lower](const std::string& Extension) { return EndsWith(Lower, Extension); });
            if (IsScript == true && EndsWith(Lower, ".exe") == false)
            {
                ProgramIndex = Index;
                break;
            }
        }
        if (ProgramIndex == Tokens.size())
        {
            for (size_t Index = 0; Index < Tokens.size(); Index++)
            {
                const std::string Base = ToLower(BaseName(Tokens[Index]));
                const std::string Bare = EndsWith(Base, ".exe") == true ? Base.substr(0, Base.size() - 4) : Base;
                if (Bare.empty() == true || Bare[0] == '-' || Bare[0] == '/' || std::find(LAUNCHERS.begin(), LAUNCHERS.end(), Bare) != LAUNCHERS.end())
                {
                    continue;
                }
                ProgramIndex = Index;
                break;
            }
        }
        if (ProgramIndex == Tokens.size())
        {
            return {};
        }

        std::string Key = ToLower(BaseName(Tokens[ProgramIndex]));
        for (size_t Index = ProgramIndex + 1; Index < Tokens.size() && Index <= ProgramIndex + MAX_ARGUMENTS; Index++)
        {
            const std::string& Argument = Tokens[Index];
            if (Argument.size() > MAX_ARGUMENT_LENGTH || Argument.find_first_of("/\\:.") != std::string::npos)
            {
                break;
            }
            Key += " " + ToLower(Argument);
        }
        return Key;
    }

    // The last non-empty line a background command has printed, with terminal color codes and carriage-return
    // overwrites (progress counters) resolved, or empty.
    static std::string LastOutputLine(const std::string& Path)
    {
        constexpr std::uintmax_t TAIL_BYTES = 4096;
        std::error_code ErrorCode;
        const std::uintmax_t Size = std::filesystem::file_size(Path, ErrorCode);
        if (ErrorCode || Size == 0)
        {
            return {};
        }
        std::ifstream Stream(Path, std::ios::binary);
        if (Stream.is_open() == false)
        {
            return {};
        }
        const std::uintmax_t Start = Size > TAIL_BYTES ? Size - TAIL_BYTES : 0;
        Stream.seekg(static_cast<std::streamoff>(Start));
        std::string Tail(static_cast<size_t>(Size - Start), '\0');
        Stream.read(Tail.data(), static_cast<std::streamsize>(Tail.size()));
        Tail.resize(static_cast<size_t>(Stream.gcount()));

        size_t End = Tail.size();
        while (End > 0)
        {
            const size_t LineStart = Tail.rfind('\n', End - 1);
            const size_t Begin = LineStart == std::string::npos ? 0 : LineStart + 1;
            std::string Line = Tail.substr(Begin, End - Begin);
            const size_t LastReturn = Line.find_last_of('\r', Line.size() >= 2 ? Line.size() - 2 : 0);
            if (LastReturn != std::string::npos && LastReturn + 1 < Line.size())
            {
                Line = Line.substr(LastReturn + 1);
            }
            Line = StripControl(Line);
            if (Line.find_first_not_of(" \t") != std::string::npos)
            {
                return Line;
            }
            if (LineStart == std::string::npos)
            {
                break;
            }
            End = LineStart;
        }
        return {};
    }

private:
    static constexpr size_t MAX_SAMPLES_PER_KEY = 30;

    // Drops ANSI escape sequences and other control characters.
    static std::string StripControl(const std::string& Text)
    {
        std::string Clean;
        for (size_t Index = 0; Index < Text.size(); Index++)
        {
            const unsigned char Character = static_cast<unsigned char>(Text[Index]);
            if (Character == 0x1B)
            {
                Index++;
                if (Index < Text.size() && Text[Index] == '[')
                {
                    while (Index + 1 < Text.size() && (Text[Index + 1] < '@' || Text[Index + 1] > '~'))
                    {
                        Index++;
                    }
                    Index++;
                }
                continue;
            }
            if (Character < 0x20 && Character != '\t')
            {
                continue;
            }
            Clean += static_cast<char>(Character);
        }
        return Clean;
    }
    static constexpr int RESCAN_SECONDS = 180;

    struct PendingStart
    {
        std::string Key;
        std::time_t StartedAt = 0;
    };

    struct FileState
    {
        std::uintmax_t Offset = 0;
        std::map<std::string, PendingStart> Pending;      // tool_use id -> start
        std::map<std::string, PendingStart> PendingTasks;  // background task id -> start
    };

    static bool EndsWith(const std::string& Text, const std::string& Suffix)
    {
        return Text.size() >= Suffix.size() && Text.compare(Text.size() - Suffix.size(), Suffix.size(), Suffix) == 0;
    }

    static std::string ToLower(std::string Text)
    {
        std::transform(Text.begin(), Text.end(), Text.begin(), [](const unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
        return Text;
    }

    static std::string BaseName(const std::string& Path)
    {
        const size_t Slash = Path.find_last_of("/\\");
        return Slash == std::string::npos ? Path : Path.substr(Slash + 1);
    }

    // Splits on whitespace, quotes and shell operators; a token ends at ; | & ( ) so "CppBuild;" yields "CppBuild".
    static std::vector<std::string> Tokenize(const std::string& Command)
    {
        std::vector<std::string> Tokens;
        std::string Current;
        for (const char Character : Command)
        {
            const bool Separator = std::isspace(static_cast<unsigned char>(Character)) != 0 || std::string("\"'`;|&(){}<>,").find(Character) != std::string::npos;
            if (Separator == false)
            {
                Current += Character;
                continue;
            }
            if (Current.empty() == false)
            {
                Tokens.push_back(Current);
                Current.clear();
            }
        }
        if (Current.empty() == false)
        {
            Tokens.push_back(Current);
        }
        return Tokens;
    }

    // Parses "2026-10-11T12:34:56.789Z" (UTC) into epoch seconds, or 0.
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

    // The quoted value following Marker (e.g. "timestamp":"), read without parsing the whole line as JSON.
    static std::string ValueAfter(const std::string& Line, const std::string& Marker, size_t From = 0)
    {
        const size_t At = Line.find(Marker, From);
        if (At == std::string::npos)
        {
            return {};
        }
        const size_t Begin = At + Marker.size();
        const size_t End = Line.find('"', Begin);
        return End == std::string::npos ? std::string() : Line.substr(Begin, End - Begin);
    }

    void AddSample(const std::string& Key, const double Seconds)
    {
        if (Key.empty() == true || Seconds < 0)
        {
            return;
        }
        std::lock_guard<std::mutex> const Guard(Mutex_);
        std::deque<double>& Samples = Samples_[Key];
        Samples.push_back(Seconds);
        while (Samples.size() > MAX_SAMPLES_PER_KEY)
        {
            Samples.pop_front();
        }
    }

    void NoteStarts(const std::string& Line, FileState& State)
    {
        const nlohmann::json Entry = nlohmann::json::parse(Line, nullptr, false);
        if (Entry.is_object() == false || Entry.contains("message") == false || Entry["message"].is_object() == false)
        {
            return;
        }
        const nlohmann::json& Content = Entry["message"].contains("content") == true ? Entry["message"]["content"] : nlohmann::json();
        if (Content.is_array() == false)
        {
            return;
        }
        const std::time_t When = ParseTimestamp(Entry.value("timestamp", ""));
        for (const nlohmann::json& Block : Content)
        {
            if (Block.is_object() == false || Block.value("type", "") != "tool_use")
            {
                continue;
            }
            const std::string Name = Block.value("name", "");
            if ((Name != "Bash" && Name != "PowerShell") || Block.contains("input") == false || Block["input"].is_object() == false)
            {
                continue;
            }
            State.Pending[Block.value("id", "")] = {KeyFor(Block["input"].value("command", "")), When};
        }
    }

    void NoteResults(const std::string& Line, FileState& State)
    {
        const std::string IdMarker = "\"tool_use_id\":\"";
        const std::time_t When = ParseTimestamp(ValueAfter(Line, "\"timestamp\":\""));
        size_t Position = 0;
        while ((Position = Line.find(IdMarker, Position)) != std::string::npos)
        {
            const std::string Id = ValueAfter(Line, IdMarker, Position);
            Position += IdMarker.size();
            const std::map<std::string, PendingStart>::iterator Found = State.Pending.find(Id);
            if (Found == State.Pending.end())
            {
                continue;
            }
            const std::string BackgroundMarker = "Command running in background with ID: ";
            const size_t BackgroundAt = Line.find(BackgroundMarker);
            if (BackgroundAt != std::string::npos)
            {
                const size_t TaskStart = BackgroundAt + BackgroundMarker.size();
                const std::string TaskId = Line.substr(TaskStart, Line.find_first_of(". \\\"", TaskStart) - TaskStart);
                State.PendingTasks[TaskId] = Found->second;
            }
            else if (When != 0 && Found->second.StartedAt != 0 && Line.find("doesn't want to proceed") == std::string::npos &&
                     Line.find("Interrupted by user") == std::string::npos)
            {
                AddSample(Found->second.Key, static_cast<double>(When - Found->second.StartedAt));
            }
            State.Pending.erase(Found);
        }
    }

    void NoteNotifications(const std::string& Line, FileState& State)
    {
        const std::string IdMarker = "<task-id>";
        const std::time_t When = ParseTimestamp(ValueAfter(Line, "\"timestamp\":\""));
        size_t Position = 0;
        while ((Position = Line.find(IdMarker, Position)) != std::string::npos)
        {
            const size_t IdStart = Position + IdMarker.size();
            Position = IdStart;
            const std::string TaskId = Line.substr(IdStart, Line.find('<', IdStart) - IdStart);
            const std::map<std::string, PendingStart>::iterator Found = State.PendingTasks.find(TaskId);
            if (Found == State.PendingTasks.end())
            {
                continue;
            }
            const size_t StatusAt = Line.find("<status>", IdStart);
            if (StatusAt == std::string::npos)
            {
                continue;
            }
            const bool Finished = Line.compare(StatusAt, 19, "<status>completed</") == 0 || Line.compare(StatusAt, 16, "<status>failed</") == 0;
            if (Finished == true && When != 0 && Found->second.StartedAt != 0)
            {
                AddSample(Found->second.Key, static_cast<double>(When - Found->second.StartedAt));
            }
            State.PendingTasks.erase(Found);
        }
    }

    void ScanFile(const std::filesystem::path& Path, FileState& State)
    {
        std::error_code ErrorCode;
        const std::uintmax_t Size = std::filesystem::file_size(Path, ErrorCode);
        if (ErrorCode || Size == State.Offset)
        {
            return;
        }
        if (Size < State.Offset)
        {
            State = FileState{};
        }
        std::ifstream Stream(Path, std::ios::binary);
        if (Stream.is_open() == false)
        {
            return;
        }
        Stream.seekg(static_cast<std::streamoff>(State.Offset));
        std::string Line;
        std::uintmax_t Consumed = State.Offset;
        while (Stop_ == false && std::getline(Stream, Line).fail() == false)
        {
            if (Stream.eof() == true)
            {
                break;  // a line still being written; read it whole on the next pass
            }
            Consumed += Line.size() + 1;
            const bool MayStart = Line.find("\"tool_use\"") != std::string::npos &&
                                  (Line.find("\"name\":\"Bash\"") != std::string::npos || Line.find("\"name\":\"PowerShell\"") != std::string::npos);
            if (MayStart == true)
            {
                NoteStarts(Line, State);
            }
            if (State.Pending.empty() == false && Line.find("\"tool_use_id\":\"") != std::string::npos)
            {
                NoteResults(Line, State);
            }
            if (State.PendingTasks.empty() == false && Line.find("<task-id>") != std::string::npos)
            {
                NoteNotifications(Line, State);
            }
        }
        State.Offset = Consumed;
    }

    void ScanAll()
    {
        std::error_code ErrorCode;
        std::filesystem::recursive_directory_iterator Iterator(ProjectsRoot_, ErrorCode);
        if (ErrorCode)
        {
            return;
        }
        for (const std::filesystem::directory_entry& Item : Iterator)
        {
            if (Stop_ == true)
            {
                return;
            }
            if (Item.path().extension() != ".jsonl" || Item.is_regular_file(ErrorCode) == false)
            {
                continue;
            }
            ScanFile(Item.path(), Files_[Item.path().string()]);
        }
    }

    void Run()
    {
        while (Stop_ == false)
        {
            ScanAll();
            SaveCache();
            std::unique_lock<std::mutex> Lock(WakeMutex_);
            Wake_.wait_for(Lock, std::chrono::seconds(RESCAN_SECONDS), [this] { return Stop_ == true; });
        }
    }

    void LoadCache()
    {
        std::ifstream Stream(CachePath_, std::ios::binary);
        if (Stream.is_open() == false)
        {
            return;
        }
        const nlohmann::json Root = nlohmann::json::parse(Stream, nullptr, false);
        if (Root.is_object() == false)
        {
            return;
        }
        if (Root.contains("files") == true && Root["files"].is_object() == true)
        {
            for (const auto& Item : Root["files"].items())
            {
                Files_[Item.key()].Offset = Item.value().get<std::uintmax_t>();
            }
        }
        if (Root.contains("samples") == true && Root["samples"].is_object() == true)
        {
            std::lock_guard<std::mutex> const Guard(Mutex_);
            for (const auto& Item : Root["samples"].items())
            {
                for (const nlohmann::json& Value : Item.value())
                {
                    Samples_[Item.key()].push_back(Value.get<double>());
                }
            }
        }
    }

    void SaveCache()
    {
        nlohmann::json Root;
        Root["version"] = 1;
        nlohmann::json Files = nlohmann::json::object();
        for (const std::pair<const std::string, FileState>& Item : Files_)
        {
            Files[Item.first] = Item.second.Offset;
        }
        Root["files"] = Files;
        nlohmann::json Samples = nlohmann::json::object();
        {
            std::lock_guard<std::mutex> const Guard(Mutex_);
            for (const std::pair<const std::string, std::deque<double>>& Item : Samples_)
            {
                Samples[Item.first] = std::vector<double>(Item.second.begin(), Item.second.end());
            }
        }
        Root["samples"] = Samples;
        std::error_code ErrorCode;
        std::filesystem::create_directories(CachePath_.parent_path(), ErrorCode);
        const std::filesystem::path Temporary = CachePath_.string() + ".tmp";
        {
            std::ofstream Stream(Temporary, std::ios::binary | std::ios::trunc);
            if (Stream.is_open() == false)
            {
                return;
            }
            Stream << Root.dump();
        }
        std::filesystem::rename(Temporary, CachePath_, ErrorCode);
    }

    std::filesystem::path CachePath_;
    std::filesystem::path ProjectsRoot_;
    std::map<std::string, FileState> Files_;  // worker thread only
    mutable std::mutex Mutex_;                // guards Samples_
    std::map<std::string, std::deque<double>> Samples_;
    std::thread Worker_;
    std::mutex WakeMutex_;
    std::condition_variable Wake_;
    std::atomic<bool> Stop_{false};
};
