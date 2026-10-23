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

    // OwnDirectory and Tag come from ScreenshotFinder::SessionShotDirectory / SessionTag; an empty tag keeps the old
    // behavior of claiming every image in shared ClaudeShots folders.
    std::vector<Shot> Poll(const std::string& ClaudeSessionId, const std::string& Cwd, const std::string& OwnDirectory, const std::string& Tag, const double Now)
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
        ScanShotDirectories(Cwd, OwnDirectory, Tag, Shots);
        return Shots;
    }

    // Percent of the context window the latest main-thread assistant message occupied, or -1 when unknown.
    // The transcript doesn't record the window size, so it's inferred from the model; usage past the standard
    // window always implies the large one.
    int ContextPercent() const
    {
        if (ContextTokens_ <= 0)
        {
            return -1;
        }
        const bool LargeWindow = ContextTokens_ > STANDARD_CONTEXT_WINDOW || HasLargeContextWindow(ContextModel_) == true;
        const std::int64_t Window = LargeWindow == true ? LARGE_CONTEXT_WINDOW : STANDARD_CONTEXT_WINDOW;
        return static_cast<int>(std::min<std::int64_t>(100, (ContextTokens_ * 100 + Window / 2) / Window));
    }

    struct ActiveCommand
    {
        enum class Kind { Shell, Background, Monitor };
        Kind Type = Kind::Shell;
        std::string TaskId;       // background shells and monitors, once started
        std::string Description;  // what Claude said the command is for
        std::string Command;
        std::string OutputFile;   // background shells write their output here
        std::time_t StartedAt = 0;
        std::time_t Deadline = 0;  // shell: when its time limit hits; monitor: when it expires; 0 = none
    };

    // Shell commands, background shells and monitors this session started at or after Since (the launch of its Claude
    // process; anything from an earlier run died with it) that have not finished yet, oldest first.
    std::vector<ActiveCommand> RunningCommands(const std::time_t Since) const
    {
        const std::time_t Now = std::time(nullptr);
        std::vector<ActiveCommand> Running;
        for (const std::pair<const std::string, ActiveCommand>& Item : Commands_)
        {
            const ActiveCommand& Command = Item.second;
            if (Command.StartedAt + LAUNCH_SLACK_SECONDS < Since)
            {
                continue;
            }
            if (Command.Deadline != 0 && Now > Command.Deadline + EXPIRY_SLACK_SECONDS)
            {
                continue;
            }
            Running.push_back(Command);
        }
        std::stable_sort(Running.begin(), Running.end(), [](const ActiveCommand& Left, const ActiveCommand& Right) { return Left.StartedAt < Right.StartedAt; });
        return Running;
    }

    // The text of Claude's most recent reply in the main conversation (empty before its first reply).
    const std::string& LastAssistantText() const { return LastAssistantText_; }

    // Background shells and monitors still running; Claude picks up again when they report.
    int RunningBackgroundTasks(const std::time_t Since) const
    {
        int Count = 0;
        for (const ActiveCommand& Command : RunningCommands(Since))
        {
            if (Command.Type != ActiveCommand::Kind::Shell && Command.TaskId.empty() == false)
            {
                Count++;
            }
        }
        return Count;
    }

    static std::filesystem::path ProjectsRoot()
    {
        const char* const ConfigDir = std::getenv("CLAUDE_CONFIG_DIR");
        const char* const Home = std::getenv("USERPROFILE") != nullptr ? std::getenv("USERPROFILE") : std::getenv("HOME");
        if (ConfigDir != nullptr)
        {
            return std::filesystem::path(ConfigDir) / "projects";
        }
        if (Home != nullptr)
        {
            return std::filesystem::path(Home) / ".claude" / "projects";
        }
        return {};
    }

private:
    static constexpr const char* SHELL_STARTED = "Command running in background with ID: ";
    static constexpr const char* MONITOR_STARTED = "Monitor started (task ";
    static constexpr const char* OUTPUT_MARKER = "Output is being written to: ";
    static constexpr std::time_t LAUNCH_SLACK_SECONDS = 5;
    static constexpr std::time_t EXPIRY_SLACK_SECONDS = 60;
    static constexpr std::time_t DEFAULT_SHELL_TIMEOUT_SECONDS = 120;

    // Text of a tool_result block, whose content is either a string or a list of text blocks.
    static std::string ToolResultText(const nlohmann::json& Block)
    {
        const nlohmann::json::const_iterator Content = Block.find("content");
        if (Content == Block.end())
        {
            return {};
        }
        if (Content->is_string() == true)
        {
            return Content->get<std::string>();
        }
        std::string Text;
        if (Content->is_array() == false)
        {
            return Text;
        }
        for (const nlohmann::json& Part : *Content)
        {
            if (Part.is_object() == true && Part.value("type", "") == "text")
            {
                Text += Part.value("text", "");
            }
        }
        return Text;
    }

    // "15m", "10m 20s", "1h 5m", "45s" at the start of Text, in seconds; 0 when nothing parses.
    static std::time_t ParseDuration(const std::string& Text)
    {
        std::time_t Total = 0;
        size_t Position = 0;
        while (Position < Text.size())
        {
            while (Position < Text.size() && Text[Position] == ' ')
            {
                Position++;
            }
            size_t DigitsEnd = Position;
            while (DigitsEnd < Text.size() && std::isdigit(static_cast<unsigned char>(Text[DigitsEnd])) != 0)
            {
                DigitsEnd++;
            }
            if (DigitsEnd == Position || DigitsEnd >= Text.size())
            {
                break;
            }
            const std::time_t Amount = static_cast<std::time_t>(std::atoll(Text.substr(Position, DigitsEnd - Position).c_str()));
            const char Unit = Text[DigitsEnd];
            if (Unit == 'h')
            {
                Total += Amount * 3600;
            }
            else if (Unit == 'm')
            {
                Total += Amount * 60;
            }
            else if (Unit == 's')
            {
                Total += Amount;
            }
            else
            {
                break;
            }
            Position = DigitsEnd + 1;
        }
        return Total;
    }

    void NoteCommandStarted(const nlohmann::json& Block, const std::time_t When)
    {
        const std::string Name = Block.value("name", "");
        const bool IsShell = Name == "Bash" || Name == "PowerShell";
        if ((IsShell == false && Name != "Monitor") || Block.contains("input") == false || Block["input"].is_object() == false)
        {
            return;
        }
        const nlohmann::json& Input = Block["input"];
        ActiveCommand Command;
        Command.Description = Input.value("description", "");
        Command.Command = Input.value("command", "");
        Command.StartedAt = When;
        if (IsShell == false)
        {
            Command.Type = ActiveCommand::Kind::Monitor;
            const double TimeoutMs = Input.value("timeout_ms", 0.0);
            Command.Deadline = TimeoutMs > 0 ? When + static_cast<std::time_t>(TimeoutMs / 1000.0) : 0;
        }
        else if (Input.value("run_in_background", false) == true)
        {
            Command.Type = ActiveCommand::Kind::Background;
        }
        else
        {
            const double TimeoutMs = Input.value("timeout", 0.0);
            Command.Deadline = When + (TimeoutMs > 0 ? static_cast<std::time_t>(TimeoutMs / 1000.0) : DEFAULT_SHELL_TIMEOUT_SECONDS);
        }
        Commands_[Block.value("id", "")] = Command;
    }

    // A tool result either finishes a shell command, or says a background shell or monitor is now running on its own.
    void NoteCommandResult(const nlohmann::json& Block, const std::time_t When)
    {
        const std::string ToolUseId = Block.value("tool_use_id", "");
        const std::string ResultText = ToolResultText(Block);
        std::string TaskId;
        ActiveCommand::Kind Type = ActiveCommand::Kind::Shell;
        std::time_t Deadline = 0;
        std::string OutputFile;
        if (ResultText.rfind(SHELL_STARTED, 0) == 0)
        {
            const size_t IdStart = std::string(SHELL_STARTED).size();
            TaskId = ResultText.substr(IdStart, ResultText.find_first_of(". \n", IdStart) - IdStart);
            Type = ActiveCommand::Kind::Background;
            const size_t OutputAt = ResultText.find(OUTPUT_MARKER);
            if (OutputAt != std::string::npos)
            {
                const size_t PathStart = OutputAt + std::string(OUTPUT_MARKER).size();
                const size_t PathEnd = ResultText.find(".output", PathStart);
                OutputFile = PathEnd == std::string::npos ? std::string() : ResultText.substr(PathStart, PathEnd + 7 - PathStart);
            }
        }
        else if (ResultText.rfind(MONITOR_STARTED, 0) == 0)
        {
            const size_t IdStart = std::string(MONITOR_STARTED).size();
            TaskId = ResultText.substr(IdStart, ResultText.find_first_of(",) \n", IdStart) - IdStart);
            Type = ActiveCommand::Kind::Monitor;
            const std::string ExpiresMarker = "expires in ";
            const size_t MarkerAt = ResultText.find(ExpiresMarker);
            if (MarkerAt != std::string::npos)
            {
                const std::time_t Duration = ParseDuration(ResultText.substr(MarkerAt + ExpiresMarker.size()));
                Deadline = Duration > 0 ? When + Duration : 0;
            }
        }

        std::map<std::string, ActiveCommand>::iterator Found = Commands_.find(ToolUseId);
        if (TaskId.empty() == true)
        {
            if (Found != Commands_.end())
            {
                Commands_.erase(Found);
            }
            return;
        }
        if (EndedTasks_.count(TaskId) > 0)
        {
            if (Found != Commands_.end())
            {
                Commands_.erase(Found);
            }
            return;
        }
        // The start may predate the part of the transcript that was read; track the task anyway.
        ActiveCommand& Command = Found != Commands_.end() ? Found->second : Commands_["task:" + TaskId];
        if (Command.StartedAt == 0)
        {
            Command.StartedAt = When;
        }
        Command.Type = Type;
        Command.TaskId = TaskId;
        Command.OutputFile = OutputFile;
        Command.Deadline = Type == ActiveCommand::Kind::Monitor ? (Deadline != 0 ? Deadline : Command.Deadline) : 0;
    }

    void EndTask(const std::string& TaskId)
    {
        if (TaskId.empty() == true)
        {
            return;
        }
        EndedTasks_.insert(TaskId);
        for (std::map<std::string, ActiveCommand>::iterator Item = Commands_.begin(); Item != Commands_.end();)
        {
            if (Item->second.TaskId == TaskId)
            {
                Item = Commands_.erase(Item);
                continue;
            }
            ++Item;
        }
    }

    // Each <task-notification> with a <status> means its task ended; ones without a status are a monitor's events.
    void NoteTaskNotifications(const std::string& Text)
    {
        const std::string Open = "<task-notification>";
        const std::string Close = "</task-notification>";
        size_t Position = 0;
        while ((Position = Text.find(Open, Position)) != std::string::npos)
        {
            const size_t End = Text.find(Close, Position);
            const std::string Body = Text.substr(Position, End == std::string::npos ? std::string::npos : End - Position);
            Position += Open.size();
            const size_t IdAt = Body.find("<task-id>");
            if (IdAt == std::string::npos || Body.find("<status>") == std::string::npos)
            {
                continue;
            }
            const size_t IdStart = IdAt + std::string("<task-id>").size();
            EndTask(Body.substr(IdStart, Body.find('<', IdStart) - IdStart));
        }
    }

    void RecordCommands(const nlohmann::json& Entry)
    {
        const std::time_t When = ParseTimestamp(Entry.value("timestamp", ""));
        if (Entry.value("type", "") == "queue-operation")
        {
            const nlohmann::json::const_iterator Queued = Entry.find("content");
            if (Queued != Entry.end() && Queued->is_string() == true)
            {
                NoteTaskNotifications(Queued->get<std::string>());
            }
            return;
        }
        const nlohmann::json::const_iterator Message = Entry.find("message");
        if (Message == Entry.end() || Message->is_object() == false || Entry.value("isSidechain", false) == true)
        {
            return;
        }
        const nlohmann::json::const_iterator Content = Message->find("content");
        if (Content == Message->end())
        {
            return;
        }
        // Only user turns carry real notifications; an assistant quoting one must not end a task.
        const bool FromUser = Message->value("role", "") == "user";
        if (Content->is_string() == true)
        {
            if (FromUser == true)
            {
                NoteTaskNotifications(Content->get<std::string>());
            }
            return;
        }
        if (Content->is_array() == false)
        {
            return;
        }
        for (const nlohmann::json& Block : *Content)
        {
            if (Block.is_object() == false)
            {
                continue;
            }
            const std::string Type = Block.value("type", "");
            if (Type == "text" && FromUser == false)
            {
                LastAssistantText_ = Block.value("text", "");
                continue;
            }
            if (Type == "tool_result" && FromUser == true)
            {
                NoteCommandResult(Block, When);
                continue;
            }
            if (Type == "text" && FromUser == true)
            {
                NoteTaskNotifications(Block.value("text", ""));
                continue;
            }
            if (Type != "tool_use" || FromUser == true)
            {
                continue;
            }
            if (Block.value("name", "") == "TaskStop" && Block.contains("input") == true && Block["input"].is_object() == true)
            {
                EndTask(Block["input"].value("task_id", ""));
                continue;
            }
            NoteCommandStarted(Block, When);
        }
    }

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
    static constexpr std::int64_t STANDARD_CONTEXT_WINDOW = 200000;
    static constexpr std::int64_t LARGE_CONTEXT_WINDOW = 1000000;

    // Every current model has a 1M window; only older generations are limited to 200k.
    static bool HasLargeContextWindow(const std::string& Model)
    {
        static const std::vector<std::string> STANDARD_WINDOW_MARKERS = {"claude-3", "claude-2", "-4-5", "-4-1", "-4-0", "-4-2025"};
        if (Model.empty() == true)
        {
            return false;
        }
        for (const std::string& Marker : STANDARD_WINDOW_MARKERS)
        {
            if (Model.find(Marker) != std::string::npos)
            {
                return false;
            }
        }
        return true;
    }

    static std::filesystem::path Locate(const std::string& ClaudeSessionId)
    {
        const std::filesystem::path Root = ProjectsRoot();
        if (Root.empty() == true)
        {
            return {};
        }
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
        if (Entry.is_object() == false)
        {
            return;
        }
        RecordCommands(Entry);
        if (Entry.contains("message") == false)
        {
            return;
        }
        RecordUsage(Entry);
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
                if (ScreenshotFinder::IsLiveFrame(Path) == true)
                {
                    continue;
                }
                Pending_.push_back({Path, Now, EntryTime});
            }
        }
    }

    void RecordUsage(const nlohmann::json& Entry)
    {
        if (Entry.value("isSidechain", false) == true)
        {
            return;
        }
        const nlohmann::json& Message = Entry["message"];
        if (Message.is_object() == false || Message.value("role", "") != "assistant" || Message.value("model", "") == "<synthetic>")
        {
            return;
        }
        const nlohmann::json::const_iterator Usage = Message.find("usage");
        if (Usage == Message.end() || Usage->is_object() == false)
        {
            return;
        }
        const std::int64_t Tokens = Usage->value("input_tokens", 0) + Usage->value("cache_creation_input_tokens", 0) +
                                    Usage->value("cache_read_input_tokens", 0);
        if (Tokens > 0)
        {
            ContextTokens_ = Tokens;
            ContextModel_ = Message.value("model", "");
        }
    }

    // Converting by sampling both clocks on every call jitters the result by a second, which made unchanged
    // files look rewritten; one offset taken per run keeps the same file time mapping to the same value.
    static std::time_t ToTimeT(const std::filesystem::file_time_type FileTime)
    {
        static const std::chrono::seconds CLOCK_OFFSET =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()) -
            std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now().time_since_epoch());
        return static_cast<std::time_t>((std::chrono::duration_cast<std::chrono::seconds>(FileTime.time_since_epoch()) + CLOCK_OFFSET).count());
    }

    void WatchShotDirectories(const std::string& Text)
    {
        for (const std::string& Directory : ScreenshotFinder::ExtractShotDirectories(Text))
        {
            WatchedDirectories_.insert(Directory);
        }
    }

    // Reports images written since this scanner started, whether or not the transcript names the file: every image in
    // the session's own folder, and in shared ClaudeShots folders (the cwd's, or one named in the transcript) only the
    // images whose name carries this session's tag, since other sessions of the project write there too.
    void ScanShotDirectories(const std::string& Cwd, const std::string& OwnDirectory, const std::string& Tag, std::vector<Shot>& Shots)
    {
        if (ScanSince_ == 0)
        {
            ScanSince_ = std::time(nullptr) - SHOT_DIRECTORY_LOOKBACK_SECONDS;
        }
        static const std::vector<std::string> EXTENSIONS = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"};

        std::map<std::filesystem::path, bool> Directories;
        Directories[(std::filesystem::path(Cwd) / ScreenshotFinder::SHOT_DIRECTORY_NAME).lexically_normal()] = Tag.empty() == true;
        for (const std::string& Watched : WatchedDirectories_)
        {
            std::filesystem::path Path(Watched);
            if (Path.is_relative() == true)
            {
                Path = std::filesystem::path(Cwd) / Path;
            }
            Directories.emplace(Path.lexically_normal(), Tag.empty() == true);
        }
        if (OwnDirectory.empty() == false)
        {
            Directories[std::filesystem::path(OwnDirectory).lexically_normal()] = true;
        }

        for (const std::pair<const std::filesystem::path, bool>& Folder : Directories)
        {
            const std::filesystem::path& Directory = Folder.first;
            const bool ClaimAll = Folder.second;
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
                if (ScreenshotFinder::IsLiveFrame(Item.path().string()) == true)
                {
                    continue;
                }
                if (ClaimAll == false && ScreenshotFinder::TagIn(Item.path().filename().string()) != Tag)
                {
                    continue;
                }
                std::error_code TimeError;
                const std::filesystem::file_time_type FileTime = Item.last_write_time(TimeError);
                if (TimeError)
                {
                    continue;
                }
                const std::time_t ModifiedAt = ToTimeT(FileTime);
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
            const std::time_t ModifiedAt = ToTimeT(FileTime);
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
    std::int64_t ContextTokens_ = 0;
    std::string ContextModel_;
    std::string LastAssistantText_;
    std::map<std::string, ActiveCommand> Commands_;  // tool_use id (or "task:<id>") -> running command
    std::set<std::string> EndedTasks_;
};
