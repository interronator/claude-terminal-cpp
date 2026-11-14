#pragma once
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "TranscriptScanner.h"

// Spots background shell commands that went quiet while their session sits idle waiting for them, so the session
// can be told to go and look instead of waiting for a "finished" report that never came.
class BackgroundWatch
{
public:
    static constexpr double CHECK_INTERVAL_SECONDS = 15.0;
    static constexpr long long QUIET_SECONDS = 120;
    static constexpr long long MONITOR_QUIET_SECONDS = 300;
    static constexpr long long REMIND_SECONDS = 600;

    static std::string Guidance()
    {
        return "Whenever you start a shell process that runs in the background or takes more than a few seconds, you are responsible for "
               "noticing when it ends: do not just wait or say you will check later. Prefer run_in_background (you are woken when it "
               "finishes); a Monitor can stay silent or never report, so do not rely on it alone, and while you wait, read its output file or poll its status every minute or two. If it has finished, act on the "
               "result straight away; if it has hung or stopped producing output, stop it and say so. The app also reminds you when a "
               "background command has been quiet for a while; when that happens, check it immediately.";
    }

    // Returns the reminder to send to the session, or an empty string when nothing needs a look.
    std::string Check(const std::string& SessionId, const std::vector<TranscriptScanner::ActiveCommand>& Commands)
    {
        const std::time_t Now = std::time(nullptr);
        std::string Lines;
        std::vector<std::string> Reminded;
        for (const TranscriptScanner::ActiveCommand& Command : Commands)
        {
            if (Command.Type == TranscriptScanner::ActiveCommand::Kind::Shell || Command.TaskId.empty() == true)
            {
                continue;
            }
            const bool IsMonitor = Command.Type == TranscriptScanner::ActiveCommand::Kind::Monitor;
            const long long QuietFor = SecondsQuiet(Command, Now);
            if (QuietFor < (IsMonitor == true ? MONITOR_QUIET_SECONDS : QUIET_SECONDS))
            {
                continue;
            }
            const std::string Key = SessionId + ":" + Command.TaskId;
            const std::map<std::string, std::time_t>::const_iterator Known = LastReminded_.find(Key);
            if (Known != LastReminded_.end() && static_cast<long long>(Now - Known->second) < REMIND_SECONDS)
            {
                continue;
            }
            Reminded.push_back(Key);
            Lines += std::string(IsMonitor == true ? "- monitor " : "- background task ") + Command.TaskId;
            if (Command.Description.empty() == false)
            {
                Lines += " (" + Command.Description + ")";
            }
            Lines += std::string(IsMonitor == true ? ": no events for " : ": no new output for ") + std::to_string(QuietFor / 60) + " min";
            if (Command.OutputFile.empty() == false)
            {
                Lines += ", output file " + Command.OutputFile;
            }
            Lines += "\n";
        }
        if (Lines.empty() == true)
        {
            return {};
        }
        for (const std::string& Key : Reminded)
        {
            LastReminded_[Key] = Now;
        }
        return "(Reminder from the app) These background commands and monitors have been quiet for a while and may already be finished or "
               "stuck. Check each one now: read its output, and if it is done act on the result. A monitor that is not reporting must not be "
               "waited on: look at the thing it watches yourself, and if the monitor is useless or stuck stop it (TaskStop) and tell the user.\n" + Lines;
    }

    void NoteBusy(const std::string& SessionId, const bool IsBusy)
    {
        if (IsBusy == false)
        {
            BusySince_.erase(SessionId);
            return;
        }
        if (BusySince_.count(SessionId) == 0)
        {
            BusySince_[SessionId] = std::time(nullptr);
        }
    }

    // A foreground command that ran past its time limit while the session stayed busy the whole time (time spent
    // waiting for an approval resets this) is hung. Returns the prompt to give the session after interrupting it,
    // or an empty string; each command is reported once.
    std::string CheckHung(const std::string& SessionId, const std::vector<TranscriptScanner::ActiveCommand>& Overdue)
    {
        const std::map<std::string, std::time_t>::const_iterator Busy = BusySince_.find(SessionId);
        if (Busy == BusySince_.end())
        {
            return {};
        }
        const std::time_t Now = std::time(nullptr);
        for (const TranscriptScanner::ActiveCommand& Command : Overdue)
        {
            const long long Allowed = static_cast<long long>(Command.Deadline - Command.StartedAt) + HUNG_GRACE_SECONDS;
            if (static_cast<long long>(Now - Busy->second) < Allowed)
            {
                continue;
            }
            const std::string Key = SessionId + ":" + std::to_string(Command.StartedAt) + ":" + Command.Command;
            if (Interrupted_.insert(Key).second == false)
            {
                continue;
            }
            const std::string Label = Command.Description.empty() == false ? Command.Description : Command.Command.substr(0, 120);
            return "(Reminder from the app) Your command \"" + Label + "\" ran well past its time limit and looked hung, so the app "
                   "interrupted it. Look at what it was doing, then decide: retry it in the background (run_in_background) with a "
                   "sensible timeout, work around it, or tell the user it is stuck. Do not just wait.";
        }
        return {};
    }

private:
    static constexpr long long HUNG_GRACE_SECONDS = 120;
    std::map<std::string, std::time_t> BusySince_;
    std::set<std::string> Interrupted_;

    static long long SecondsQuiet(const TranscriptScanner::ActiveCommand& Command, const std::time_t Now)
    {
        if (Command.Type == TranscriptScanner::ActiveCommand::Kind::Monitor)
        {
            const std::time_t Last = std::max(Command.LastEventAt, Command.StartedAt);
            return Last == 0 ? 0 : static_cast<long long>(Now - Last);
        }
        std::error_code ErrorCode;
        if (Command.OutputFile.empty() == false)
        {
            const std::filesystem::file_time_type Modified = std::filesystem::last_write_time(Command.OutputFile, ErrorCode);
            if (ErrorCode == std::error_code())
            {
                const std::chrono::seconds Age = std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now() - Modified);
                return Age.count() < 0 ? 0 : static_cast<long long>(Age.count());
            }
        }
        return Command.StartedAt == 0 ? 0 : static_cast<long long>(Now - Command.StartedAt);
    }

    std::map<std::string, std::time_t> LastReminded_;
};
