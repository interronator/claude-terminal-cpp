#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <string>
#include <system_error>
#include <vector>

#include "Platform.h"
#include "Terminal.h"

class PathLinks
{
public:
    static std::string FindExistingPath(const Terminal& Term, const int AbsRow, const int Col)
    {
        const std::string Token = TokenAt(Term, AbsRow, Col);
        if (LooksLikeWindowsPath(Token) == false)
        {
            return std::string();
        }
        const std::shared_ptr<Cache>& Shared = SharedCache();
        const std::chrono::steady_clock::time_point Now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> Lock(Shared->Mutex);
        Entry& Found = Shared->Entries[Token];
        if (Found.Pending == true)
        {
            return Found.Result;
        }
        if (Found.Valid == true && Now - Found.Stamp < EXPIRY)
        {
            return Found.Result;
        }
        if (Shared->Entries.size() > MAX_ENTRIES)
        {
            for (std::unordered_map<std::string, Entry>::iterator Iter = Shared->Entries.begin(); Iter != Shared->Entries.end();)
            {
                if (Iter->second.Pending == false && Iter->first != Token)
                {
                    Iter = Shared->Entries.erase(Iter);
                    continue;
                }
                ++Iter;
            }
        }
        Found.Pending = true;
        const std::shared_ptr<Cache> Keep = Shared;
        std::thread(
            [Keep, Token]()
            {
                const std::string Resolved = Resolve(Token);
                std::lock_guard<std::mutex> Inner(Keep->Mutex);
                Entry& Done = Keep->Entries[Token];
                Done.Result = Resolved;
                Done.Stamp = std::chrono::steady_clock::now();
                Done.Valid = true;
                Done.Pending = false;
            })
            .detach();
        return Found.Result;
    }

    static void Open(const std::string& Path)
    {
        std::error_code ErrorCode;
        if (std::filesystem::is_directory(std::filesystem::path(Path), ErrorCode) == true)
        {
            Platform::Reveal(Path);
            return;
        }
        Platform::ShowInFolder(Path);
    }

private:
    struct Entry
    {
        std::string Result;
        std::chrono::steady_clock::time_point Stamp;
        bool Valid = false;
        bool Pending = false;
    };

    struct Cache
    {
        std::mutex Mutex;
        std::unordered_map<std::string, Entry> Entries;
    };

    static constexpr std::chrono::seconds EXPIRY{5};
    static constexpr size_t MAX_ENTRIES = 256;

    static const std::shared_ptr<Cache>& SharedCache()
    {
        static const std::shared_ptr<Cache> Instance = std::make_shared<Cache>();
        return Instance;
    }

    static std::string Resolve(const std::string& Token)
    {
        std::string Candidate = TrimTrailing(Token);
        while (Candidate.size() > 3)
        {
            std::error_code ErrorCode;
            if (std::filesystem::exists(std::filesystem::path(Candidate), ErrorCode) == true)
            {
                return Candidate;
            }
            const size_t Colon = Candidate.find_last_of(':');
            if (Colon == std::string::npos || Colon < 2)
            {
                break;
            }
            Candidate = TrimTrailing(Candidate.substr(0, Colon));
        }
        return std::string();
    }

    static bool IsSeparator(const uint32_t Ch)
    {
        if (Ch == 0 || Ch == Cell::CONTINUATION)
        {
            return true;
        }
        return Ch <= ' ' || Ch == '"' || Ch == '\'' || Ch == '`' || Ch == '<' || Ch == '>' || Ch == '|' || Ch == '*' || Ch == '?' || Ch == ',' ||
               Ch == '(' || Ch == ')' || Ch == '[' || Ch == ']';
    }

    static bool LooksLikeWindowsPath(const std::string& Token)
    {
        if (Token.size() < 3)
        {
            return false;
        }
        const char Drive = Token[0];
        const bool IsLetter = (Drive >= 'A' && Drive <= 'Z') || (Drive >= 'a' && Drive <= 'z');
        return IsLetter == true && Token[1] == ':' && (Token[2] == '\\' || Token[2] == '/');
    }

    static std::string TrimTrailing(std::string Text)
    {
        while (Text.empty() == false && (Text.back() == '.' || Text.back() == ':' || Text.back() == ';' || Text.back() == '!'))
        {
            Text.pop_back();
        }
        return Text;
    }

    static void AppendUtf8(std::string& Out, const uint32_t Ch)
    {
        if (Ch < 0x80)
        {
            Out.push_back(static_cast<char>(Ch));
            return;
        }
        if (Ch < 0x800)
        {
            Out.push_back(static_cast<char>(0xC0 | (Ch >> 6)));
            Out.push_back(static_cast<char>(0x80 | (Ch & 0x3F)));
            return;
        }
        if (Ch < 0x10000)
        {
            Out.push_back(static_cast<char>(0xE0 | (Ch >> 12)));
            Out.push_back(static_cast<char>(0x80 | ((Ch >> 6) & 0x3F)));
            Out.push_back(static_cast<char>(0x80 | (Ch & 0x3F)));
            return;
        }
        Out.push_back(static_cast<char>(0xF0 | (Ch >> 18)));
        Out.push_back(static_cast<char>(0x80 | ((Ch >> 12) & 0x3F)));
        Out.push_back(static_cast<char>(0x80 | ((Ch >> 6) & 0x3F)));
        Out.push_back(static_cast<char>(0x80 | (Ch & 0x3F)));
    }

    static std::string TokenAt(const Terminal& Term, const int AbsRow, const int Col)
    {
        if (AbsRow < 0 || Col < 0 || Col >= Term.Cols())
        {
            return std::string();
        }
        std::vector<uint32_t> Line;
        Line.reserve(static_cast<size_t>(Term.Cols()));
        for (int Index = 0; Index < Term.Cols(); Index++)
        {
            Line.push_back(Term.GetCell(AbsRow, Index).Ch);
        }
        if (IsSeparator(Line[static_cast<size_t>(Col)]) == true)
        {
            return std::string();
        }
        int First = Col;
        while (First > 0 && IsSeparator(Line[static_cast<size_t>(First - 1)]) == false)
        {
            First--;
        }
        int Last = Col;
        while (Last + 1 < Term.Cols() && IsSeparator(Line[static_cast<size_t>(Last + 1)]) == false)
        {
            Last++;
        }
        std::string Token;
        for (int Index = First; Index <= Last; Index++)
        {
            AppendUtf8(Token, Line[static_cast<size_t>(Index)]);
        }
        return Token;
    }
};
