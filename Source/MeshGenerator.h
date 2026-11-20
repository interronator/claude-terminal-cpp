#pragma once
#include <atomic>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// The local mesh pipeline (concept images -> TRELLIS mesh -> Blender cleanup) driven from the Blender review window.
// One stage runs at a time as a detached PowerShell script that waits for free RAM, appends everything to a log file and ends the log with a "FIN <code>" line.
class MeshGenerator
{
public:
    enum class Stage
    {
        Concepts,
        Mesh,
        Cleanup
    };

    struct Settings
    {
        std::string ScriptsDir = "D:\\claude-terminal-cpp\\Scripts\\MeshPipeline";
        std::string ImageVenvDir = "D:\\MeshGen\\VenvImage";
        std::string MeshVenvDir = "D:\\MeshGen\\Venv";
        std::string BlenderExe = "D:\\Blender\\blender.exe";
        std::string JobsRoot = "D:\\MeshGen\\Jobs";
        std::string RamCheckScript;
    };

    struct Brief
    {
        char Name[64] = "Crate";
        char Subject[256] = "wooden ammo crate";
        int Kind = 0;
        float SizeMeters = 0.6f;
        int TriBudget = 12000;
        int TextureSize = 2048;
        int ConceptCount = 3;
        int ConceptSeed = 100;
        int MeshSeed = 1;
    };

    struct Progress
    {
        bool Running = false;
        bool WaitingForRam = false;
        bool Finished = false;
        bool Cancelled = false;
        int ExitCode = 0;
        Stage Current = Stage::Concepts;
        float Fraction = -1.0f;
        double ElapsedSeconds = 0.0;
        std::string LogTail;
    };

    struct Report
    {
        bool Valid = false;
        int Tris = 0;
        float Bounds[3] = {0.0f, 0.0f, 0.0f};
        int NonManifold = 0;
        int LooseVerts = 0;
        int Degenerate = 0;
        int UvOutOfRange = 0;
        float MinZ = 0.0f;
        double Seconds = 0.0;
        std::string Folder;
        std::string FbxPath;
        std::string BlendPath;
        std::string ReportPath;
        std::vector<std::string> RenderPaths;
        std::vector<std::string> Flags;
    };

    MeshGenerator();

    Settings Config;
    Brief Draft;
    std::string SelectedConcept;
    std::set<std::string> RejectedConcepts;
    std::vector<Stage> Queue;
    char AdviceText[2048] = "";
    std::string LastMessage;

    static const char* StageName(const Stage Kind);
    static std::string SafeText(const std::string& Text);
    static long long FileKey(const std::string& Path);
    static std::string SettingsPath();
    static std::string RamTask(const Stage Kind);

    std::string AssetName() const;
    std::string JobFolder() const;
    std::string ConceptsFolder() const;
    std::string MeshPath() const;
    std::string CleanupRoot() const;
    std::string AssetFolder() const;
    std::string BuildCommand(const Stage Kind) const;
    std::string ConceptsCommand() const;
    std::string MeshCommand() const;
    std::string CleanupCommand() const;
    std::vector<std::string> ListConcepts() const;
    Report ReadReport() const;
    std::string ApplyAdvice(const std::string& Text);
    std::string DescribeBrief() const;

    void LoadSettings();
    void SaveSettings() const;

    // Returns an error text, or an empty string when the stage was launched.
    std::string Start(const Stage Kind);
    Progress Poll();
    void Cancel();
    bool Busy() const;

private:
    struct RunState
    {
        std::atomic<bool> ThreadDone{false};
        std::atomic<bool> Cancelled{false};
    };

    static std::string TailOf(const std::string& Path, const size_t MaxBytes);
    static std::string PsQuote(const std::string& Text);
    std::string RamCheckPath() const;

    std::shared_ptr<RunState> Run_;
    Stage CurrentStage_ = Stage::Concepts;
    std::string LogPath_;
    std::string PidPath_;
    std::time_t StartedAt_ = 0;
    std::time_t FinishedAt_ = 0;
    bool Active_ = false;
    size_t ExpectedConcepts_ = 0;
    Progress Last_;
};
