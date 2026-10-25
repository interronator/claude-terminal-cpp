#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Git.h"
#include "Pty.h"
#include "Session.h"

// Drives `gh auth login --web` in a hidden pseudo-terminal so the one-time device code can be shown in the app.
// gh does the OAuth exchange and keeps the token; this class only reads gh's console output.
class GitHubLogin
{
public:
    enum class Phase { Idle, Starting, WaitingForBrowser, Succeeded, Failed, SigningOut, SignedOut, SignOutFailed };

    struct Snapshot
    {
        Phase Value = Phase::Idle;
        std::string Code;
        std::string Message;
    };

    ~GitHubLogin()
    {
        Cancel();
        if (LogoutThread_.joinable() == true)
        {
            LogoutThread_.join();
        }
    }

    void StartLogout(const std::string& User)
    {
        Cancel();
        {
            std::lock_guard<std::mutex> const Guard(Mutex_);
            if (Phase_ == Phase::SigningOut)
            {
                return;
            }
            Phase_ = Phase::SigningOut;
            Message_.clear();
        }
        if (LogoutThread_.joinable() == true)
        {
            LogoutThread_.join();
        }
        LogoutThread_ = std::thread(
            [this, User]
            {
                std::string Error;
                const bool Ok = Git::LogoutGitHub(User, Error);
                {
                    std::lock_guard<std::mutex> const Guard(Mutex_);
                    Phase_ = Ok == true ? Phase::SignedOut : Phase::SignOutFailed;
                    Message_ = Error;
                }
                if (Session::Wake)
                {
                    Session::Wake();
                }
            });
    }

    void Start()
    {
        Cancel();
        {
            std::lock_guard<std::mutex> const Guard(Mutex_);
            Output_.clear();
            Code_.clear();
            Message_.clear();
            AnsweredPrompts_ = 0;
            Phase_ = Phase::Starting;
        }
        Pty_ = Pty::Create();
        PtyCallbacks Callbacks;
        Callbacks.OnData = [this](const char* Data, size_t Length) { HandleData(Data, Length); };
        Callbacks.OnExit = [this](int ExitCode) { HandleExit(ExitCode); };
        std::string StartError;
        const std::vector<std::string> Arguments = {"gh", "auth", "login", "--hostname", "github.com", "--git-protocol", "https", "--web"};
        if (Pty_->Start(Arguments, "", 100, 30, std::move(Callbacks), StartError) == true)
        {
            return;
        }
        Pty_.reset();
        std::lock_guard<std::mutex> const Guard(Mutex_);
        Phase_ = Phase::Failed;
        Message_ = "Could not start the GitHub CLI: " + StartError;
    }

    void Cancel()
    {
        if (Pty_ == nullptr)
        {
            return;
        }
        Pty_->Kill();
        Pty_.reset();
        std::lock_guard<std::mutex> const Guard(Mutex_);
        if (Phase_ == Phase::Starting || Phase_ == Phase::WaitingForBrowser)
        {
            Phase_ = Phase::Idle;
        }
    }

    void Reset()
    {
        Cancel();
        std::lock_guard<std::mutex> const Guard(Mutex_);
        if (Phase_ == Phase::SigningOut)
        {
            return;
        }
        Phase_ = Phase::Idle;
        Code_.clear();
        Message_.clear();
    }

    Snapshot Read() const
    {
        std::lock_guard<std::mutex> const Guard(Mutex_);
        return Snapshot{Phase_, Code_, Message_};
    }

private:
    void HandleData(const char* Data, size_t Length)
    {
        std::string Answer;
        {
            std::lock_guard<std::mutex> const Guard(Mutex_);
            Output_.append(Data, Length);
            const std::string Clean = StripEscapes(Output_);

            if (Code_.empty() == true)
            {
                const std::string Marker = "one-time code: ";
                const size_t MarkerAt = Clean.find(Marker);
                if (MarkerAt != std::string::npos && Clean.size() >= MarkerAt + Marker.size() + 9)
                {
                    Code_ = Clean.substr(MarkerAt + Marker.size(), 9);
                    Phase_ = Phase::WaitingForBrowser;
                }
            }

            const int PromptCount = CountOccurrences(Clean, "Press Enter") + CountOccurrences(Clean, "(Y/n)");
            if (PromptCount > AnsweredPrompts_)
            {
                AnsweredPrompts_ = PromptCount;
                Answer = "\r";
            }
        }
        if (Answer.empty() == false && Pty_ != nullptr)
        {
            Pty_->Write(Answer.data(), Answer.size());
        }
        if (Session::Wake)
        {
            Session::Wake();
        }
    }

    void HandleExit(int ExitCode)
    {
        {
            std::lock_guard<std::mutex> const Guard(Mutex_);
            if (Phase_ == Phase::Idle)
            {
                return;
            }
            if (ExitCode == 0)
            {
                Phase_ = Phase::Succeeded;
            }
            else
            {
                Phase_ = Phase::Failed;
                Message_ = LastLines(StripEscapes(Output_));
            }
        }
        if (Session::Wake)
        {
            Session::Wake();
        }
    }

    static int CountOccurrences(const std::string& Text, const std::string& Needle)
    {
        int Count = 0;
        for (size_t At = Text.find(Needle); At != std::string::npos; At = Text.find(Needle, At + Needle.size()))
        {
            Count++;
        }
        return Count;
    }

    static std::string StripEscapes(const std::string& Text)
    {
        std::string Clean;
        for (size_t Index = 0; Index < Text.size(); Index++)
        {
            const char Character = Text[Index];
            if (Character == '\x1b' && Index + 1 < Text.size())
            {
                const char Kind = Text[Index + 1];
                Index++;
                if (Kind == '[')
                {
                    while (Index + 1 < Text.size() && (Text[Index + 1] < '@' || Text[Index + 1] > '~'))
                    {
                        Index++;
                    }
                    Index++;
                }
                else if (Kind == ']')
                {
                    while (Index + 1 < Text.size() && Text[Index + 1] != '\x07' && Text[Index + 1] != '\x1b')
                    {
                        Index++;
                    }
                    Index++;
                }
                continue;
            }
            if (Character == '\r')
            {
                continue;
            }
            Clean += Character;
        }
        return Clean;
    }

    static std::string LastLines(const std::string& Text)
    {
        std::vector<std::string> Lines;
        size_t Start = 0;
        while (Start < Text.size())
        {
            size_t End = Text.find('\n', Start);
            if (End == std::string::npos)
            {
                End = Text.size();
            }
            std::string const Line = Text.substr(Start, End - Start);
            const size_t First = Line.find_first_not_of(" \t");
            if (First != std::string::npos)
            {
                Lines.push_back(Line.substr(First));
            }
            Start = End + 1;
        }
        std::string Joined;
        const size_t From = Lines.size() > 3 ? Lines.size() - 3 : 0;
        for (size_t Index = From; Index < Lines.size(); Index++)
        {
            Joined += (Joined.empty() == true ? "" : "\n") + Lines[Index];
        }
        return Joined.empty() == true ? "GitHub login did not complete." : Joined;
    }

    mutable std::mutex Mutex_;
    std::unique_ptr<Pty> Pty_;
    std::thread LogoutThread_;
    std::string Output_;
    std::string Code_;
    std::string Message_;
    Phase Phase_ = Phase::Idle;
    int AnsweredPrompts_ = 0;
};
