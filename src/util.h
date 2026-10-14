#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

class Util
{
public:
    static std::string Uuid4()
    {
        static std::mt19937_64 Rng{std::random_device{}()};
        uint64_t HighBits = Rng();
        uint64_t LowBits = Rng();
        HighBits = (HighBits & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // version 4
        LowBits = (LowBits & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;    // variant 10xx
        char Buffer[40];
        std::snprintf(Buffer, sizeof Buffer, "%08x-%04x-%04x-%04x-%012llx", (unsigned)(HighBits >> 32),
                      (unsigned)((HighBits >> 16) & 0xFFFF), (unsigned)(HighBits & 0xFFFF), (unsigned)(LowBits >> 48),
                      (unsigned long long)(LowBits & 0xFFFFFFFFFFFFULL));
        return Buffer;
    }

    // Splits on whitespace, honoring simple "double" and 'single' quotes.
    static std::vector<std::string> SplitCommand(const std::string& Command)
    {
        std::vector<std::string> Tokens;
        std::string Current;
        bool InToken = false;
        char Quote = 0;
        for (char const Character : Command)
        {
            if (Quote != 0)
            {
                if (Character == Quote)
                {
                    Quote = 0;
                }
                else
                {
                    Current += Character;
                }
                continue;
            }
            if (Character == '"' || Character == '\'')
            {
                Quote = Character;
                InToken = true;
                continue;
            }
            if (std::isspace((unsigned char)Character) != 0)
            {
                if (InToken)
                {
                    Tokens.push_back(Current);
                    Current.clear();
                    InToken = false;
                }
                continue;
            }
            Current += Character;
            InToken = true;
        }
        if (InToken)
        {
            Tokens.push_back(Current);
        }
        return Tokens;
    }

    static std::string Utf8(uint32_t CodePoint)
    {
        std::string Out;
        if (CodePoint < 0x80)
        {
            Out += (char)CodePoint;
        }
        else if (CodePoint < 0x800)
        {
            Out += (char)(0xC0 | (CodePoint >> 6));
            Out += (char)(0x80 | (CodePoint & 0x3F));
        }
        else if (CodePoint < 0x10000)
        {
            Out += (char)(0xE0 | (CodePoint >> 12));
            Out += (char)(0x80 | ((CodePoint >> 6) & 0x3F));
            Out += (char)(0x80 | (CodePoint & 0x3F));
        }
        else
        {
            Out += (char)(0xF0 | (CodePoint >> 18));
            Out += (char)(0x80 | ((CodePoint >> 12) & 0x3F));
            Out += (char)(0x80 | ((CodePoint >> 6) & 0x3F));
            Out += (char)(0x80 | (CodePoint & 0x3F));
        }
        return Out;
    }

    static std::string Trim(const std::string& Value)
    {
        size_t Start = 0;
        size_t End = Value.size();
        while (Start < End && std::isspace((unsigned char)Value[Start]) != 0)
        {
            Start++;
        }
        while (End > Start && std::isspace((unsigned char)Value[End - 1]) != 0)
        {
            End--;
        }
        return Value.substr(Start, End - Start);
    }

    static std::string Slug(const std::string& Value)
    {
        std::string Out;
        bool Dash = false;
        for (unsigned char const Character : Value)
        {
            if (std::isalnum(Character) != 0)
            {
                Out += (char)std::tolower(Character);
                Dash = false;
                continue;
            }
            if (Dash == false && Out.empty() == false)
            {
                Out += '-';
                Dash = true;
            }
        }
        while (Out.empty() == false && Out.back() == '-')
        {
            Out.pop_back();
        }
        if (Out.size() > 40)
        {
            Out.resize(40);
        }
        return Out;
    }

    // Id of the most recently written Claude conversation for a folder, or empty if there is none.
    static std::string LatestClaudeSessionId(const std::string& Cwd)
    {
        std::filesystem::path ConfigDir;
        const char* const ConfigOverride = std::getenv("CLAUDE_CONFIG_DIR");
#ifdef _WIN32
        const char* const Home = std::getenv("USERPROFILE");
#else
        const char* const Home = std::getenv("HOME");
#endif
        if (ConfigOverride != nullptr)
        {
            ConfigDir = ConfigOverride;
        }
        else if (Home != nullptr)
        {
            ConfigDir = std::filesystem::path(Home) / ".claude";
        }
        else
        {
            return "";
        }

        std::string Encoded;
        for (unsigned char const Character : Cwd)
        {
            Encoded += std::isalnum(Character) != 0 ? (char)Character : '-';
        }

        std::error_code ErrorCode;
        std::filesystem::directory_iterator Iterator(ConfigDir / "projects" / Encoded, ErrorCode);
        if (ErrorCode.value() != 0)
        {
            return "";
        }
        std::string BestId;
        std::filesystem::file_time_type BestTime{};
        for (const std::filesystem::directory_entry& Entry : Iterator)
        {
            if (Entry.path().extension() != ".jsonl")
            {
                continue;
            }
            std::filesystem::file_time_type const WriteTime = Entry.last_write_time(ErrorCode);
            if (ErrorCode.value() != 0)
            {
                continue;
            }
            if (BestId.empty() == false && WriteTime <= BestTime)
            {
                continue;
            }
            BestId = Entry.path().stem().string();
            BestTime = WriteTime;
        }
        return BestId;
    }

    static std::string BaseName(const std::string& Path)
    {
        std::filesystem::path FsPath(Path);
        if (FsPath.filename().empty())
        {
            FsPath = FsPath.parent_path();
        }
        std::string const Name = FsPath.filename().string();
        return Name.empty() ? Path : Name;
    }
};
