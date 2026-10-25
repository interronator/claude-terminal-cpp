#pragma once
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "util.h"

// A per-project PROJECT_LOG.md that every Claude session appends a line to whenever it finishes a piece of work,
// plus a free-form notes section the user edits. Entries look like
// "- 2026-10-20 14:32 | Session name | Work type | What was done" (older lines have no work type).
class ProjectLog
{
public:
    enum class Grouping { Hour, Day, Week };

    struct Entry
    {
        std::time_t When = 0;
        std::string Stamp;
        std::string SessionName;
        std::string Type;
        std::string Text;
    };

    struct TypeCount
    {
        std::string Type;
        int Count = 0;
    };

    static constexpr const char* UNTYPED = "Other";
    static constexpr const char* WORK_TYPES =
        "Coding, New feature, Bug fix, Optimization, Art, Sound, Level design, UI, Research, Testing, Build and tools, Documentation";

    struct Group
    {
        std::string Title;
        std::vector<Entry> Entries;
    };

    static constexpr const char* FILE_NAME = "PROJECT_LOG.md";
    static constexpr const char* NOTES_HEADING = "## Notes";
    static constexpr const char* LOG_HEADING = "## Log";

    static std::filesystem::path PathFor(const std::string& ProjectDir)
    {
        return std::filesystem::path(ProjectDir) / FILE_NAME;
    }

    static std::string Guidance(const std::string& ProjectDir, const std::string& SessionName)
    {
        const std::string Path = PathFor(ProjectDir).string();
        return "Project log: " + Path + " records what has been done in this project. Read its Notes section when you start, for context the user left. "
               "Every time you finish a piece of work (a change, a fix, a finding, a build or test result), append one line to the end of that file "
               "in exactly this format: \"- YYYY-MM-DD HH:MM | " + SessionName + " | <work type> | <one short plain-language sentence saying what was done>\", "
               "using the current local date and time. The work type is the single best fit from: " + WORK_TYPES +
               " (use another short name only when none of these fit, and spell types the same way every time). Only append; never rewrite, reorder or remove existing lines, and never edit the Notes section. "
               "If the file does not exist, create it with the lines \"# Project log\", \"" + NOTES_HEADING + "\", \"" + LOG_HEADING + "\" (each followed by "
               "a blank line) before appending.";
    }

    // Reloads from disk when the file changed since the last load. Returns true when the contents were reloaded.
    bool Refresh(const std::string& ProjectDir)
    {
        const std::filesystem::path Path = PathFor(ProjectDir);
        std::error_code ErrorCode;
        const bool Exists = std::filesystem::exists(Path, ErrorCode);
        const std::filesystem::file_time_type Modified = Exists == true ? std::filesystem::last_write_time(Path, ErrorCode) : std::filesystem::file_time_type{};
        if (ProjectDir == LoadedDir_ && Exists == LoadedExists_ && Modified == LoadedModified_)
        {
            return false;
        }
        LoadedDir_ = ProjectDir;
        LoadedExists_ = Exists;
        LoadedModified_ = Modified;
        Parse(Exists == true ? Util::ReadFile(Path) : std::string());
        return true;
    }

    bool FileExists() const { return LoadedExists_; }
    const std::string& Notes() const { return Notes_; }
    const std::vector<Entry>& Entries() const { return Entries_; }
    const std::vector<TypeCount>& AllTimeCounts() const { return AllTimeCounts_; }

    // Replaces the notes section while keeping every log line, re-reading the file first so lines sessions appended
    // since the last load are not lost.
    bool SaveNotes(const std::string& ProjectDir, const std::string& NewNotes)
    {
        const std::filesystem::path Path = PathFor(ProjectDir);
        std::string LogPart;
        std::error_code ErrorCode;
        if (std::filesystem::exists(Path, ErrorCode) == true)
        {
            const std::string Text = Util::ReadFile(Path);
            const size_t LogAt = FindHeading(Text, LOG_HEADING);
            if (LogAt != std::string::npos)
            {
                LogPart = Text.substr(LogAt);
            }
        }
        if (LogPart.empty() == true)
        {
            LogPart = std::string(LOG_HEADING) + "\n\n";
        }
        const std::string Body = TrimEnds(NewNotes);
        const std::string Content = "# Project log\n\n" + std::string(NOTES_HEADING) + "\n\n" + (Body.empty() == true ? std::string() : Body + "\n\n") + LogPart;
        std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
        if (Stream.is_open() == false)
        {
            return false;
        }
        Stream << Content;
        Stream.close();
        LoadedDir_.clear();
        Refresh(ProjectDir);
        return true;
    }

    // Newest group first, newest entry first within each group.
    std::vector<Group> Grouped(const Grouping By) const
    {
        std::vector<Group> Groups;
        for (std::vector<Entry>::const_reverse_iterator Item = Entries_.rbegin(); Item != Entries_.rend(); ++Item)
        {
            const std::string Title = GroupTitle(*Item, By);
            if (Groups.empty() == true || Groups.back().Title != Title)
            {
                Groups.push_back({Title, {}});
            }
            Groups.back().Entries.push_back(*Item);
        }
        return Groups;
    }

    // Entries at or after Since, counted per work type, largest first.
    std::vector<TypeCount> CountByType(const std::time_t Since) const
    {
        std::vector<TypeCount> Counts;
        for (const Entry& Item : Entries_)
        {
            if (Item.When < Since)
            {
                continue;
            }
            const std::string Type = Item.Type.empty() == true ? UNTYPED : Item.Type;
            std::vector<TypeCount>::iterator Found =
                std::find_if(Counts.begin(), Counts.end(), [&Type](const TypeCount& Candidate) { return Candidate.Type == Type; });
            if (Found == Counts.end())
            {
                Counts.push_back({Type, 1});
                continue;
            }
            Found->Count++;
        }
        std::stable_sort(Counts.begin(), Counts.end(), [](const TypeCount& Left, const TypeCount& Right) { return Left.Count > Right.Count; });
        return Counts;
    }

    struct Bucket
    {
        std::string Label;     // short axis label ("14", "Oct 26")
        std::string Title;     // full label for tooltips
        std::vector<TypeCount> Types;
        int Total = 0;
    };

    // The last Count days (or months) ending today, oldest first, with each one's entries counted per work type.
    std::vector<Bucket> Buckets(const bool Monthly, const int Count) const
    {
        std::vector<Bucket> Result(static_cast<size_t>(Count));
        std::vector<std::string> Keys(static_cast<size_t>(Count));
        const std::time_t Now = std::time(nullptr);
        const std::tm Today = LocalTime(Now);
        for (int Index = 0; Index < Count; Index++)
        {
            std::tm Start = Today;
            Start.tm_hour = 12;
            Start.tm_min = 0;
            Start.tm_sec = 0;
            Start.tm_isdst = -1;
            const int Back = Count - 1 - Index;
            if (Monthly == true)
            {
                Start.tm_mday = 1;
                Start.tm_mon -= Back;
            }
            else
            {
                Start.tm_mday -= Back;
            }
            std::mktime(&Start);
            Keys[static_cast<size_t>(Index)] = BucketKey(Start, Monthly);
            char Buffer[64];
            Bucket& Current = Result[static_cast<size_t>(Index)];
            std::strftime(Buffer, sizeof Buffer, Monthly == true ? "%b %y" : "%d", &Start);
            Current.Label = Buffer;
            std::strftime(Buffer, sizeof Buffer, Monthly == true ? "%B %Y" : "%a %Y-%m-%d", &Start);
            Current.Title = Buffer;
        }
        for (const Entry& Item : Entries_)
        {
            const std::vector<std::string>::const_iterator Found = std::find(Keys.begin(), Keys.end(), BucketKey(LocalTime(Item.When), Monthly));
            if (Found == Keys.end())
            {
                continue;
            }
            Bucket& Target = Result[static_cast<size_t>(Found - Keys.begin())];
            const std::string Type = Item.Type.empty() == true ? UNTYPED : Item.Type;
            Target.Total++;
            std::vector<TypeCount>::iterator Existing =
                std::find_if(Target.Types.begin(), Target.Types.end(), [&Type](const TypeCount& Candidate) { return Candidate.Type == Type; });
            if (Existing == Target.Types.end())
            {
                Target.Types.push_back({Type, 1});
                continue;
            }
            Existing->Count++;
        }
        return Result;
    }

    // Local midnight today, or of this week's Monday.
    static std::time_t StartOfToday(const bool WholeWeek)
    {
        const std::time_t Now = std::time(nullptr);
        std::tm Local{};
#ifdef _WIN32
        localtime_s(&Local, &Now);
#else
        localtime_r(&Now, &Local);
#endif
        if (WholeWeek == true)
        {
            Local.tm_mday -= (Local.tm_wday + 6) % 7;
        }
        Local.tm_hour = 0;
        Local.tm_min = 0;
        Local.tm_sec = 0;
        Local.tm_isdst = -1;
        return std::mktime(&Local);
    }

private:
    static std::tm LocalTime(const std::time_t When)
    {
        std::tm Local{};
#ifdef _WIN32
        localtime_s(&Local, &When);
#else
        localtime_r(&When, &Local);
#endif
        return Local;
    }

    static std::string BucketKey(const std::tm& Time, const bool Monthly)
    {
        char Buffer[16];
        std::strftime(Buffer, sizeof Buffer, Monthly == true ? "%Y-%m" : "%Y-%m-%d", &Time);
        return Buffer;
    }

    static std::string TrimEnds(const std::string& Text)
    {
        const size_t Begin = Text.find_first_not_of(" \t\r\n");
        if (Begin == std::string::npos)
        {
            return {};
        }
        const size_t End = Text.find_last_not_of(" \t\r\n");
        return Text.substr(Begin, End - Begin + 1);
    }

    // Offset of a line that is exactly Heading, or npos.
    static size_t FindHeading(const std::string& Text, const std::string& Heading)
    {
        size_t LineStart = 0;
        while (LineStart < Text.size())
        {
            size_t LineEnd = Text.find('\n', LineStart);
            if (LineEnd == std::string::npos)
            {
                LineEnd = Text.size();
            }
            if (TrimEnds(Text.substr(LineStart, LineEnd - LineStart)) == Heading)
            {
                return LineStart;
            }
            LineStart = LineEnd + 1;
        }
        return std::string::npos;
    }

    void Parse(const std::string& Text)
    {
        Entries_.clear();
        Notes_.clear();
        const size_t NotesAt = FindHeading(Text, NOTES_HEADING);
        const size_t LogAt = FindHeading(Text, LOG_HEADING);
        if (NotesAt != std::string::npos)
        {
            const size_t NotesBegin = Text.find('\n', NotesAt);
            const size_t NotesEnd = LogAt != std::string::npos && LogAt > NotesAt ? LogAt : Text.size();
            if (NotesBegin != std::string::npos && NotesBegin < NotesEnd)
            {
                Notes_ = TrimEnds(Text.substr(NotesBegin + 1, NotesEnd - NotesBegin - 1));
            }
        }

        std::istringstream Stream(Text);
        std::string Line;
        while (std::getline(Stream, Line).fail() == false)
        {
            Entry Parsed;
            if (ParseEntry(Line, Parsed) == true)
            {
                Entries_.push_back(std::move(Parsed));
            }
        }
        std::stable_sort(Entries_.begin(), Entries_.end(), [](const Entry& Left, const Entry& Right) { return Left.When < Right.When; });
        AllTimeCounts_ = CountByType(0);
    }

    static bool ParseEntry(const std::string& Line, Entry& Parsed)
    {
        int Year = 0;
        int Month = 0;
        int Day = 0;
        int Hour = 0;
        int Minute = 0;
        if (std::sscanf(Line.c_str(), "- %d-%d-%d %d:%d", &Year, &Month, &Day, &Hour, &Minute) != 5)
        {
            return false;
        }
        const size_t FirstBar = Line.find('|');
        if (FirstBar == std::string::npos)
        {
            return false;
        }
        const size_t SecondBar = Line.find('|', FirstBar + 1);
        std::tm Time{};
        Time.tm_year = Year - 1900;
        Time.tm_mon = Month - 1;
        Time.tm_mday = Day;
        Time.tm_hour = Hour;
        Time.tm_min = Minute;
        Time.tm_isdst = -1;
        Parsed.When = std::mktime(&Time);
        Parsed.Stamp = TrimEnds(Line.substr(2, FirstBar - 2));
        if (SecondBar == std::string::npos)
        {
            Parsed.Text = TrimEnds(Line.substr(FirstBar + 1));
            return true;
        }
        Parsed.SessionName = TrimEnds(Line.substr(FirstBar + 1, SecondBar - FirstBar - 1));
        const size_t ThirdBar = Line.find('|', SecondBar + 1);
        if (ThirdBar == std::string::npos)
        {
            Parsed.Text = TrimEnds(Line.substr(SecondBar + 1));
            return true;
        }
        Parsed.Type = NormalizeType(TrimEnds(Line.substr(SecondBar + 1, ThirdBar - SecondBar - 1)));
        Parsed.Text = TrimEnds(Line.substr(ThirdBar + 1));
        return true;
    }

    // Capitalizes the first letter so "coding" and "Coding" count as one slice.
    static std::string NormalizeType(std::string Type)
    {
        if (Type.empty() == false)
        {
            Type[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(Type[0])));
        }
        return Type;
    }

    static std::string GroupTitle(const Entry& Item, const Grouping By)
    {
        std::tm Local{};
#ifdef _WIN32
        localtime_s(&Local, &Item.When);
#else
        localtime_r(&Item.When, &Local);
#endif
        char Buffer[64];
        if (By == Grouping::Hour)
        {
            std::strftime(Buffer, sizeof Buffer, "%a %Y-%m-%d  %H:00", &Local);
            return Buffer;
        }
        if (By == Grouping::Day)
        {
            std::strftime(Buffer, sizeof Buffer, "%A %Y-%m-%d", &Local);
            return Buffer;
        }
        const int DaysSinceMonday = (Local.tm_wday + 6) % 7;
        std::tm WeekStart = Local;
        WeekStart.tm_mday -= DaysSinceMonday;
        WeekStart.tm_hour = 12;
        WeekStart.tm_isdst = -1;
        std::mktime(&WeekStart);
        std::strftime(Buffer, sizeof Buffer, "Week of %Y-%m-%d", &WeekStart);
        return Buffer;
    }

    std::string LoadedDir_;
    bool LoadedExists_ = false;
    std::filesystem::file_time_type LoadedModified_{};
    std::string Notes_;
    std::vector<Entry> Entries_;
    std::vector<TypeCount> AllTimeCounts_;
};
