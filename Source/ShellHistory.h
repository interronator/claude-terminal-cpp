#pragma once

#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "TranscriptScanner.h"

// Remembers, per session, the shell commands that were running on the previous scan and are gone now.
class ShellHistory
{
public:
    struct Finished
    {
        TranscriptScanner::ActiveCommand Command;
        std::time_t EndedAt = 0;
    };

    void Update(const std::string& SessionId, const std::vector<TranscriptScanner::ActiveCommand>& Running, const std::time_t Now)
    {
        constexpr size_t MAX_FINISHED = 10;
        std::vector<TranscriptScanner::ActiveCommand>& Previous = Previous_[SessionId];
        std::vector<Finished>& Done = Finished_[SessionId];
        for (const TranscriptScanner::ActiveCommand& Old : Previous)
        {
            bool StillRunning = false;
            for (const TranscriptScanner::ActiveCommand& Current : Running)
            {
                if (SameCommand(Old, Current) == true)
                {
                    StillRunning = true;
                    break;
                }
            }
            if (StillRunning == true)
            {
                continue;
            }
            Finished Entry;
            Entry.Command = Old;
            Entry.EndedAt = Now;
            Done.insert(Done.begin(), Entry);
        }
        if (Done.size() > MAX_FINISHED)
        {
            Done.resize(MAX_FINISHED);
        }
        Previous = Running;
    }

    const std::vector<Finished>& For(const std::string& SessionId) const
    {
        static const std::vector<Finished> NONE;
        const std::map<std::string, std::vector<Finished>>::const_iterator Found = Finished_.find(SessionId);
        return Found == Finished_.end() ? NONE : Found->second;
    }

    void Forget(const std::string& SessionId)
    {
        Previous_.erase(SessionId);
        Finished_.erase(SessionId);
    }

private:
    static bool SameCommand(const TranscriptScanner::ActiveCommand& Left, const TranscriptScanner::ActiveCommand& Right)
    {
        return Left.Type == Right.Type && Left.TaskId == Right.TaskId && Left.StartedAt == Right.StartedAt && Left.Command == Right.Command;
    }

    std::map<std::string, std::vector<TranscriptScanner::ActiveCommand>> Previous_;
    std::map<std::string, std::vector<Finished>> Finished_;
};
