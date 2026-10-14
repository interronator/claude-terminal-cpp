#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

// Picks image file paths out of free text and resolves them against a working directory.
class ScreenshotFinder
{
public:
    static std::vector<std::string> ExtractImagePaths(const std::string& Text)
    {
        static const std::vector<std::string> EXTENSIONS = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"};
        std::string Lower = Text;
        std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](const unsigned char Character) { return static_cast<char>(std::tolower(Character)); });

        std::vector<std::string> Paths;
        for (const std::string& Extension : EXTENSIONS)
        {
            size_t Position = 0;
            while ((Position = Lower.find(Extension, Position)) != std::string::npos)
            {
                const size_t End = Position + Extension.size();
                Position = End;
                if (End < Text.size() && std::isalnum(static_cast<unsigned char>(Text[End])) != 0)
                {
                    continue;
                }
                size_t Begin = End - Extension.size();
                while (Begin > 0 && IsDelimiter(Text[Begin - 1]) == false)
                {
                    Begin--;
                }
                const std::string Path = Text.substr(Begin, End - Begin);
                if (Path.size() > Extension.size())
                {
                    Paths.push_back(Path);
                }
            }
        }
        return Paths;
    }

    static constexpr const char* SHOT_DIRECTORY_NAME = "ClaudeShots";

    // Finds paths in the text that end at a directory named ClaudeShots (including ones that only prefix a file path).
    static std::vector<std::string> ExtractShotDirectories(const std::string& Text)
    {
        const std::string Marker = SHOT_DIRECTORY_NAME;
        std::string Lower = Text;
        std::string LowerMarker = Marker;
        std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](const unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
        std::transform(LowerMarker.begin(), LowerMarker.end(), LowerMarker.begin(), [](const unsigned char Character) { return static_cast<char>(std::tolower(Character)); });

        std::vector<std::string> Directories;
        size_t Position = 0;
        while ((Position = Lower.find(LowerMarker, Position)) != std::string::npos)
        {
            const size_t End = Position + Marker.size();
            Position = End;
            const bool EndsAtBoundary = End == Text.size() || Text[End] == '\\' || Text[End] == '/' || IsDelimiter(Text[End]) == true;
            if (EndsAtBoundary == false)
            {
                continue;
            }
            size_t Begin = End - Marker.size();
            while (Begin > 0 && IsDelimiter(Text[Begin - 1]) == false)
            {
                Begin--;
            }
            Directories.push_back(Text.substr(Begin, End - Begin));
        }
        return Directories;
    }

    // Returns the normalized absolute path if it names an existing file, otherwise an empty string.
    static std::string Resolve(const std::string& Candidate, const std::string& Cwd)
    {
        std::error_code ErrorCode;
        std::filesystem::path Path(Candidate);
        if (Path.is_relative() == true)
        {
            Path = std::filesystem::path(Cwd) / Path;
        }
        if (std::filesystem::is_regular_file(Path, ErrorCode) == false)
        {
            return {};
        }
        return Path.lexically_normal().string();
    }

private:
    static bool IsDelimiter(const char Character)
    {
        return std::isspace(static_cast<unsigned char>(Character)) != 0 || std::string("\"'`()<>[]|*,;").find(Character) != std::string::npos;
    }
};
