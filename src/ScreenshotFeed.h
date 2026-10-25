#pragma once
#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

// Every screenshot found for any session, newest first, each tagged with the session that produced it.
class ScreenshotFeed
{
public:
    struct Entry
    {
        std::string SessionId;
        std::string Path;
        std::time_t Time = 0;
    };

    static constexpr size_t MAX_ENTRIES = 200;

    const std::vector<Entry>& Entries() const { return Entries_; }

    // Each file appears once. A path seen again with a newer time (the file was overwritten) moves to the top instead of
    // duplicating. Owned means the file carries the session's tag, which overrides whichever session reported it first.
    bool Add(const std::string& SessionId, const std::string& Path, const std::time_t Time, const bool Owned)
    {
        for (Entry& Existing : Entries_)
        {
            if (Existing.Path != Path)
            {
                continue;
            }
            if (Owned == true)
            {
                Existing.SessionId = SessionId;
            }
            Existing.Time = std::max(Existing.Time, Time);
            Sort();
            return false;
        }
        Entries_.push_back({SessionId, Path, Time});
        Sort();
        if (Entries_.size() > MAX_ENTRIES)
        {
            const bool Survived = std::any_of(Entries_.begin(), Entries_.begin() + MAX_ENTRIES, [&](const Entry& Item) { return Item.SessionId == SessionId && Item.Path == Path; });
            Entries_.resize(MAX_ENTRIES);
            return Survived;
        }
        return true;
    }

    void RemoveSession(const std::string& SessionId)
    {
        Entries_.erase(std::remove_if(Entries_.begin(), Entries_.end(), [&](const Entry& Item) { return Item.SessionId == SessionId; }), Entries_.end());
    }

    void RemovePath(const std::string& Path)
    {
        Entries_.erase(std::remove_if(Entries_.begin(), Entries_.end(), [&](const Entry& Item) { return Item.Path == Path; }), Entries_.end());
    }

    size_t CountForSession(const std::string& SessionId) const
    {
        return static_cast<size_t>(std::count_if(Entries_.begin(), Entries_.end(), [&](const Entry& Item) { return Item.SessionId == SessionId; }));
    }

private:
    void Sort()
    {
        std::stable_sort(Entries_.begin(), Entries_.end(), [](const Entry& First, const Entry& Second) { return First.Time > Second.Time; });
    }

    std::vector<Entry> Entries_;
};
