#pragma once
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Git.h"
#include "nlohmann/json.hpp"

// A project's RAM budget, shared with every Claude session on this PC through RamCheck.ps1 and its RamProjects.json
// (in ~/.claude/tools). Dynamic mode reserves what RamCheck has learned the project's jobs use; Fixed mode makes every
// heavy job in the project reserve the same amount.
class RamSettings
{
public:
    struct TaskEstimate
    {
        std::string Name;
        double Gb = 0;
        std::string Source;
    };

    struct Snapshot
    {
        std::string Error;
        bool Fixed = false;
        double FixedGb = 0;
        double TotalGb = 0;
        double AvailableGb = 0;
        std::vector<TaskEstimate> Tasks;
    };

    static std::filesystem::path ToolsDir()
    {
        const char* const Home = std::getenv("USERPROFILE") != nullptr ? std::getenv("USERPROFILE") : std::getenv("HOME");
        return std::filesystem::path(Home != nullptr ? Home : ".") / ".claude" / "tools";
    }

    static const char* TaskLabel(const std::string& Name)
    {
        static const std::pair<const char*, const char*> LABELS[] = {
            {"Editor", "Unreal editor"},          {"EditorBigMap", "Unreal editor, big map"}, {"PlayCheck", "Play-in-editor check"},
            {"Headless", "Scripted editor run"},  {"Commandlet", "Unreal commandlet"},        {"CppBuild", "C++ build"},
            {"GameWindow", "Game window (each)"}, {"Blender", "Blender"},                     {"BlenderBatch", "Blender script"}};
        for (const std::pair<const char*, const char*>& Label : LABELS)
        {
            if (Name == Label.first)
            {
                return Label.second;
            }
        }
        return Name.c_str();
    }

    // Runs RamCheck.ps1 -Json for the project. Blocks for a second or two, so call it off the UI thread.
    static Snapshot Query(const std::string& ProjectPath)
    {
        Snapshot Result;
#ifndef _WIN32
        Result.Error = "RAM budgeting uses RamCheck.ps1, which only runs on Windows.";
        return Result;
#else
        const std::filesystem::path Script = ToolsDir() / "RamCheck.ps1";
        std::error_code ErrorCode;
        if (std::filesystem::exists(Script, ErrorCode) == false)
        {
            Result.Error = "RamCheck.ps1 was not found at " + Script.string() + ".";
            return Result;
        }
        const ProcResult Output =
            Git::RunProcess({"powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", Script.string(), "-Project", ProjectPath, "-Json"}, ProjectPath);
        const size_t Start = Output.Output.find('{');
        const nlohmann::json Root = Start == std::string::npos ? nlohmann::json() : nlohmann::json::parse(Output.Output.substr(Start), nullptr, false);
        if (Output.Code != 0 || Root.is_object() == false)
        {
            Result.Error = "RamCheck.ps1 did not answer: " + Output.Output.substr(0, Output.Output.find('\n'));
            return Result;
        }
        Result.Fixed = Root.value("Mode", std::string()) == "Fixed";
        Result.FixedGb = Root.value("FixedGb", 0.0);
        Result.TotalGb = Root.value("TotalGb", 0.0);
        Result.AvailableGb = Root.value("AvailableGb", 0.0);
        if (Root.contains("Tasks") == true && Root["Tasks"].is_array() == true)
        {
            for (const nlohmann::json& Task : Root["Tasks"])
            {
                Result.Tasks.push_back({Task.value("Name", std::string()), Task.value("Gb", 0.0), Task.value("Source", std::string())});
            }
        }
        return Result;
#endif
    }

    // Writes Mode/FixedGb for the project into RamProjects.json, keeping every other entry and setting as it was.
    static bool Save(const std::string& ProjectPath, const bool Fixed, const double FixedGb, std::string& Error)
    {
        const std::filesystem::path File = ToolsDir() / "RamProjects.json";
        nlohmann::ordered_json Root = nlohmann::ordered_json::object();
        std::error_code ErrorCode;
        if (std::filesystem::exists(File, ErrorCode) == true)
        {
            std::ifstream Stream(File, std::ios::binary);
            std::stringstream Buffer;
            Buffer << Stream.rdbuf();
            std::string Text = Buffer.str();
            if (Text.rfind("\xEF\xBB\xBF", 0) == 0)
            {
                Text.erase(0, 3);  // PowerShell writes UTF-8 with a byte-order mark
            }
            Root = nlohmann::ordered_json::parse(Text, nullptr, false);
            if (Root.is_object() == false)
            {
                Error = "Could not read " + File.string() + "; fix or delete it, then save again.";
                return false;
            }
        }
        const std::string Wanted = NormalizeKey(ProjectPath);
        std::string Key;
        nlohmann::ordered_json Inherited = nlohmann::ordered_json::object();
        size_t InheritedLength = 0;
        for (nlohmann::ordered_json::const_iterator Entry = Root.cbegin(); Entry != Root.cend(); ++Entry)
        {
            const std::string Candidate = NormalizeKey(Entry.key());
            if (Candidate == Wanted)
            {
                Key = Entry.key();
                break;
            }
            // A parent folder's entry (its Scale and task values) would stop applying once the project has its own entry, so start from a copy.
            const bool IsParent = Wanted.size() > Candidate.size() && Wanted.compare(0, Candidate.size(), Candidate) == 0 && Wanted[Candidate.size()] == '/';
            if (IsParent == true && Entry.value().is_object() == true && Candidate.size() > InheritedLength)
            {
                Inherited = Entry.value();
                InheritedLength = Candidate.size();
            }
        }
        if (Key.empty() == true)
        {
            Key = ProjectPath;
            std::replace(Key.begin(), Key.end(), '\\', '/');
            Root[Key] = Inherited;
        }
        nlohmann::ordered_json& Values = Root[Key];
        Values["Mode"] = Fixed == true ? "Fixed" : "Dynamic";
        Values["FixedGb"] = FixedGb;

        std::filesystem::create_directories(File.parent_path(), ErrorCode);
        const std::filesystem::path Temp = File.string() + ".tmp";
        {
            std::ofstream Stream(Temp, std::ios::binary | std::ios::trunc);
            Stream << Root.dump(4) << "\n";
            if (Stream.good() == false)
            {
                Error = "Could not write " + Temp.string() + ".";
                return false;
            }
        }
        std::filesystem::rename(Temp, File, ErrorCode);
        if (ErrorCode.value() != 0)
        {
            Error = "Could not replace " + File.string() + ": " + ErrorCode.message();
            return false;
        }
        return true;
    }

private:
    // RamCheck matches folders case-insensitively with either slash, so compare keys the same way.
    static std::string NormalizeKey(std::string Path)
    {
        std::replace(Path.begin(), Path.end(), '\\', '/');
        while (Path.empty() == false && Path.back() == '/')
        {
            Path.pop_back();
        }
        std::transform(Path.begin(), Path.end(), Path.begin(), [](const unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
        return Path;
    }
};
