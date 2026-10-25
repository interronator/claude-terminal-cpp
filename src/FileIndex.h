#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Background-scanned list of the files under one project folder, with a ranked substring/subsequence search.
class FileIndex
{
public:
    ~FileIndex() { Stop(); }

    static std::string Lower(const std::string& Text)
    {
        std::string Result = Text;
        for (char& Character : Result)
        {
            Character = static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
        }
        return Result;
    }

    void Start(const std::string& Root)
    {
        Stop();
        {
            std::lock_guard<std::mutex> const Guard(Mutex_);
            Paths_.clear();
            Lowered_.clear();
            Root_ = Root;
        }
        Scanning_ = true;
        Cancel_ = false;
        Worker_ = std::thread([this, Root] { Scan(Root); });
    }

    bool Scanning() const { return Scanning_; }

    size_t Size() const
    {
        std::lock_guard<std::mutex> const Guard(Mutex_);
        return Paths_.size();
    }

    std::string Root() const
    {
        std::lock_guard<std::mutex> const Guard(Mutex_);
        return Root_;
    }

    // Best matches first: filename prefix, filename substring, path substring, then loose subsequence.
    void Search(const std::string& Query, size_t Limit, std::vector<std::string>& Out) const
    {
        Out.clear();
        const std::string Needle = Lower(Query);
        if (Needle.empty() == true)
        {
            return;
        }
        std::vector<std::pair<std::pair<int, size_t>, size_t>> Scored;
        std::lock_guard<std::mutex> const Guard(Mutex_);
        for (size_t Index = 0; Index < Lowered_.size(); Index++)
        {
            const std::string& Path = Lowered_[Index];
            const size_t NameStart = Path.find_last_of('/') == std::string::npos ? 0 : Path.find_last_of('/') + 1;
            const size_t NameAt = Path.find(Needle, NameStart);
            int Rank = 4;
            if (NameAt == NameStart)
            {
                Rank = 0;
            }
            else if (NameAt != std::string::npos)
            {
                Rank = 1;
            }
            else if (Path.find(Needle) != std::string::npos)
            {
                Rank = 2;
            }
            else if (IsSubsequence(Needle, Path) == true)
            {
                Rank = 3;
            }
            if (Rank == 4)
            {
                continue;
            }
            Scored.push_back({{Rank, Path.size()}, Index});
        }
        const size_t Keep = std::min(Limit, Scored.size());
        std::partial_sort(Scored.begin(), Scored.begin() + Keep, Scored.end());
        for (size_t Index = 0; Index < Keep; Index++)
        {
            Out.push_back(Paths_[Scored[Index].second]);
        }
    }

private:
    static constexpr size_t MAX_FILES = 60000;

    static bool IsSubsequence(const std::string& Needle, const std::string& Haystack)
    {
        size_t NeedleAt = 0;
        for (size_t Index = 0; Index < Haystack.size() && NeedleAt < Needle.size(); Index++)
        {
            if (Haystack[Index] == Needle[NeedleAt])
            {
                NeedleAt++;
            }
        }
        return NeedleAt == Needle.size();
    }

    static bool IsSkippedDirectory(const std::string& Name)
    {
        static const std::vector<std::string> SKIPPED = {".git",   "node_modules", ".venv",  "venv", "__pycache__", ".next",
                                                         "target", "dist",         "build",  "out",  ".gradle",     ".idea",
                                                         ".vs",    "obj",          "bin",    ".cache", ".claude"};
        return std::find(SKIPPED.begin(), SKIPPED.end(), Name) != SKIPPED.end();
    }

    void Scan(const std::string& Root)
    {
        namespace fs = std::filesystem;
        std::error_code ErrorCode;
        fs::recursive_directory_iterator Iterator(Root, fs::directory_options::skip_permission_denied, ErrorCode);
        const fs::recursive_directory_iterator End;
        std::vector<std::string> Batch;
        size_t Total = 0;
        while (ErrorCode.value() == 0 && Iterator != End && Cancel_ == false && Total < MAX_FILES)
        {
            const fs::directory_entry Entry = *Iterator;
            std::error_code EntryError;
            if (Entry.is_directory(EntryError) == true)
            {
                if (IsSkippedDirectory(Entry.path().filename().string()) == true)
                {
                    Iterator.disable_recursion_pending();
                }
            }
            else if (Entry.is_regular_file(EntryError) == true)
            {
                Batch.push_back(fs::relative(Entry.path(), Root, EntryError).generic_string());
                Total++;
            }
            if (Batch.size() >= 500)
            {
                Flush(Batch);
            }
            Iterator.increment(ErrorCode);
        }
        Flush(Batch);
        Scanning_ = false;
    }

    void Flush(std::vector<std::string>& Batch)
    {
        std::lock_guard<std::mutex> const Guard(Mutex_);
        for (std::string& Path : Batch)
        {
            Lowered_.push_back(Lower(Path));
            Paths_.push_back(std::move(Path));
        }
        Batch.clear();
    }

    void Stop()
    {
        Cancel_ = true;
        if (Worker_.joinable() == true)
        {
            Worker_.join();
        }
    }

    mutable std::mutex Mutex_;
    std::thread Worker_;
    std::atomic<bool> Cancel_{false};
    std::atomic<bool> Scanning_{false};
    std::string Root_;
    std::vector<std::string> Paths_;
    std::vector<std::string> Lowered_;
};
