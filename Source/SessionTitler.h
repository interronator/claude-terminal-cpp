#pragma once
#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Git.h"
#include "Session.h"

// Asks a small, fast Claude model for a very short title for a session, based on the first prompt the user sent it.
class SessionTitler
{
public:
    struct Finished
    {
        std::string SessionId;
        std::string OriginalName;
        std::string Title;  // empty when no usable title came back
    };

    bool IsPending(const std::string& SessionId) const
    {
        std::lock_guard<std::mutex> const Guard(Shared_->Mutex);
        return Shared_->Pending.count(SessionId) > 0;
    }

    void Request(std::vector<std::string> ClaudeArgv, const std::string& SessionId, const std::string& OriginalName, const std::string& Prompt)
    {
        {
            std::lock_guard<std::mutex> const Guard(Shared_->Mutex);
            Shared_->Pending.insert(SessionId);
        }
        ClaudeArgv.push_back("-p");
        ClaudeArgv.push_back(INSTRUCTION + Flatten(Prompt));
        ClaudeArgv.push_back("--model");
        ClaudeArgv.push_back("haiku");
        const std::shared_ptr<Shared> SharedState = Shared_;
        std::thread(
            [SharedState, ClaudeArgv, SessionId, OriginalName]
            {
                const ProcResult Outcome = Git::RunProcess(ClaudeArgv, "");
                const std::string Title = Outcome.Code == 0 ? Clean(Outcome.Output) : std::string();
                {
                    std::lock_guard<std::mutex> const Guard(SharedState->Mutex);
                    SharedState->Done.push_back({SessionId, OriginalName, Title});
                    SharedState->Pending.erase(SessionId);
                }
                if (Session::Wake != nullptr)
                {
                    Session::Wake();
                }
            })
            .detach();
    }

    std::vector<Finished> TakeFinished()
    {
        std::vector<Finished> Taken;
        std::lock_guard<std::mutex> const Guard(Shared_->Mutex);
        Taken.swap(Shared_->Done);
        return Taken;
    }

    // Prompts that are commands or notices rather than a task description make a poor title source.
    static bool IsUsablePrompt(const std::string& Prompt)
    {
        const std::string Flat = Flatten(Prompt);
        return Flat.empty() == false && Flat.front() != '<' && Flat.front() != '/';
    }

private:
    static constexpr size_t MAX_PROMPT_CHARS = 600;
    static constexpr size_t MAX_TITLE_CHARS = 40;
    static constexpr const char* INSTRUCTION =
        "Write a title for a coding chat session that starts with the request below. Use 2 to 4 words, as short as possible, "
        "no quotes, no punctuation at the end. Reply with only the title.\n\nRequest: ";

    struct Shared
    {
        std::mutex Mutex;
        std::set<std::string> Pending;
        std::vector<Finished> Done;
    };

    static std::string Flatten(const std::string& Text)
    {
        std::string Flat;
        bool PreviousSpace = true;
        for (const char Character : Text)
        {
            const bool IsSpace = std::isspace(static_cast<unsigned char>(Character)) != 0;
            if (IsSpace == true && PreviousSpace == true)
            {
                continue;
            }
            Flat.push_back(IsSpace == true ? ' ' : Character);
            PreviousSpace = IsSpace;
            if (Flat.size() >= MAX_PROMPT_CHARS)
            {
                break;
            }
        }
        while (Flat.empty() == false && Flat.back() == ' ')
        {
            Flat.pop_back();
        }
        return Flat;
    }

    static bool IsJunk(const char Character)
    {
        return std::isspace(static_cast<unsigned char>(Character)) != 0 || Character == '"' || Character == '\'' || Character == '`' || Character == '*' ||
               Character == '.' || Character == '#';
    }

    static std::string Clean(const std::string& Output)
    {
        std::string Line = Output.substr(0, Output.find('\n'));
        while (Line.empty() == false && IsJunk(Line.back()) == true)
        {
            Line.pop_back();
        }
        size_t Start = 0;
        while (Start < Line.size() && IsJunk(Line[Start]) == true)
        {
            Start++;
        }
        Line = Line.substr(Start);
        if (Line.size() > MAX_TITLE_CHARS)
        {
            Line.resize(MAX_TITLE_CHARS);
            const size_t LastSpace = Line.find_last_of(' ');
            if (LastSpace != std::string::npos && LastSpace > 0)
            {
                Line.resize(LastSpace);
            }
        }
        return Line;
    }

    std::shared_ptr<Shared> Shared_ = std::make_shared<Shared>();
};
