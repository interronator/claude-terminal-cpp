#pragma once
#include <string>

#include <windows.h>

class WinStrings
{
public:
    static std::wstring Widen(const std::string& Text)
    {
        if (Text.empty() == true)
        {
            return {};
        }
        const int Length = MultiByteToWideChar(CP_UTF8, 0, Text.data(), (int)Text.size(), nullptr, 0);
        std::wstring Wide((size_t)Length, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, Text.data(), (int)Text.size(), Wide.data(), Length);
        return Wide;
    }

    static std::wstring QuoteArg(const std::wstring& Arg)
    {
        if (Arg.empty() == false && Arg.find_first_of(L" \t\"") == std::wstring::npos)
        {
            return Arg;
        }
        std::wstring Quoted = L"\"";
        size_t Backslashes = 0;
        for (const wchar_t Character : Arg)
        {
            if (Character == L'\\')
            {
                Backslashes++;
                continue;
            }
            if (Character == L'"')
            {
                Quoted.append(Backslashes * 2 + 1, L'\\');
                Quoted += L'"';
                Backslashes = 0;
                continue;
            }
            Quoted.append(Backslashes, L'\\');
            Backslashes = 0;
            Quoted += Character;
        }
        Quoted.append(Backslashes * 2, L'\\');
        return Quoted + L"\"";
    }

    static std::string Narrow(const std::wstring& Wide)
    {
        if (Wide.empty() == true)
        {
            return {};
        }
        const int Length = WideCharToMultiByte(CP_UTF8, 0, Wide.data(), (int)Wide.size(), nullptr, 0, nullptr, nullptr);
        std::string Text((size_t)Length, '\0');
        WideCharToMultiByte(CP_UTF8, 0, Wide.data(), (int)Wide.size(), Text.data(), Length, nullptr, nullptr);
        return Text;
    }
};
