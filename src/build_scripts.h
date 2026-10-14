#pragma once
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// Creates starter build.bat / run.bat files for a project folder and tells whether they exist.
class BuildScripts
{
public:
    static std::filesystem::path BuildPath(const std::string& Dir)
    {
        return std::filesystem::path(Dir) / "build.bat";
    }

    static std::filesystem::path RunPath(const std::string& Dir)
    {
        return std::filesystem::path(Dir) / "run.bat";
    }

    static bool BothExist(const std::string& Dir)
    {
        std::error_code ErrorCode;
        return std::filesystem::exists(BuildPath(Dir), ErrorCode) == true && std::filesystem::exists(RunPath(Dir), ErrorCode) == true;
    }

    static std::string Guidance()
    {
        return "Build and run convention for this project: build.bat in the project root builds the project, and run.bat in the project root "
               "launches the built executable (it kills any running copy first). To build, run build.bat. To launch the program, run run.bat. "
               "Do not guess the executable path: run.bat is the source of truth for where the exe lives. If you change the build output "
               "location or the exe name, update run.bat to match. If either file is missing, create it before building.";
    }

    static std::string SetupRequest(const std::string& Created)
    {
        return "The Build and Run button created starter " + Created +
               " in this project folder. Please review them and fix them so build.bat really builds this project and run.bat launches the "
               "exact executable that build.bat produces (use its real path, kill any running copy first). Then run build.bat to confirm it works. " +
               Guidance();
    }

    // Writes whichever of the two files is missing. Returns the names that were created.
    static std::string CreateMissing(const std::string& Dir)
    {
        std::error_code ErrorCode;
        std::string Created;
        if (std::filesystem::exists(BuildPath(Dir), ErrorCode) == false)
        {
            Write(BuildPath(Dir), BuildContent(Dir));
            Created += "build.bat";
        }
        if (std::filesystem::exists(RunPath(Dir), ErrorCode) == false)
        {
            Write(RunPath(Dir), RunContent(Dir));
            Created += Created.empty() == true ? "run.bat" : " and run.bat";
        }
        return Created;
    }

private:
    static void Write(const std::filesystem::path& Path, const std::string& Content)
    {
        std::ofstream Stream(Path, std::ios::binary);
        Stream << Content;
    }

    static bool Exists(const std::string& Dir, const char* const Name)
    {
        std::error_code ErrorCode;
        return std::filesystem::exists(std::filesystem::path(Dir) / Name, ErrorCode);
    }

    static std::string ReadFile(const std::filesystem::path& Path)
    {
        std::ifstream Stream(Path);
        std::stringstream Buffer;
        Buffer << Stream.rdbuf();
        return Buffer.str();
    }

    static bool IsSpace(const char Character)
    {
        return Character == ' ' || Character == '\t' || Character == '\n' || Character == '\r';
    }

    // First argument inside "Marker(" (e.g. add_executable(Name ...)) in a file's text, or empty.
    static std::string FirstArgument(const std::string& Text, const std::string& Marker)
    {
        const size_t Start = Text.find(Marker + "(");
        if (Start == std::string::npos)
        {
            return {};
        }
        size_t Begin = Start + Marker.size() + 1;
        while (Begin < Text.size() && IsSpace(Text[Begin]) == true)
        {
            Begin++;
        }
        size_t End = Begin;
        while (End < Text.size() && IsSpace(Text[End]) == false && Text[End] != ')')
        {
            End++;
        }
        return Text.substr(Begin, End - Begin);
    }

    static std::string CargoName(const std::string& Dir)
    {
        std::istringstream Stream(ReadFile(std::filesystem::path(Dir) / "Cargo.toml"));
        std::string Line;
        while (std::getline(Stream, Line).fail() == false)
        {
            if (Line.rfind("name", 0) != 0)
            {
                continue;
            }
            const size_t Open = Line.find('"');
            const size_t Close = Line.rfind('"');
            if (Open != std::string::npos && Close > Open)
            {
                return Line.substr(Open + 1, Close - Open - 1);
            }
        }
        return {};
    }

    static std::string ExecutableName(const std::string& Dir)
    {
        if (Exists(Dir, "CMakeLists.txt") == true)
        {
            const std::string Text = ReadFile(std::filesystem::path(Dir) / "CMakeLists.txt");
            const std::string Name = FirstArgument(Text, "add_executable");
            return Name.empty() == false ? Name : FirstArgument(Text, "project");
        }
        if (Exists(Dir, "Cargo.toml") == true)
        {
            return CargoName(Dir);
        }
        return {};
    }

    static std::string BuildContent(const std::string& Dir)
    {
        std::string Body;
        if (Exists(Dir, "CMakeLists.txt") == true)
        {
            Body = "cmake -S . -B build || exit /b 1\r\ncmake --build build --config Release || exit /b 1\r\n";
        }
        else if (Exists(Dir, "Cargo.toml") == true)
        {
            Body = "cargo build --release || exit /b 1\r\n";
        }
        else if (Exists(Dir, "package.json") == true)
        {
            Body = "call npm install || exit /b 1\r\ncall npm run build --if-present || exit /b 1\r\n";
        }
        else
        {
            Body = "echo No known build system found. Edit build.bat to add your build steps.\r\nexit /b 1\r\n";
        }
        return "@echo off\r\ncd /d \"%~dp0\"\r\n" + Body;
    }

    // Kills any previous instance before launching so only one copy runs at a time.
    static std::string RunContent(const std::string& Dir)
    {
        const std::string Header = "@echo off\r\ncd /d \"%~dp0\"\r\n";
        const std::string Name = ExecutableName(Dir);
        if (Exists(Dir, "CMakeLists.txt") == true && Name.empty() == false)
        {
            return Header + "taskkill /F /IM \"" + Name + ".exe\" >nul 2>&1\r\nstart \"\" \"build\\Release\\" + Name + ".exe\"\r\n";
        }
        if (Exists(Dir, "Cargo.toml") == true && Name.empty() == false)
        {
            return Header + "taskkill /F /IM \"" + Name + ".exe\" >nul 2>&1\r\nstart \"\" \"target\\release\\" + Name + ".exe\"\r\n";
        }
        if (Exists(Dir, "package.json") == true)
        {
            return Header + "call npm start\r\n";
        }
        return Header + "echo No known run command found. Edit run.bat to launch your program.\r\nexit /b 1\r\n";
    }
};
