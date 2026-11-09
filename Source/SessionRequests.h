#pragma once
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

    static void EnsureInbox()
    {
        std::error_code ErrorCode;
        std::filesystem::create_directories(InboxDir(), ErrorCode);
    }

    static std::string Guidance(const std::string& Tag)
    {
        const std::string Inbox = InboxDir().string();
        return "You are running inside the Claude Terminal app, which shows each Claude session in its own pane. When the user asks you to do "
               "several separate tasks (for example tasks you suggested, or a list they gave), do not do them all yourself or with subagents: start "
               "one new Claude Terminal session per task so they run side by side. To start a session, write one JSON file per task into the folder " +
               Inbox + " with the fields name (a short session name), prompt (a complete, self-contained description of the task, because the new "
               "session has none of your context), cwd (the folder to work in; use your current project folder unless the task belongs elsewhere) "
               "and from (set it to '" + Tag + "'). Write each file under a name ending in .tmp first, then rename it to end in .json, so the app "
               "never reads a half-written file. The app opens each session within a couple of seconds and sends it the prompt. When the tasks "
               "touch related code, say in each prompt which other sessions you started and for which task, and tell it to coordinate with them. "
               "Afterwards tell the user which sessions you started. Still do a task yourself when it is a single small task, when the tasks depend "
               "on each other's results, or when the user asks you to do it yourself. "
               "Other Claude sessions may be working in this project at the same time. Use ListAgents to see them and SendMessage to talk to them. "
               "When your work touches the same files, systems or features as another session, message it before making big changes: say what you "
               "are about to change, ask what it is changing, agree who edits which file, and tell it when you finish something it depends on. "
               "Reply when another session messages you. Keep messages short, and only send them when they help avoid conflicts or duplicate work.";
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
        if (ErrorCode)
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
            if (Parsed.Error.empty() == true || ErrorCode)
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
        std::ifstream Stream(Path);
        const nlohmann::json Root = nlohmann::json::parse(Stream, nullptr, false);
        if (Root.is_discarded() == true || Root.is_object() == false)
        {
            Parsed.Error = "not valid JSON";
            return Parsed;
        }
        Parsed.Name = StringField(Root, "name");
        Parsed.Prompt = StringField(Root, "prompt");
        Parsed.Cwd = StringField(Root, "cwd");
        Parsed.From = StringField(Root, "from");
        if (Parsed.Prompt.empty() == true)
        {
            Parsed.Error = "it has no prompt";
        }
        return Parsed;
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
};
