#pragma once
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "Store.h"

// Inbox folder where Claude sessions drop one JSON file per new session they want this app to start.
class SessionRequests
{
public:
    struct Request
    {
        std::string Name;
        std::string Prompt;
        std::string Cwd;
        std::string From;
        std::string Error;  // set when the file could not be read; the other fields are then empty
        std::string File;
    };

    static std::filesystem::path InboxDir()
    {
        return std::filesystem::path(Store::DefaultPath()).parent_path() / "SessionRequests";
    }

    static std::filesystem::path DoneDir()
    {
        return InboxDir() / "Done";
    }

    static std::filesystem::path StartedDir()
    {
        return InboxDir() / "Started";
    }

    static void EnsureInbox()
    {
        std::error_code ErrorCode;
        std::filesystem::create_directories(DoneDir(), ErrorCode);
        std::filesystem::create_directories(StartedDir(), ErrorCode);
    }

    static void WriteStarted(const std::string& RequestFile, const std::string& Name, const std::string& Tag)
    {
        std::error_code ErrorCode;
        std::filesystem::create_directories(StartedDir(), ErrorCode);
        std::ofstream Stream(StartedDir() / (std::filesystem::path(RequestFile).stem().string() + ".txt"), std::ios::binary);
        Stream << "name: " << Name << "\ntag: " << Tag << "\n";
    }

    // Tags of sessions that dropped "<tag>.done" into the Done folder to say they are finished and need nothing more.
    std::vector<std::string> CollectDone()
    {
        std::vector<std::string> Tags;
        const std::chrono::steady_clock::time_point Now = std::chrono::steady_clock::now();
        if (Now - LastDonePoll_ < std::chrono::seconds(1))
        {
            return Tags;
        }
        LastDonePoll_ = Now;
        std::error_code ErrorCode;
        std::filesystem::directory_iterator Iterator(DoneDir(), ErrorCode);
        if (ErrorCode != std::error_code())
        {
            return Tags;
        }
        for (const std::filesystem::directory_entry& Entry : Iterator)
        {
            if (Entry.is_regular_file(ErrorCode) == false || Entry.path().extension() != ".done")
            {
                continue;
            }
            Tags.push_back(Entry.path().stem().string());
            std::filesystem::remove(Entry.path(), ErrorCode);
        }
        return Tags;
    }

    static std::string Guidance(const std::string& Tag, const std::string& PeerName)
    {
        const std::string Inbox = InboxDir().string();
        return "You are running inside the Claude Terminal app, which shows each Claude session in its own pane. When the user asks you to do "
               "several separate tasks (for example tasks you suggested, or a list they gave), do not do them all yourself or with subagents: start "
               "one new Claude Terminal session per task so they run side by side. To start a session, write one JSON file per task into the folder " +
               Inbox + " with the fields name (a short unique session name), cwd (the folder to work in; use your current project folder unless the "
               "task belongs elsewhere) and from (set it to '" + Tag + "'). Do not put the task in the file: the new session starts empty. Write each "
               "file under a name ending in .tmp first, then rename it to end in .json, so the app never reads a half-written file. Within a couple of "
               "seconds the app opens the session, and once its Claude has finished loading and is ready it writes <file name without extension>.txt in " +
               StartedDir().string() + " containing the new session's name and tag. Wait for that file (retry every few seconds; do not message "
               "before it exists), find the ListAgents row whose name ends with that tag, and give the session its task with SendMessage: a complete, self-contained description, because it has none of your context. In that "
               "message also say that you started it (your name '" + PeerName + "', tag " + Tag + "), to report back to you with SendMessage when "
               "finished or blocked, and that when it is completely finished and needs nothing more it should make its very last action creating an "
               "empty file named " + DoneDir().string() + "\\<its own tag>.done (the app then closes it; not when it is blocked or the user may want "
               "to talk to it). After sending, check ListAgents after about 30 seconds: if the session still shows idle and has not replied, send the "
               "task again, and again if needed. When the tasks touch related code, say in each message which other sessions you started and for which task, and "
               "tell it to coordinate with them. "
               "Give each session its own files and folders to edit, name the shared contract (a short doc) they both follow, and state the order: "
               "which session must finish something first and which one waits for its message, so waiting is planned. Tell each session to ignore "
               "build or test failures in the other sessions' files until they report done. "
               "Afterwards tell the user which sessions you started. Still do a task yourself when it is a single small task, when the tasks depend "
               "on each other's results, or when the user asks you to do it yourself. "
               "Other Claude sessions may be working in this project at the same time. Use ListAgents to see them and SendMessage to talk to them. "
               "When your work touches the same files, systems or features as another session, message it before making big changes: say what you "
               "are about to change, ask what it is changing, agree who edits which file, and tell it when you finish something it depends on. "
               "Reply when another session messages you. Keep messages short, and only send them when they help avoid conflicts or duplicate work. "
               "In ListAgents this session is named '" + PeerName + "'. Every session started by this app has a name ending in its tag (cs-xxxxxxxx), so "
               "when you are given a tag, pick the ListAgents row whose name ends with it. A session you start is told to report back to you with "
               "SendMessage when it finishes or is blocked: read those reports and act on them. If you start child sessions to make visual edits "
               "or additions, the screenshots panel in the terminal only shows screenshots saved in your own session's screenshot folder, so once "
               "the child sessions are done, capture the final screenshots of what was changed or added yourself, save them to your own folder with "
               "your own tag, and mention their full paths, so the user sees the results in the screenshots panel.";
    }

    std::vector<Request> Collect()
    {
        std::vector<Request> Found;
        const std::chrono::steady_clock::time_point Now = std::chrono::steady_clock::now();
        if (Now - LastPoll_ < std::chrono::seconds(1))
        {
            return Found;
        }
        LastPoll_ = Now;
        const std::filesystem::path Inbox = InboxDir();
        std::error_code ErrorCode;
        std::filesystem::directory_iterator Iterator(Inbox, ErrorCode);
        if (ErrorCode != std::error_code())
        {
            return Found;
        }
        for (const std::filesystem::directory_entry& Entry : Iterator)
        {
            if (Entry.is_regular_file(ErrorCode) == false || Entry.path().extension() != ".json")
            {
                continue;
            }
            Request Parsed = Read(Entry.path());
            if (Parsed.Error.empty() == false)
            {
                std::filesystem::create_directories(Inbox / "Failed", ErrorCode);
                std::filesystem::rename(Entry.path(), Inbox / "Failed" / Entry.path().filename(), ErrorCode);
            }
            if (Parsed.Error.empty() == true || ErrorCode != std::error_code())
            {
                std::filesystem::remove(Entry.path(), ErrorCode);
            }
            Found.push_back(std::move(Parsed));
        }
        return Found;
    }

private:
    static Request Read(const std::filesystem::path& Path)
    {
        Request Parsed;
        Parsed.File = Path.filename().string();
        std::ifstream Stream(Path, std::ios::binary);
        const std::string Text((std::istreambuf_iterator<char>(Stream)), std::istreambuf_iterator<char>());
        nlohmann::json Root = nlohmann::json::parse(Text, nullptr, false);
        if (Root.is_discarded() == true)
        {
            Root = nlohmann::json::parse(EscapeLooseBackslashes(Text), nullptr, false);
        }
        if (Root.is_discarded() == true || Root.is_object() == false)
        {
            Parsed.Error = "not valid JSON";
            return Parsed;
        }
        Parsed.Name = StringField(Root, "name");
        Parsed.Prompt = StringField(Root, "prompt");
        Parsed.Cwd = StringField(Root, "cwd");
        Parsed.From = StringField(Root, "from");
        return Parsed;
    }

    // Sessions often write Windows paths with single backslashes ("M:\Battlenet"), which is invalid JSON.
    static std::string EscapeLooseBackslashes(const std::string& Text)
    {
        std::string Fixed;
        Fixed.reserve(Text.size() + 16);
        for (size_t Index = 0; Index < Text.size(); ++Index)
        {
            const char Current = Text[Index];
            if (Current != '\\')
            {
                Fixed.push_back(Current);
                continue;
            }
            const char Next = Index + 1 < Text.size() ? Text[Index + 1] : '\0';
            if (IsDriveLetterPrefix(Text, Index) == true)
            {
                Fixed += "\\\\";
                continue;
            }
            if (Next == '\\' || Next == '"' || Next == '/' || Next == 'b' || Next == 'f' || Next == 'n' || Next == 'r' || Next == 't')
            {
                Fixed.push_back(Current);
                Fixed.push_back(Next);
                ++Index;
                continue;
            }
            if (Next == 'u' && HasFourHexDigits(Text, Index + 2) == true)
            {
                Fixed.append(Text, Index, 6);
                Index += 5;
                continue;
            }
            Fixed += "\\\\";
        }
        return Fixed;
    }

    static bool IsDriveLetterPrefix(const std::string& Text, const size_t BackslashIndex)
    {
        if (BackslashIndex < 2 || Text[BackslashIndex - 1] != ':')
        {
            return false;
        }
        if (std::isalpha(static_cast<unsigned char>(Text[BackslashIndex - 2])) == 0)
        {
            return false;
        }
        if (BackslashIndex >= 3 && std::isalnum(static_cast<unsigned char>(Text[BackslashIndex - 3])) != 0)
        {
            return false;
        }
        return true;
    }

    static bool HasFourHexDigits(const std::string& Text, const size_t StartIndex)
    {
        if (StartIndex + 4 > Text.size())
        {
            return false;
        }
        for (size_t Offset = 0; Offset < 4; ++Offset)
        {
            if (std::isxdigit(static_cast<unsigned char>(Text[StartIndex + Offset])) == 0)
            {
                return false;
            }
        }
        return true;
    }

    static std::string StringField(const nlohmann::json& Root, const char* const Key)
    {
        const nlohmann::json::const_iterator Found = Root.find(Key);
        if (Found == Root.end() || Found->is_string() == false)
        {
            return std::string();
        }
        return Found->get<std::string>();
    }

    std::chrono::steady_clock::time_point LastPoll_{};
    std::chrono::steady_clock::time_point LastDonePoll_{};
};
