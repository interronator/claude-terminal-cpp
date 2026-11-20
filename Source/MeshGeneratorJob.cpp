#include "MeshGenerator.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "Git.h"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;

MeshGenerator::MeshGenerator()
{
    const char* const Home = std::getenv("USERPROFILE");
    if (Home != nullptr)
    {
        Config.RamCheckScript = (fs::path(Home) / ".claude" / "tools" / "RamCheck.ps1").string();
    }
    LoadSettings();
}

const char* MeshGenerator::StageName(const Stage Kind)
{
    if (Kind == Stage::Concepts)
    {
        return "Concepts";
    }
    if (Kind == Stage::Mesh)
    {
        return "Mesh";
    }
    return "Cleanup";
}

std::string MeshGenerator::RamTask(const Stage Kind)
{
    return Kind == Stage::Cleanup ? "Blender" : "Headless";
}

std::string MeshGenerator::SafeText(const std::string& Text)
{
    std::string Clean;
    for (const char Letter : Text)
    {
        if (Letter == '"')
        {
            Clean += '\'';
            continue;
        }
        if (Letter == '%' || Letter == '\r' || Letter == '\n' || Letter == '^' || Letter == '&' || Letter == '|' || Letter == '<' || Letter == '>')
        {
            Clean += ' ';
            continue;
        }
        Clean += Letter;
    }
    return Clean;
}

std::string MeshGenerator::PsQuote(const std::string& Text)
{
    std::string Quoted = "'";
    for (const char Letter : Text)
    {
        Quoted += Letter;
        if (Letter == '\'')
        {
            Quoted += '\'';
        }
    }
    return Quoted + "'";
}

long long MeshGenerator::FileKey(const std::string& Path)
{
    std::error_code ErrorCode;
    const fs::file_time_type Stamp = fs::last_write_time(fs::path(Path), ErrorCode);
    if (ErrorCode.value() != 0)
    {
        return 0;
    }
    return static_cast<long long>(Stamp.time_since_epoch().count() / 10000000);
}

std::string MeshGenerator::SettingsPath()
{
    const char* const Roaming = std::getenv("APPDATA");
    if (Roaming == nullptr)
    {
        return std::string();
    }
    return (fs::path(Roaming) / "ClaudeTerminal" / "MeshGenerator.json").string();
}

std::string MeshGenerator::AssetName() const
{
    std::string Clean;
    for (const char Letter : std::string(Draft.Name))
    {
        if (std::isalnum(static_cast<unsigned char>(Letter)) != 0 || Letter == '_')
        {
            Clean += Letter;
        }
    }
    return Clean.empty() == true ? "Asset" : Clean;
}

std::string MeshGenerator::JobFolder() const
{
    return (fs::path(Config.JobsRoot) / AssetName()).string();
}

std::string MeshGenerator::ConceptsFolder() const
{
    return (fs::path(JobFolder()) / "Concepts").string();
}

std::string MeshGenerator::MeshPath() const
{
    return (fs::path(JobFolder()) / "Mesh" / (AssetName() + ".glb")).string();
}

std::string MeshGenerator::CleanupRoot() const
{
    return (fs::path(JobFolder()) / "Cleanup").string();
}

std::string MeshGenerator::AssetFolder() const
{
    return (fs::path(CleanupRoot()) / ((Draft.Kind == 1 ? "SK_" : "SM_") + AssetName())).string();
}

std::string MeshGenerator::BuildCommand(const Stage Kind) const
{
    if (Kind == Stage::Concepts)
    {
        return ConceptsCommand();
    }
    if (Kind == Stage::Mesh)
    {
        return MeshCommand();
    }
    return CleanupCommand();
}

std::string MeshGenerator::ConceptsCommand() const
{
    const fs::path Scripts(Config.ScriptsDir);
    const std::string Kinds = Draft.Kind == 1 ? "character" : "prop";
    return "\"" + (fs::path(Config.ImageVenvDir) / "Scripts" / "python.exe").string() + "\" -u \"" + (Scripts / "GenerateConcepts.py").string() + "\" --kind " + Kinds +
           " --subject \"" + SafeText(Draft.Subject) + "\" --count " + std::to_string(Draft.ConceptCount) + " --seed " + std::to_string(Draft.ConceptSeed) + " --out \"" +
           ConceptsFolder() + "\"";
}

std::string MeshGenerator::MeshCommand() const
{
    const fs::path Scripts(Config.ScriptsDir);
    return "\"" + (fs::path(Config.MeshVenvDir) / "Scripts" / "python.exe").string() + "\" -u \"" + (Scripts / "GenerateMesh.py").string() + "\" \"" + SafeText(SelectedConcept) +
           "\" \"" + MeshPath() + "\" --seed " + std::to_string(Draft.MeshSeed) + " --texture 4096";
}

std::string MeshGenerator::CleanupCommand() const
{
    const fs::path Scripts(Config.ScriptsDir);
    const std::string Kinds = Draft.Kind == 1 ? "character" : "prop";
    char Size[32];
    std::snprintf(Size, sizeof Size, "%.4f", Draft.SizeMeters);
    return "\"" + Config.BlenderExe + "\" -b -P \"" + (Scripts / "CleanupMesh.py").string() + "\" -- --input \"" + MeshPath() + "\" --name " + AssetName() + " --kind " + Kinds +
           " --size " + Size + " --tris " + std::to_string(Draft.TriBudget) + " --tex " + std::to_string(Draft.TextureSize) + " --out \"" + CleanupRoot() + "\"";
}

std::vector<std::string> MeshGenerator::ListConcepts() const
{
    std::vector<std::string> Paths;
    std::error_code ErrorCode;
    for (const fs::directory_entry& Entry : fs::directory_iterator(ConceptsFolder(), ErrorCode))
    {
        const std::string FileName = Entry.path().filename().string();
        if (FileName.rfind("Concept_", 0) == 0 && Entry.path().extension() == ".png")
        {
            Paths.push_back(Entry.path().string());
        }
    }
    std::sort(Paths.begin(), Paths.end());
    return Paths;
}

MeshGenerator::Report MeshGenerator::ReadReport() const
{
    Report Result;
    Result.Folder = AssetFolder();
    Result.ReportPath = (fs::path(Result.Folder) / "Report.json").string();
    std::ifstream File(Result.ReportPath);
    if (File.is_open() == false)
    {
        return Result;
    }
    const nlohmann::json Json = nlohmann::json::parse(File, nullptr, false);
    if (Json.is_object() == false)
    {
        return Result;
    }
    Result.Valid = true;
    Result.Tris = Json.value("low_tris", 0);
    Result.Seconds = Json.value("seconds", 0.0);
    Result.FbxPath = Json.value("fbx", std::string());
    Result.BlendPath = (fs::path(Result.Folder) / ((Draft.Kind == 1 ? "SK_" : "SM_") + AssetName() + ".blend")).string();
    if (Json.contains("bounds_m") == true && Json["bounds_m"].is_array() == true && Json["bounds_m"].size() >= 3)
    {
        for (size_t Index = 0; Index < 3; Index++)
        {
            Result.Bounds[Index] = Json["bounds_m"][Index].is_number() == true ? Json["bounds_m"][Index].get<float>() : 0.0f;
        }
    }
    if (Json.contains("check") == true && Json["check"].is_object() == true)
    {
        const nlohmann::json& Check = Json["check"];
        Result.NonManifold = Check.value("non_manifold_edges", 0);
        Result.LooseVerts = Check.value("loose_verts", 0);
        Result.Degenerate = Check.value("degenerate_faces", 0);
        Result.UvOutOfRange = Check.value("uv_out_of_range_loops", 0);
        Result.MinZ = Check.value("min_z", 0.0f);
    }
    for (const char* const Label : {"front", "threequarter", "side", "back"})
    {
        const std::string Path = (fs::path(Result.Folder) / "Renders" / (std::string(Label) + ".png")).string();
        std::error_code ErrorCode;
        if (fs::exists(Path, ErrorCode) == true)
        {
            Result.RenderPaths.push_back(Path);
        }
    }
    if (Result.NonManifold > 0)
    {
        Result.Flags.push_back(std::to_string(Result.NonManifold) + " open or non-manifold edges (holes)");
    }
    if (Result.LooseVerts > 0)
    {
        Result.Flags.push_back(std::to_string(Result.LooseVerts) + " loose vertices");
    }
    if (Result.Degenerate > 0)
    {
        Result.Flags.push_back(std::to_string(Result.Degenerate) + " degenerate faces");
    }
    if (Result.UvOutOfRange > 0)
    {
        Result.Flags.push_back(std::to_string(Result.UvOutOfRange) + " UV corners outside 0-1");
    }
    if (std::abs(Result.MinZ) > 0.01f)
    {
        Result.Flags.push_back("model does not sit on the ground (lowest point " + std::to_string(Result.MinZ) + " m)");
    }
    if (Draft.TriBudget > 0 && Result.Tris > Draft.TriBudget * 11 / 10)
    {
        Result.Flags.push_back(std::to_string(Result.Tris) + " triangles is over the budget of " + std::to_string(Draft.TriBudget));
    }
    const float Longest = std::max(Result.Bounds[0], std::max(Result.Bounds[1], Result.Bounds[2]));
    if (Draft.SizeMeters > 0.0f && Longest > 0.0f && std::abs(Longest - Draft.SizeMeters) > Draft.SizeMeters * 0.05f && Draft.Kind == 0)
    {
        Result.Flags.push_back("longest side is " + std::to_string(Longest) + " m, wanted " + std::to_string(Draft.SizeMeters) + " m");
    }
    return Result;
}

std::string MeshGenerator::DescribeBrief() const
{
    return "name " + AssetName() + ", " + (Draft.Kind == 1 ? "character" : "prop") + ", subject '" + SafeText(Draft.Subject) + "', size " + std::to_string(Draft.SizeMeters) +
           " m, triangle budget " + std::to_string(Draft.TriBudget) + ", texture " + std::to_string(Draft.TextureSize) + ", concept seed " + std::to_string(Draft.ConceptSeed) +
           ", mesh seed " + std::to_string(Draft.MeshSeed);
}

std::string MeshGenerator::ApplyAdvice(const std::string& Text)
{
    std::istringstream Lines(Text);
    std::string Line;
    std::string Summary;
    while (std::getline(Lines, Line).fail() == false)
    {
        const size_t Colon = Line.find(':');
        if (Colon == std::string::npos)
        {
            continue;
        }
        std::string Key = Line.substr(0, Colon);
        std::string Value = Line.substr(Colon + 1);
        const size_t First = Value.find_first_not_of(" \t\r");
        Value = First == std::string::npos ? std::string() : Value.substr(First);
        while (Value.empty() == false && (Value.back() == '\r' || Value.back() == ' '))
        {
            Value.pop_back();
        }
        std::transform(Key.begin(), Key.end(), Key.begin(), [](const unsigned char Letter) { return static_cast<char>(std::tolower(Letter)); });
        while (Key.empty() == false && (Key.front() == ' ' || Key.front() == '-' || Key.front() == '*'))
        {
            Key.erase(Key.begin());
        }
        while (Key.empty() == false && Key.back() == ' ')
        {
            Key.pop_back();
        }
        if (Value.empty() == true)
        {
            continue;
        }
        if (Key == "prompt" || Key == "subject")
        {
            std::snprintf(Draft.Subject, sizeof Draft.Subject, "%s", Value.c_str());
            Summary += "subject, ";
        }
        else if (Key == "seed" || Key == "mesh seed")
        {
            Draft.MeshSeed = std::atoi(Value.c_str());
            Summary += "mesh seed, ";
        }
        else if (Key == "concept seed")
        {
            Draft.ConceptSeed = std::atoi(Value.c_str());
            Summary += "concept seed, ";
        }
        else if (Key == "tris" || Key == "triangles")
        {
            Draft.TriBudget = std::max(100, std::atoi(Value.c_str()));
            Summary += "triangle budget, ";
        }
        else if (Key == "size")
        {
            Draft.SizeMeters = std::max(0.01f, static_cast<float>(std::atof(Value.c_str())));
            Summary += "size, ";
        }
    }
    if (Summary.empty() == false)
    {
        Summary.resize(Summary.size() - 2);
    }
    return Summary;
}

void MeshGenerator::LoadSettings()
{
    std::ifstream File(SettingsPath());
    if (File.is_open() == false)
    {
        return;
    }
    const nlohmann::json Json = nlohmann::json::parse(File, nullptr, false);
    if (Json.is_object() == false)
    {
        return;
    }
    Config.ScriptsDir = Json.value("scriptsDir", Config.ScriptsDir);
    Config.ImageVenvDir = Json.value("imageVenvDir", Config.ImageVenvDir);
    Config.MeshVenvDir = Json.value("meshVenvDir", Config.MeshVenvDir);
    Config.BlenderExe = Json.value("blenderExe", Config.BlenderExe);
    Config.JobsRoot = Json.value("jobsRoot", Config.JobsRoot);
    Config.RamCheckScript = Json.value("ramCheckScript", Config.RamCheckScript);
    const std::string Name = Json.value("name", std::string(Draft.Name));
    const std::string Subject = Json.value("subject", std::string(Draft.Subject));
    std::snprintf(Draft.Name, sizeof Draft.Name, "%s", Name.c_str());
    std::snprintf(Draft.Subject, sizeof Draft.Subject, "%s", Subject.c_str());
    Draft.Kind = Json.value("kind", Draft.Kind);
    Draft.SizeMeters = Json.value("size", Draft.SizeMeters);
    Draft.TriBudget = Json.value("tris", Draft.TriBudget);
    Draft.TextureSize = Json.value("tex", Draft.TextureSize);
    Draft.ConceptCount = Json.value("concepts", Draft.ConceptCount);
    Draft.ConceptSeed = Json.value("conceptSeed", Draft.ConceptSeed);
    Draft.MeshSeed = Json.value("meshSeed", Draft.MeshSeed);
}

void MeshGenerator::SaveSettings() const
{
    const std::string Path = SettingsPath();
    if (Path.empty() == true)
    {
        return;
    }
    nlohmann::json Json;
    Json["scriptsDir"] = Config.ScriptsDir;
    Json["imageVenvDir"] = Config.ImageVenvDir;
    Json["meshVenvDir"] = Config.MeshVenvDir;
    Json["blenderExe"] = Config.BlenderExe;
    Json["jobsRoot"] = Config.JobsRoot;
    Json["ramCheckScript"] = Config.RamCheckScript;
    Json["name"] = std::string(Draft.Name);
    Json["subject"] = std::string(Draft.Subject);
    Json["kind"] = Draft.Kind;
    Json["size"] = Draft.SizeMeters;
    Json["tris"] = Draft.TriBudget;
    Json["tex"] = Draft.TextureSize;
    Json["concepts"] = Draft.ConceptCount;
    Json["conceptSeed"] = Draft.ConceptSeed;
    Json["meshSeed"] = Draft.MeshSeed;
    std::error_code ErrorCode;
    fs::create_directories(fs::path(Path).parent_path(), ErrorCode);
    std::ofstream File(Path);
    if (File.is_open() == true)
    {
        File << Json.dump(2);
    }
}

std::string MeshGenerator::RamCheckPath() const
{
    std::error_code ErrorCode;
    if (Config.RamCheckScript.empty() == true || fs::exists(Config.RamCheckScript, ErrorCode) == false)
    {
        return std::string();
    }
    return Config.RamCheckScript;
}

bool MeshGenerator::Busy() const
{
    return Active_ == true;
}

std::string MeshGenerator::TailOf(const std::string& Path, const size_t MaxBytes)
{
    std::ifstream File(Path, std::ios::binary | std::ios::ate);
    if (File.is_open() == false)
    {
        return std::string();
    }
    const std::streamoff Size = File.tellg();
    const std::streamoff Start = Size > static_cast<std::streamoff>(MaxBytes) ? Size - static_cast<std::streamoff>(MaxBytes) : 0;
    File.seekg(Start);
    std::string Text(static_cast<size_t>(Size - Start), '\0');
    File.read(Text.data(), static_cast<std::streamsize>(Text.size()));
    Text.resize(static_cast<size_t>(File.gcount()));
    return Text;
}

std::string MeshGenerator::Start(const Stage Kind)
{
    if (Active_ == true)
    {
        return "A step is already running.";
    }
    if (Kind == Stage::Mesh && SelectedConcept.empty() == true)
    {
        return "Pick a concept image first.";
    }
    std::error_code ErrorCode;
    if (Kind == Stage::Cleanup && fs::exists(MeshPath(), ErrorCode) == false)
    {
        return "Run the Mesh step first: " + MeshPath() + " does not exist.";
    }
    const fs::path Job(JobFolder());
    fs::create_directories(Job / "Logs", ErrorCode);
    fs::create_directories(Job / "Mesh", ErrorCode);
    if (ErrorCode.value() != 0)
    {
        return "Could not create " + Job.string() + ": " + ErrorCode.message();
    }
    const std::string StageText = StageName(Kind);
    LogPath_ = (Job / "Logs" / (StageText + ".log")).string();
    PidPath_ = (Job / "Logs" / (StageText + ".pid")).string();
    const std::string BatPath = (Job / "Logs" / ("Run" + StageText + ".bat")).string();
    const std::string ScriptPath = (Job / "Logs" / ("Run" + StageText + ".ps1")).string();
    {
        std::ofstream Log(LogPath_, std::ios::trunc);
    }
    fs::remove(PidPath_, ErrorCode);
    {
        std::ofstream Bat(BatPath, std::ios::trunc | std::ios::binary);
        if (Bat.is_open() == false)
        {
            return "Could not write " + BatPath;
        }
        Bat << "@echo off\r\nchcp 65001 >nul\r\n" << BuildCommand(Kind) << " >> \"" << LogPath_ << "\" 2>&1\r\n";
    }
    const std::string RamScript = RamCheckPath();
    const std::string Task = RamTask(Kind);
    {
        std::ofstream Script(ScriptPath, std::ios::trunc | std::ios::binary);
        if (Script.is_open() == false)
        {
            return "Could not write " + ScriptPath;
        }
        Script << "\xEF\xBB\xBF"
               << "$ErrorActionPreference = 'Continue'\r\n"
               << "Set-Content -Path " << PsQuote(PidPath_) << " -Value $PID\r\n"
               << "$Ram = " << PsQuote(RamScript) << "\r\n"
               << "if ($Ram -ne '') {\r\n"
               << "  $Waited = $false\r\n"
               << "  while ($true) {\r\n"
               << "    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Ram -Need " << Task << " *> $null\r\n"
               << "    if ($LASTEXITCODE -eq 0) { break }\r\n"
               << "    if (-not $Waited) { Add-Content -Path " << PsQuote(LogPath_) << " -Value '[WAITING_RAM]'; $Waited = $true }\r\n"
               << "    Start-Sleep -Seconds 30\r\n"
               << "  }\r\n"
               << "}\r\n"
               << "Add-Content -Path " << PsQuote(LogPath_) << " -Value '[RUNNING]'\r\n"
               << "& cmd.exe /c " << PsQuote(BatPath) << "\r\n"
               << "$Code = $LASTEXITCODE\r\n"
               << "if ($Ram -ne '') { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Ram -Release *> $null }\r\n"
               << "Add-Content -Path " << PsQuote(LogPath_) << " -Value \"FIN $Code\"\r\n";
    }

    CurrentStage_ = Kind;
    StartedAt_ = std::time(nullptr);
    FinishedAt_ = 0;
    ExpectedConcepts_ = Kind == Stage::Concepts ? static_cast<size_t>(std::max(1, Draft.ConceptCount)) : 0;
    Last_ = Progress{};
    Run_ = std::make_shared<RunState>();
    Active_ = true;
    if (Kind == Stage::Concepts)
    {
        SelectedConcept.clear();
        RejectedConcepts.clear();
    }
    const std::shared_ptr<RunState> State = Run_;
    std::thread Worker(
        [State, ScriptPath]()
        {
            Git::RunProcess({"powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", ScriptPath}, "");
            State->ThreadDone = true;
        });
    Worker.detach();
    SaveSettings();
    return std::string();
}

MeshGenerator::Progress MeshGenerator::Poll()
{
    if (Active_ == false)
    {
        return Last_;
    }
    const bool ThreadDone = Run_->ThreadDone.load();
    Progress Now;
    Now.Current = CurrentStage_;
    const std::string Raw = TailOf(LogPath_, 16384);
    std::string Clean = Raw;
    std::replace(Clean.begin(), Clean.end(), '\r', '\n');
    std::vector<std::string> Lines;
    std::istringstream Stream(Clean);
    std::string Line;
    while (std::getline(Stream, Line).fail() == false)
    {
        if (Line.empty() == false)
        {
            Lines.push_back(Line);
        }
    }
    bool Running = false;
    bool Fin = false;
    int ExitCode = 0;
    float Percent = -1.0f;
    float TqdmPercent = -1.0f;
    std::vector<std::string> Shown;
    for (const std::string& Entry : Lines)
    {
        if (Entry == "[WAITING_RAM]")
        {
            Now.WaitingForRam = true;
            continue;
        }
        if (Entry == "[RUNNING]")
        {
            Running = true;
            Now.WaitingForRam = false;
            continue;
        }
        if (Entry.rfind("FIN ", 0) == 0)
        {
            Fin = true;
            ExitCode = std::atoi(Entry.c_str() + 4);
            continue;
        }
        if (Entry.rfind("[PROGRESS]", 0) == 0)
        {
            const size_t Space = Entry.find_last_of(' ');
            if (Space != std::string::npos)
            {
                Percent = static_cast<float>(std::atof(Entry.c_str() + Space + 1)) / 100.0f;
            }
            continue;
        }
        const size_t Sign = Entry.find('%');
        if (Sign != std::string::npos && Sign > 0 && std::isdigit(static_cast<unsigned char>(Entry[Sign - 1])) != 0)
        {
            size_t Begin = Sign;
            while (Begin > 0 && std::isdigit(static_cast<unsigned char>(Entry[Begin - 1])) != 0)
            {
                Begin--;
            }
            TqdmPercent = static_cast<float>(std::atoi(Entry.c_str() + Begin)) / 100.0f;
        }
        Shown.push_back(Entry);
    }
    if (Percent < 0.0f)
    {
        Percent = TqdmPercent;
    }
    if (CurrentStage_ == Stage::Concepts)
    {
        Percent = std::min(1.0f, static_cast<float>(ListConcepts().size()) / static_cast<float>(std::max<size_t>(1, ExpectedConcepts_)));
    }
    Now.Fraction = Percent < 0.0f ? -1.0f : std::min(1.0f, Percent);
    const size_t FirstShown = Shown.size() > 12 ? Shown.size() - 12 : 0;
    for (size_t Index = FirstShown; Index < Shown.size(); Index++)
    {
        Now.LogTail += Shown[Index].substr(0, 220) + "\n";
    }
    const bool Cancelled = Run_->Cancelled.load();
    const bool Ended = Fin == true || Cancelled == true || ThreadDone == true;
    if (Ended == true && FinishedAt_ == 0)
    {
        FinishedAt_ = std::time(nullptr);
    }
    Now.ElapsedSeconds = static_cast<double>((FinishedAt_ != 0 ? FinishedAt_ : std::time(nullptr)) - StartedAt_);
    Now.Running = Ended == false;
    Now.Finished = Ended;
    Now.Cancelled = Cancelled;
    Now.ExitCode = Cancelled == true ? -1 : Fin == true ? ExitCode : 1;
    if (Ended == true)
    {
        Active_ = false;
        Now.Fraction = Now.ExitCode == 0 ? 1.0f : Now.Fraction;
    }
    Last_ = Now;
    return Now;
}

void MeshGenerator::Cancel()
{
    if (Active_ == false || Run_ == nullptr)
    {
        return;
    }
    Run_->Cancelled = true;
    const std::string PidPath = PidPath_;
    const std::string RamScript = RamCheckPath();
    const std::shared_ptr<RunState> State = Run_;
    std::thread Killer(
        [PidPath, RamScript, State]()
        {
            std::string Digits;
            for (int Attempt = 0; Attempt < 40 && Digits.empty() == true && State->ThreadDone.load() == false; Attempt++)
            {
                for (const char Letter : TailOf(PidPath, 32))
                {
                    if (std::isdigit(static_cast<unsigned char>(Letter)) != 0)
                    {
                        Digits += Letter;
                    }
                }
                if (Digits.empty() == true)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            }
            if (Digits.empty() == false)
            {
                Git::RunProcess({"taskkill.exe", "/F", "/T", "/PID", Digits}, "");
            }
            if (RamScript.empty() == false)
            {
                Git::RunProcess({"powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", RamScript, "-Release"}, "");
            }
        });
    Killer.detach();
}
