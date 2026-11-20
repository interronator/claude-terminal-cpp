#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Terminal.h"

// Finds a session name, id or tag written in a terminal row, so it can be hovered and clicked like a link.
class SessionMentions
{
public:
    void Add(const std::string& Text, const std::string& SessionId)
    {
        std::vector<uint32_t> Codes = Decode(Text);
        if (Codes.size() < MIN_LENGTH)
        {
            return;
        }
        Candidates_.push_back({std::move(Codes), SessionId});
    }

    // Session id of the longest candidate that covers the given column, or empty.
    std::string Find(const Terminal& Term, const int AbsRow, const int Col) const
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
        std::string BestId;
        size_t BestLength = 0;
        for (const Candidate& Current : Candidates_)
        {
            const size_t Length = Current.Codes.size();
            if (Length <= BestLength || Length > Line.size())
            {
                continue;
            }
            const size_t FirstStart = static_cast<size_t>(Col) + 1 > Length ? static_cast<size_t>(Col) + 1 - Length : 0;
            for (size_t Start = FirstStart; Start <= static_cast<size_t>(Col) && Start + Length <= Line.size(); Start++)
            {
                if (Matches(Line, Start, Current.Codes) == true)
                {
                    BestId = Current.SessionId;
                    BestLength = Length;
                    break;
                }
            }
        }
        return BestId;
    }

private:
    struct Candidate
    {
        std::vector<uint32_t> Codes;
        std::string SessionId;
    };

    static constexpr size_t MIN_LENGTH = 3;

    static bool IsWordCharacter(const uint32_t Ch)
    {
        return (Ch >= '0' && Ch <= '9') || (Ch >= 'A' && Ch <= 'Z') || (Ch >= 'a' && Ch <= 'z') || Ch == '-' || Ch == '_' || (Ch > 127 && Ch != Cell::CONTINUATION);
    }

    static bool Matches(const std::vector<uint32_t>& Line, const size_t Start, const std::vector<uint32_t>& Codes)
    {
        for (size_t Offset = 0; Offset < Codes.size(); Offset++)
        {
            if (Line[Start + Offset] != Codes[Offset])
            {
                return false;
            }
        }
        if (Start > 0 && IsWordCharacter(Line[Start - 1]) == true)
        {
            return false;
        }
        const size_t End = Start + Codes.size();
        return End >= Line.size() || IsWordCharacter(Line[End]) == false;
    }

    static std::vector<uint32_t> Decode(const std::string& Text)
    {
        std::vector<uint32_t> Codes;
        for (size_t Index = 0; Index < Text.size();)
        {
            const unsigned char Lead = static_cast<unsigned char>(Text[Index]);
            const size_t Extra = Lead >= 0xF0 ? 3 : Lead >= 0xE0 ? 2 : Lead >= 0xC0 ? 1 : 0;
            uint32_t Code = Extra == 0 ? Lead : Lead & (0x3F >> Extra);
            for (size_t Part = 1; Part <= Extra && Index + Part < Text.size(); Part++)
            {
                Code = (Code << 6) | (static_cast<unsigned char>(Text[Index + Part]) & 0x3F);
            }
            Codes.push_back(Code);
            Index += Extra + 1;
        }
        return Codes;
    }

    std::vector<Candidate> Candidates_;
};
