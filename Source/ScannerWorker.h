#pragma once
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Session.h"
#include "TranscriptScanner.h"

// Runs TranscriptScanner::Poll off the UI thread: reading and parsing big transcripts takes long enough to freeze the window.
class ScannerWorker
{
public:
    struct Job
    {
        std::string SessionId;
        std::shared_ptr<TranscriptScanner> Scanner;
        std::string ClaudeSessionId;
        std::string Cwd;
        std::string OwnDirectory;
        std::string Tag;
        double Now = 0;
    };

    struct Result
    {
        std::string SessionId;
        std::vector<TranscriptScanner::Shot> Shots;
    };

    ScannerWorker()
    {
        Thread_ = std::thread([this] { Run(); });
    }

    ~ScannerWorker()
    {
        {
            std::lock_guard<std::mutex> const Guard(Mutex_);
            Stop_ = true;
        }
        Wakeup_.notify_all();
        Thread_.join();
    }

    ScannerWorker(const ScannerWorker&) = delete;
    ScannerWorker& operator=(const ScannerWorker&) = delete;

    bool IsBusy(const std::string& SessionId) const
    {
        std::lock_guard<std::mutex> const Guard(Mutex_);
        return InFlight_.count(SessionId) > 0;
    }

    void Submit(Job NewJob)
    {
        {
            std::lock_guard<std::mutex> const Guard(Mutex_);
            if (InFlight_.insert(NewJob.SessionId).second == false)
            {
                return;
            }
            Queue_.push_back(std::move(NewJob));
        }
        Wakeup_.notify_one();
    }

    std::vector<Result> TakeResults()
    {
        std::vector<Result> Taken;
        std::lock_guard<std::mutex> const Guard(Mutex_);
        Taken.swap(Results_);
        return Taken;
    }

private:
    void Run()
    {
        while (true)
        {
            Job Current;
            {
                std::unique_lock<std::mutex> Lock(Mutex_);
                Wakeup_.wait(Lock, [this] { return Stop_ == true || Queue_.empty() == false; });
                if (Stop_ == true)
                {
                    return;
                }
                Current = std::move(Queue_.front());
                Queue_.pop_front();
            }
            Result Done;
            Done.SessionId = Current.SessionId;
            Done.Shots = Current.Scanner->Poll(Current.ClaudeSessionId, Current.Cwd, Current.OwnDirectory, Current.Tag, Current.Now);
            {
                std::lock_guard<std::mutex> const Guard(Mutex_);
                InFlight_.erase(Current.SessionId);
                Results_.push_back(std::move(Done));
            }
            if (Session::Wake != nullptr)
            {
                Session::Wake();
            }
        }
    }

    mutable std::mutex Mutex_;
    std::condition_variable Wakeup_;
    std::deque<Job> Queue_;
    std::set<std::string> InFlight_;
    std::vector<Result> Results_;
    bool Stop_ = false;
    std::thread Thread_;
};
