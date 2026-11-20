#pragma once

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// The last few lines of background shell output files, re-read at most once a second and only while someone asks for them.
class ShellOutputTail
{
public:
    const std::vector<std::string>& Lines(const std::string& Path, const double Now)
    {
        constexpr double CHECK_INTERVAL_SECONDS = 1.0;
        constexpr double FORGET_AFTER_SECONDS = 30.0;
        Forget(Now, FORGET_AFTER_SECONDS);
        Entry& Current = Entries_[Path];
        Current.LastAskedAt = Now;
        if (Now >= Current.CheckAt)
        {
            Current.CheckAt = Now + CHECK_INTERVAL_SECONDS;
            Current.Lines = Read(Path);
        }
        return Current.Lines;
    }

private:
    struct Entry
    {
        double CheckAt = 0;
        double LastAskedAt = 0;
        std::vector<std::string> Lines;
    };

    // A carriage return rewrites the line, so only what follows the last one is visible.
    static void FinishLine(std::vector<std::string>& Lines, std::string& Line)
    {
        const size_t LastReturn = Line.rfind('\r');
        const std::string Shown = LastReturn == std::string::npos ? Line : Line.substr(LastReturn + 1);
        Line.clear();
        if (Shown.empty() == false)
        {
            Lines.push_back(Shown);
        }
    }

    static std::vector<std::string> Read(const std::string& Path)
    {
        constexpr std::streamoff TAIL_BYTES = 16 * 1024;
        constexpr size_t MAX_LINES = 20;
        std::ifstream File(Path, std::ios::binary);
        if (File.is_open() == false)
        {
            return {};
        }
        File.seekg(0, std::ios::end);
        const std::streamoff Size = File.tellg();
        const std::streamoff Start = std::max<std::streamoff>(0, Size - TAIL_BYTES);
        File.seekg(Start);
        std::string Text((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
        if (Start > 0)
        {
            const size_t FirstBreak = Text.find('\n');
            Text = FirstBreak == std::string::npos ? std::string() : Text.substr(FirstBreak + 1);
        }

        std::vector<std::string> Lines;
        std::string Line;
        for (size_t Index = 0; Index < Text.size(); Index++)
        {
            const char Letter = Text[Index];
            if (Letter == '\n')
            {
                FinishLine(Lines, Line);
                continue;
            }
            if (Letter == '\x1b' && Index + 1 < Text.size() && Text[Index + 1] == '[')
            {
                Index += 2;
                while (Index < Text.size() && (Text[Index] < '@' || Text[Index] > '~'))
                {
                    Index++;
                }
                continue;
            }
            Line.push_back(Letter);
        }
        FinishLine(Lines, Line);
        if (Lines.size() > MAX_LINES)
        {
            Lines.erase(Lines.begin(), Lines.end() - static_cast<std::ptrdiff_t>(MAX_LINES));
        }
        return Lines;
    }

    void Forget(const double Now, const double Age)
    {
        for (std::map<std::string, Entry>::iterator Item = Entries_.begin(); Item != Entries_.end();)
        {
            if (Now - Item->second.LastAskedAt > Age && Item->second.LastAskedAt > 0)
            {
                Item = Entries_.erase(Item);
                continue;
            }
            ++Item;
        }
    }

    std::map<std::string, Entry> Entries_;
};
