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
    static constexpr const char* TAG_PREFIX = "cs-";
    static constexpr size_t TAG_HEX_LENGTH = 8;

    // Short id that marks a session's screenshots ("cs-1a2b3c4d"), taken from the app's session id.
    static std::string SessionTag(const std::string& SessionId)
    {
        std::string Hex;
        for (const char Character : SessionId)
        {
            if (std::isxdigit(static_cast<unsigned char>(Character)) == 0)
            {
                continue;
            }
            Hex += static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
            if (Hex.size() == TAG_HEX_LENGTH)
            {
                break;
            }
        }
        return Hex.size() == TAG_HEX_LENGTH ? TAG_PREFIX + Hex : std::string();
    }

    // The session's own folder: <project>/ClaudeShots/<tag>.
    static std::filesystem::path SessionShotDirectory(const std::string& ProjectDir, const std::string& Tag)
    {
        return std::filesystem::path(ProjectDir) / SHOT_DIRECTORY_NAME / Tag;
    }

    // The last session tag appearing in a path (folder or file name), or empty.
    static std::string TagIn(const std::string& Path)
    {
        std::string Lower = Path;
        std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](const unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
        const std::string Prefix = TAG_PREFIX;
        std::string Found;
        size_t Position = 0;
        while ((Position = Lower.find(Prefix, Position)) != std::string::npos)
        {
            const size_t HexStart = Position + Prefix.size();
            Position = HexStart;
            if (HexStart + TAG_HEX_LENGTH > Lower.size())
            {
                break;
            }
            const std::string Hex = Lower.substr(HexStart, TAG_HEX_LENGTH);
            const bool AllHex = std::all_of(Hex.begin(), Hex.end(), [](const unsigned char Character) { return std::isxdigit(Character) != 0; });
            const size_t After = HexStart + TAG_HEX_LENGTH;
            const bool EndsTag = After == Lower.size() || std::isxdigit(static_cast<unsigned char>(Lower[After])) == 0;
            if (AllHex == true && EndsTag == true)
            {
                Found = Prefix + Hex;
            }
        }
        return Found;
    }

    // Live-view frames (<tag>_live.png) are overwritten every second, so they stay out of the screenshot feed.
    static bool IsLiveFrame(const std::string& Path)
    {
        const std::string Stem = std::filesystem::path(Path).stem().string();
        return EndsWith(Stem, "_live") == true || EndsWith(Stem, "_live_pending") == true;
    }

    static std::string Guidance(const std::string& ProjectDir, const std::string& Tag)
    {
        const std::string Directory = SessionShotDirectory(ProjectDir, Tag).string();
        return "Screenshots: this session's tag is " + Tag + ". Save every screenshot or image you capture or render to " + Directory +
               " (create the folder if needed) and start each file name with the tag, e.g. " + Tag + "_main-menu.png. If a tool saves an image "
               "somewhere else, copy it into that folder with the tagged name. When you mention a screenshot, give its full path. Never put "
               "another session's tag in your file names.";
    }

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
    static bool EndsWith(const std::string& Text, const std::string& Suffix)
    {
        return Text.size() >= Suffix.size() && Text.compare(Text.size() - Suffix.size(), Suffix.size(), Suffix) == 0;
    }

    static bool IsDelimiter(const char Character)
    {
        return std::isspace(static_cast<unsigned char>(Character)) != 0 || std::string("\"'`()<>[]|*,;").find(Character) != std::string::npos;
    }
};
