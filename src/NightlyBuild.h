#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Git.h"
#include "Util.h"

// Scheduled Windows builds of a project. Each one is a folder under <project>/NightlyBuilds holding its settings, the
// PowerShell script Task Scheduler runs, the last log, and a status file the script keeps up to date. Every call that
// touches Task Scheduler runs on a background thread; the window reads the latest snapshot.
class NightlyBuild
{
public:
    enum class Kind
    {
        PackageGame,
        CompileEditor,
        BuildScript
    };

    static constexpr const char* FOLDER_NAME = "NightlyBuilds";
    static constexpr const char* TASK_PATH = "\\ClaudeTerminal\\";
    static constexpr const char* CONFIGS[] = {"Development", "Shipping", "DebugGame"};
    static constexpr const char* DAY_LABELS[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static constexpr const char* DAY_NAMES[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    static constexpr int DAY_COUNT = 7;

    struct Job
    {
        std::string Name;
        Kind What = Kind::PackageGame;
        std::string Config = "Development";
        int Hour = 2;
        int Minute = 0;
        std::array<bool, DAY_COUNT> Days{true, true, true, true, true, true, true};
        std::string OutputDir;
        std::string EngineDir;  // empty: found from the .uproject's engine version

        // Task Scheduler's state name (Ready, Running, Disabled, Queued), which is not translated; empty when the task is missing.
        std::string TaskState;
        std::string NextRun;
        std::string LastResult;
        std::string LastTime;
        bool HasLog = false;
        bool HasOutput = false;

        bool IsRunning() const
        {
            return TaskState == "Running";
        }

        bool IsPaused() const
        {
            return TaskState == "Disabled";
        }
    };

    static bool Supports(const std::string& ProjectDir)
    {
#ifdef _WIN32
        return IsUnreal(ProjectDir) == true || HasBuildScript(ProjectDir) == true;
#else
        (void)ProjectDir;
        return false;
#endif
    }

    // Supports, re-checked at most every few seconds; for UI drawn every frame.
    bool SupportsCached(const std::string& ProjectDir, const double Now)
    {
        constexpr double RECHECK_SECONDS = 10.0;
        CachedSupport& Entry = SupportsCache_[ProjectDir];
        if (Now >= Entry.CheckAt)
        {
            Entry.Value = Supports(ProjectDir);
            Entry.CheckAt = Now + RECHECK_SECONDS;
        }
        return Entry.Value;
    }

    static bool IsUnreal(const std::string& ProjectDir)
    {
        return FindUproject(ProjectDir).empty() == false;
    }

    static bool HasBuildScript(const std::string& ProjectDir)
    {
        std::error_code ErrorCode;
        return std::filesystem::exists(std::filesystem::path(ProjectDir) / "build.bat", ErrorCode);
    }

    static std::string FindUproject(const std::string& ProjectDir)
    {
        std::error_code ErrorCode;
        for (const std::filesystem::directory_entry& Item : std::filesystem::directory_iterator(ProjectDir, ErrorCode))
        {
            if (Item.path().extension() == ".uproject")
            {
                return Item.path().string();
            }
        }
        return {};
    }

    static std::string KindLabel(const Kind What)
    {
        switch (What)
        {
            case Kind::PackageGame: return "Package game";
            case Kind::CompileEditor: return "Compile editor";
            case Kind::BuildScript: return "Run build.bat";
        }
        return {};
    }

    // Unreal has no Shipping build of the editor.
    static bool ConfigAllowed(const Kind What, const std::string& Config)
    {
        return What != Kind::CompileEditor || Config != "Shipping";
    }

    static std::string SafeName(const std::string& Name)
    {
        std::string Safe;
        for (const char Character : Name)
        {
            const bool Keep = std::isalnum(static_cast<unsigned char>(Character)) != 0 || Character == '-' || Character == '_';
            Safe += Keep == true ? Character : '-';
        }
        return Safe;
    }

    static std::filesystem::path JobsRoot(const std::string& ProjectDir)
    {
        return std::filesystem::path(ProjectDir) / FOLDER_NAME;
    }

    static std::filesystem::path JobDir(const std::string& ProjectDir, const std::string& Name)
    {
        return JobsRoot(ProjectDir) / SafeName(Name);
    }

    static std::filesystem::path LogPath(const std::string& ProjectDir, const std::string& Name)
    {
        return JobDir(ProjectDir, Name) / LOG_FILE;
    }

    static std::string DefaultOutputDir(const std::string& ProjectDir, const std::string& Name)
    {
        return (JobDir(ProjectDir, Name.empty() == true ? "Nightly" : Name) / "Output").string();
    }

    // The task's name inside TASK_PATH. The hash of the full project path keeps two projects with the same folder name apart.
    static std::string TaskLeaf(const std::string& ProjectDir, const std::string& Name)
    {
        std::string Normal = std::filesystem::path(ProjectDir).lexically_normal().string();
        while (Normal.empty() == false && (Normal.back() == '\\' || Normal.back() == '/'))
        {
            Normal.pop_back();
        }
        uint32_t Hash = 2166136261u;
        for (const char Character : Normal)
        {
            const char Folded = Character == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
            Hash = (Hash ^ static_cast<unsigned char>(Folded)) * 16777619u;
        }
        char HashText[16];
        std::snprintf(HashText, sizeof HashText, "%08x", Hash);
        std::string Folder = SafeName(std::filesystem::path(Normal).filename().string());
        if (Folder.empty() == true)
        {
            Folder = "project";
        }
        return Folder + "-" + HashText + "-" + SafeName(Name);
    }

    static std::string TimeText(const Job& Item)
    {
        char Time[8];
        std::snprintf(Time, sizeof Time, "%02d:%02d", Item.Hour, Item.Minute);
        return Time;
    }

    static bool EveryDay(const Job& Item)
    {
        return std::count(Item.Days.begin(), Item.Days.end(), true) == DAY_COUNT;
    }

    static std::string DayList(const Job& Item, const char* const Names[], const std::string& Separator)
    {
        std::string Days;
        for (int Index = 0; Index < DAY_COUNT; Index++)
        {
            if (Item.Days[Index] == false)
            {
                continue;
            }
            Days += (Days.empty() == true ? "" : Separator) + std::string(Names[Index]);
        }
        return Days;
    }

    static std::string ScheduleText(const Job& Item)
    {
        return TimeText(Item) + (EveryDay(Item) == true ? " every day" : " " + DayList(Item, DAY_LABELS, " "));
    }

    // Checks the settings now so the window can show the problem; the files and the task are made in the background.
    bool Create(const std::string& ProjectDir, const Job& Item, std::string& Err)
    {
        if (SafeName(Item.Name).empty() == true)
        {
            Err = "Give the build a name.";
            return false;
        }
        std::error_code ErrorCode;
        if (std::filesystem::exists(JobDir(ProjectDir, Item.Name) / SETTINGS_FILE, ErrorCode) == true)
        {
            Err = "A nightly build with that name already exists.";
            return false;
        }
        if (std::find(Item.Days.begin(), Item.Days.end(), true) == Item.Days.end())
        {
            Err = "Pick at least one day.";
            return false;
        }
        if (Item.What != Kind::BuildScript && IsUnreal(ProjectDir) == false)
        {
            Err = "This project has no .uproject file.";
            return false;
        }
        if (Item.What == Kind::BuildScript && HasBuildScript(ProjectDir) == false)
        {
            Err = "This project has no build.bat.";
            return false;
        }
        if (ConfigAllowed(Item.What, Item.Config) == false)
        {
            Err = "Unreal can't build the editor in Shipping. Pick Development or DebugGame.";
            return false;
        }
        StartAction(ProjectDir, [ProjectDir, Item](std::string& Message)
                    {
                        return CreateNow(ProjectDir, Item, Message);
                    });
        return true;
    }

    void RunNow(const std::string& ProjectDir, const std::string& Name)
    {
        StartAction(ProjectDir, [ProjectDir, Name](std::string& Message)
                    {
                        return TaskCommand("Start-ScheduledTask", ProjectDir, Name, "Started " + Name + ".", Message);
                    });
    }

    void Cancel(const std::string& ProjectDir, const std::string& Name)
    {
        StartAction(ProjectDir, [ProjectDir, Name](std::string& Message)
                    {
                        return CancelNow(ProjectDir, Name, Message);
                    });
    }

    void SetPaused(const std::string& ProjectDir, const std::string& Name, const bool Paused)
    {
        StartAction(ProjectDir, [ProjectDir, Name, Paused](std::string& Message)
                    {
                        return TaskCommand(Paused == true ? "Disable-ScheduledTask" : "Enable-ScheduledTask", ProjectDir, Name,
                                           (Paused == true ? "Paused " : "Resumed ") + Name + ".", Message);
                    });
    }

    void Delete(const std::string& ProjectDir, const std::string& Name)
    {
        StartAction(ProjectDir, [ProjectDir, Name](std::string& Message)
                    {
                        return DeleteNow(ProjectDir, Name, Message);
                    });
    }

    // Re-reads the builds in the background unless a read is already going.
    void Refresh(const std::string& ProjectDir)
    {
        const std::shared_ptr<Shared> State = State_;
        uint64_t Read = 0;
        {
            std::lock_guard<std::mutex> Lock(State->Mutex);
            if (State->Refreshing == true)
            {
                return;
            }
            State->Refreshing = true;
            Read = ++State->ReadsStarted;
        }
        std::thread([State, ProjectDir, Read]
                    {
                        std::vector<Job> Jobs = LoadJobs(ProjectDir);
                        std::lock_guard<std::mutex> Lock(State->Mutex);
                        Store(*State, Read, ProjectDir, std::move(Jobs));
                        State->Refreshing = false;
                    }).detach();
    }

    std::vector<Job> Jobs(const std::string& ProjectDir) const
    {
        std::lock_guard<std::mutex> Lock(State_->Mutex);
        return State_->JobsDir == ProjectDir ? State_->Jobs : std::vector<Job>();
    }

    bool Loaded(const std::string& ProjectDir) const
    {
        std::lock_guard<std::mutex> Lock(State_->Mutex);
        return State_->Loaded == true && State_->JobsDir == ProjectDir;
    }

    bool Busy() const
    {
        std::lock_guard<std::mutex> Lock(State_->Mutex);
        return State_->Actions > 0;
    }

    // The newest action's message, once; false when there is none.
    bool TakeMessage(std::string& Message, bool& IsError)
    {
        std::lock_guard<std::mutex> Lock(State_->Mutex);
        if (State_->Message.empty() == true)
        {
            return false;
        }
        Message = State_->Message;
        IsError = State_->MessageIsError;
        State_->Message.clear();
        return true;
    }

private:
    static constexpr const char* SETTINGS_FILE = "job.ini";
    static constexpr const char* SCRIPT_FILE = "build.ps1";
    static constexpr const char* STATUS_FILE = "status.txt";
    static constexpr const char* PID_FILE = "pid.txt";
    static constexpr const char* LOG_FILE = "last.log";
    static constexpr const char* RUNNING = "Running";

    struct Shared
    {
        std::mutex Mutex;
        std::vector<Job> Jobs;
        std::string JobsDir;
        bool Loaded = false;
        bool Refreshing = false;
        int Actions = 0;
        uint64_t ReadsStarted = 0;
        uint64_t ReadStored = 0;
        std::string Message;
        bool MessageIsError = false;
    };
    std::shared_ptr<Shared> State_ = std::make_shared<Shared>();

    struct CachedSupport
    {
        bool Value = false;
        double CheckAt = 0;
    };
    std::map<std::string, CachedSupport> SupportsCache_;

    struct TaskInfo
    {
        std::string State;
        std::string NextRun;
    };

    // Keeps only the newest read: a refresh that started before an action finished must not overwrite the action's result.
    static void Store(Shared& State, const uint64_t Read, const std::string& ProjectDir, std::vector<Job>&& Jobs)
    {
        if (Read < State.ReadStored)
        {
            return;
        }
        State.ReadStored = Read;
        State.Jobs = std::move(Jobs);
        State.JobsDir = ProjectDir;
        State.Loaded = true;
    }

    // Runs one Task Scheduler action off the UI thread, then reloads the list so the window shows the result.
    void StartAction(const std::string& ProjectDir, std::function<bool(std::string&)> Action)
    {
        const std::shared_ptr<Shared> State = State_;
        {
            std::lock_guard<std::mutex> Lock(State->Mutex);
            State->Actions++;
        }
        std::thread([State, ProjectDir, Action]
                    {
                        std::string Message;
                        const bool Ok = Action(Message);
                        uint64_t Read = 0;
                        {
                            std::lock_guard<std::mutex> Lock(State->Mutex);
                            Read = ++State->ReadsStarted;
                        }
                        std::vector<Job> Jobs = LoadJobs(ProjectDir);
                        std::lock_guard<std::mutex> Lock(State->Mutex);
                        Store(*State, Read, ProjectDir, std::move(Jobs));
                        State->Actions--;
                        State->Message = Message;
                        State->MessageIsError = Ok == false;
                    }).detach();
    }

    // Runs a Windows PowerShell script. The ScheduledTasks cmdlets report task states by their English names on every
    // Windows language, and have no length limit on the task's command, unlike schtasks.exe. Output is the script's
    // output, or the error message when it fails. Every error stops the script except where a command opts out with
    // -ErrorAction SilentlyContinue. Scripts are kept free of double quotes so command-line quoting cannot change them.
    static bool PowerShell(const std::string& Body, std::string& Output)
    {
        const std::string Script =
            "try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }\n"
            "$ProgressPreference = 'SilentlyContinue'\n"
            "$ErrorActionPreference = 'Stop'\n"
            "try {\n" + Body + "\n} catch {\n"
            "  [Console]::Out.WriteLine($_.Exception.Message)\n"
            "  exit 1\n"
            "}\n"
            "exit 0\n";
        const ProcResult Result = Git::RunProcess({"powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", Script}, "");
        Output = Util::Trim(Result.Output);
        return Result.Code == 0;
    }

    static std::string TaskArgs(const std::string& ProjectDir, const std::string& Name)
    {
        return "-TaskPath " + PsQuote(TASK_PATH) + " -TaskName " + PsQuote(TaskLeaf(ProjectDir, Name));
    }

    static bool TaskCommand(const std::string& Cmdlet, const std::string& ProjectDir, const std::string& Name, const std::string& Success, std::string& Message)
    {
        std::string Output;
        if (PowerShell(Cmdlet + " " + TaskArgs(ProjectDir, Name) + " | Out-Null", Output) == true)
        {
            Message = Success;
            return true;
        }
        Message = "Task Scheduler: " + Output;
        return false;
    }

    static std::string Now()
    {
        const std::time_t Current = std::time(nullptr);
        std::tm Local{};
#ifdef _WIN32
        localtime_s(&Local, &Current);
#else
        localtime_r(&Current, &Local);
#endif
        char Stamp[32];
        std::strftime(Stamp, sizeof Stamp, "%Y-%m-%d %H:%M", &Local);
        return Stamp;
    }

    static bool WriteFile(const std::filesystem::path& Path, const std::string& Content)
    {
        std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
        Stream << Content;
        return Stream.good();
    }

    // For text inside a PowerShell single-quoted string.
    static std::string PsQuote(const std::string& Text)
    {
        std::string Quoted = "'";
        for (const char Character : Text)
        {
            Quoted += Character == '\'' ? std::string("''") : std::string(1, Character);
        }
        return Quoted + "'";
    }

    static std::string SaveSettings(const Job& Item)
    {
        std::string Days;
        for (int Index = 0; Index < DAY_COUNT; Index++)
        {
            Days += Item.Days[Index] == true ? '1' : '0';
        }
        return "Name=" + Item.Name + "\nKind=" + std::to_string(static_cast<int>(Item.What)) + "\nConfig=" + Item.Config +
               "\nHour=" + std::to_string(Item.Hour) + "\nMinute=" + std::to_string(Item.Minute) + "\nDays=" + Days +
               "\nOutputDir=" + Item.OutputDir + "\nEngineDir=" + Item.EngineDir + "\n";
    }

    static Job LoadSettings(const std::filesystem::path& Path)
    {
        Job Item;
        std::istringstream Stream(Util::ReadFile(Path));
        std::string Line;
        while (std::getline(Stream, Line).fail() == false)
        {
            if (Line.empty() == false && Line.back() == '\r')
            {
                Line.pop_back();
            }
            const size_t Equals = Line.find('=');
            if (Equals == std::string::npos)
            {
                continue;
            }
            const std::string Key = Line.substr(0, Equals);
            const std::string Value = Line.substr(Equals + 1);
            if (Key == "Name")
            {
                Item.Name = Value;
            }
            else if (Key == "Kind")
            {
                Item.What = static_cast<Kind>(std::atoi(Value.c_str()));
            }
            else if (Key == "Config")
            {
                Item.Config = Value;
            }
            else if (Key == "Hour")
            {
                Item.Hour = std::atoi(Value.c_str());
            }
            else if (Key == "Minute")
            {
                Item.Minute = std::atoi(Value.c_str());
            }
            else if (Key == "Days")
            {
                for (int Index = 0; Index < DAY_COUNT && Index < static_cast<int>(Value.size()); Index++)
                {
                    Item.Days[Index] = Value[Index] == '1';
                }
            }
            else if (Key == "OutputDir")
            {
                Item.OutputDir = Value;
            }
            else if (Key == "EngineDir")
            {
                Item.EngineDir = Value;
            }
        }
        return Item;
    }

    // Engine lookup happens when the build runs, so an engine update or move between nights is picked up.
    // pid.txt holds the process number and its start time, so a cancel can tell the build apart from an unrelated program
    // that got the same number after a crash or reboot. The final status is written before pid.txt is removed, so a
    // "Running" status without a running task always means the run died.
    static std::string ScriptContent(const std::string& ProjectDir, const Job& Item)
    {
        std::string Script =
            "$JobDir = $PSScriptRoot\r\n"
            "$Log = Join-Path $JobDir '" + std::string(LOG_FILE) + "'\r\n"
            "$StatusFile = Join-Path $JobDir '" + std::string(STATUS_FILE) + "'\r\n"
            "$PidFile = Join-Path $JobDir '" + std::string(PID_FILE) + "'\r\n"
            "function Set-Status($Text) { Set-Content -Path $StatusFile -Value \"$Text|$(Get-Date -Format 'yyyy-MM-dd HH:mm')\" -Encoding UTF8 }\r\n"
            "Set-Content -Path $PidFile -Value ('{0}|{1}' -f $PID, (Get-Process -Id $PID).StartTime.ToFileTimeUtc())\r\n"
            "Set-Status '" + std::string(RUNNING) + "'\r\n"
            "$ProjectDir = " + PsQuote(ProjectDir) + "\r\n"
            "Set-Location $ProjectDir\r\n"
            "\"Nightly build '\" + " + PsQuote(Item.Name) + " + \"' started $(Get-Date)\" | Out-File -FilePath $Log -Encoding utf8\r\n"
            "$Code = 1\r\n"
            "try {\r\n";
        if (Item.What == Kind::BuildScript)
        {
            Script +=
                "  & (Join-Path $ProjectDir 'build.bat') 2>&1 | ForEach-Object { \"$_\" } | Out-File -FilePath $Log -Append -Encoding utf8\r\n"
                "  $Code = $LASTEXITCODE\r\n";
        }
        else
        {
            Script +=
                "  $Uproject = " + PsQuote(FindUproject(ProjectDir)) + "\r\n"
                "  $EngineDir = " + PsQuote(Item.EngineDir) + "\r\n"
                "  if ($EngineDir -eq '') {\r\n"
                "    $Assoc = (Get-Content $Uproject -Raw | ConvertFrom-Json).EngineAssociation\r\n"
                "    if ($Assoc -match '^\\{') {\r\n"
                "      $EngineDir = (Get-ItemProperty 'HKCU:\\Software\\Epic Games\\Unreal Engine\\Builds' -ErrorAction SilentlyContinue).$Assoc\r\n"
                "    } elseif ($Assoc) {\r\n"
                "      $EngineDir = (Get-ItemProperty \"HKLM:\\SOFTWARE\\EpicGames\\Unreal Engine\\$Assoc\" -ErrorAction SilentlyContinue).InstalledDirectory\r\n"
                "    }\r\n"
                "  }\r\n"
                "  if (-not $EngineDir) {\r\n"
                "    $Search = Split-Path $ProjectDir -Parent\r\n"
                "    while ($Search -and -not (Test-Path (Join-Path $Search 'Engine\\Build\\BatchFiles\\RunUAT.bat'))) { $Search = Split-Path $Search -Parent }\r\n"
                "    $EngineDir = $Search\r\n"
                "  }\r\n"
                "  if (-not $EngineDir) { throw 'Could not find the Unreal Engine for this project. Set the engine folder in the nightly build settings.' }\r\n"
                "  \"Engine: $EngineDir\" | Out-File -FilePath $Log -Append -Encoding utf8\r\n";
            if (Item.What == Kind::PackageGame)
            {
                Script +=
                    "  $Tool = Join-Path $EngineDir 'Engine\\Build\\BatchFiles\\RunUAT.bat'\r\n"
                    "  $OutputDir = " + PsQuote(Item.OutputDir) + "\r\n"
                    "  $ToolArgs = @('BuildCookRun', \"-project=$Uproject\", '-noP4', '-platform=Win64', '-clientconfig=" + Item.Config +
                    "', '-build', '-cook', '-stage', '-pak', '-archive', \"-archivedirectory=$OutputDir\", '-unattended', '-utf8output')\r\n";
            }
            else
            {
                Script +=
                    "  $Tool = Join-Path $EngineDir 'Engine\\Build\\BatchFiles\\Build.bat'\r\n"
                    "  $Target = [IO.Path]::GetFileNameWithoutExtension($Uproject) + 'Editor'\r\n"
                    "  $ToolArgs = @($Target, 'Win64', '" + Item.Config + "', \"-Project=$Uproject\", '-WaitMutex')\r\n";
            }
            Script +=
                "  & $Tool @ToolArgs 2>&1 | ForEach-Object { \"$_\" } | Out-File -FilePath $Log -Append -Encoding utf8\r\n"
                "  $Code = $LASTEXITCODE\r\n";
        }
        Script +=
            "} catch {\r\n"
            "  \"$_\" | Out-File -FilePath $Log -Append -Encoding utf8\r\n"
            "  $Code = 1\r\n"
            "}\r\n"
            "if ($Code -eq 0) { Set-Status 'Succeeded' } else { Set-Status \"Failed (exit code $Code)\" }\r\n"
            "Remove-Item $PidFile -ErrorAction SilentlyContinue\r\n"
            "exit $Code\r\n";
        return Script;
    }

    // Keeps the build's files out of git, including the packaged game in the default Output folder.
    static void ExcludeFromGit(const std::string& ProjectDir)
    {
        const Git::Info Info = Git::GetInfo(ProjectDir);
        if (Info.IsGitRepo == false)
        {
            return;
        }
        std::error_code JobsError;
        std::error_code RootError;
        std::error_code RelativeError;
        const std::filesystem::path Jobs = std::filesystem::weakly_canonical(JobsRoot(ProjectDir), JobsError);
        const std::filesystem::path Root = std::filesystem::weakly_canonical(Info.Root, RootError);
        const std::filesystem::path Relative = std::filesystem::relative(Jobs, Root, RelativeError);
        if (JobsError.value() != 0 || RootError.value() != 0 || RelativeError.value() != 0 || Relative.empty() == true || Relative.begin()->string() == "..")
        {
            return;
        }
        Git::ExcludePath(Info.Root, "/" + Relative.generic_string() + "/");
    }

    static bool CreateNow(const std::string& ProjectDir, Job Item, std::string& Message)
    {
        if (Item.OutputDir.empty() == true)
        {
            Item.OutputDir = DefaultOutputDir(ProjectDir, Item.Name);
        }
        const std::filesystem::path Dir = JobDir(ProjectDir, Item.Name);
        std::error_code ErrorCode;
        std::filesystem::create_directories(Dir, ErrorCode);
        ExcludeFromGit(ProjectDir);
        const std::filesystem::path Script = Dir / SCRIPT_FILE;
        // Windows PowerShell reads a script without a BOM in the old ANSI code page, which garbles non-English paths.
        if (WriteFile(Script, "\xEF\xBB\xBF" + ScriptContent(ProjectDir, Item)) == false || WriteFile(Dir / SETTINGS_FILE, SaveSettings(Item)) == false)
        {
            Message = "Could not write the build files in " + Dir.string();
            return false;
        }
        const std::string Trigger = EveryDay(Item) == true ? "-Daily" : "-Weekly -DaysOfWeek " + DayList(Item, DAY_NAMES, ",");
        const std::string Body =
            "$Action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ' + [char]34 + " +
            PsQuote(Script.string()) + " + [char]34)\n"
            "$Trigger = New-ScheduledTaskTrigger " + Trigger + " -At " + PsQuote(TimeText(Item)) + "\n"
            "Register-ScheduledTask " + TaskArgs(ProjectDir, Item.Name) + " -Action $Action -Trigger $Trigger -Force | Out-Null";
        std::string Output;
        if (PowerShell(Body, Output) == true)
        {
            Message = "Set up " + Item.Name + ": " + ScheduleText(Item) + ".";
            return true;
        }
        Message = "Task Scheduler: " + Output;
        std::filesystem::remove(Dir / SETTINGS_FILE, ErrorCode);
        std::filesystem::remove(Script, ErrorCode);
        std::filesystem::remove(Dir, ErrorCode);
        return false;
    }

    // Ends the whole process tree: Unreal's build tools start several child programs that outlive their parent otherwise.
    // Only kills when the process number in pid.txt still belongs to the build, checked by its start time.
    static bool CancelNow(const std::string& ProjectDir, const std::string& Name, std::string& Message)
    {
        const std::filesystem::path Dir = JobDir(ProjectDir, Name);
        const std::string Body =
            "$Text = Get-Content -LiteralPath " + PsQuote((Dir / PID_FILE).string()) + " -TotalCount 1 -ErrorAction SilentlyContinue\n"
            "if ($Text) {\n"
            "  $Parts = ([string]$Text).Split('|')\n"
            "  try {\n"
            "    $Process = Get-Process -Id ([int]$Parts[0]) -ErrorAction SilentlyContinue\n"
            "    if ($Process -and $Parts.Count -ge 2 -and $Process.StartTime.ToFileTimeUtc() -eq [long]$Parts[1]) {\n"
            "      taskkill.exe /T /F /PID $Process.Id | Out-Null\n"
            "    }\n"
            "  } catch { }\n"
            "}\n"
            "Stop-ScheduledTask " + TaskArgs(ProjectDir, Name) + " -ErrorAction SilentlyContinue";
        std::string Output;
        PowerShell(Body, Output);
        std::error_code ErrorCode;
        std::filesystem::remove(Dir / PID_FILE, ErrorCode);
        WriteFile(Dir / STATUS_FILE, "Cancelled|" + Now() + "\r\n");
        Message = "Cancelled " + Name + ".";
        return true;
    }

    // Keeps the Output folder: deleting a schedule should not throw away builds it already made.
    static bool DeleteNow(const std::string& ProjectDir, const std::string& Name, std::string& Message)
    {
        const std::filesystem::path Dir = JobDir(ProjectDir, Name);
        std::error_code ErrorCode;
        if (std::filesystem::exists(Dir / PID_FILE, ErrorCode) == true)
        {
            std::string Ignored;
            CancelNow(ProjectDir, Name, Ignored);
        }
        std::string Output;
        const bool Removed = PowerShell("Unregister-ScheduledTask " + TaskArgs(ProjectDir, Name) + " -Confirm:$false", Output);
        for (const char* const File : {SETTINGS_FILE, SCRIPT_FILE, STATUS_FILE, PID_FILE, LOG_FILE})
        {
            std::filesystem::remove(Dir / File, ErrorCode);
        }
        std::filesystem::remove(Dir, ErrorCode);  // only succeeds when nothing else is left in it
        Message = "Deleted " + Name + ".";
        if (Removed == false)
        {
            Message += " (Task Scheduler had no task for it.)";
        }
        return true;
    }

    // One PowerShell call for every task in TASK_PATH, one "Name|State|NextRun" line each.
    static bool QueryTasks(std::map<std::string, TaskInfo>& Tasks)
    {
        const std::string Body =
            "Get-ScheduledTask -TaskPath " + PsQuote(TASK_PATH) + " -ErrorAction SilentlyContinue | ForEach-Object {\n"
            "  $Next = ($_ | Get-ScheduledTaskInfo).NextRunTime\n"
            "  $NextText = if ($Next) { $Next.ToString('yyyy-MM-dd HH:mm') } else { '' }\n"
            "  $_.TaskName + '|' + $_.State + '|' + $NextText\n"
            "}";
        std::string Output;
        if (PowerShell(Body, Output) == false)
        {
            return false;
        }
        std::istringstream Stream(Output);
        std::string Line;
        while (std::getline(Stream, Line).fail() == false)
        {
            const size_t First = Line.find('|');
            const size_t Second = First == std::string::npos ? std::string::npos : Line.find('|', First + 1);
            if (Second == std::string::npos)
            {
                continue;
            }
            TaskInfo& Info = Tasks[Line.substr(0, First)];
            Info.State = Line.substr(First + 1, Second - First - 1);
            Info.NextRun = Util::Trim(Line.substr(Second + 1));
        }
        return true;
    }

    static std::string ReadStatus(const std::filesystem::path& Dir)
    {
        return Util::Trim(Util::ReadFile(Dir / STATUS_FILE));
    }

    static std::vector<Job> LoadJobs(const std::string& ProjectDir)
    {
        std::vector<Job> Jobs;
        std::vector<std::filesystem::path> Dirs;
        std::vector<std::string> StatusBefore;
        std::error_code ErrorCode;
        for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(JobsRoot(ProjectDir), ErrorCode))
        {
            const std::filesystem::path Settings = Entry.path() / SETTINGS_FILE;
            if (std::filesystem::exists(Settings, ErrorCode) == false)
            {
                continue;
            }
            Job Item = LoadSettings(Settings);
            if (Item.Name.empty() == true)
            {
                continue;
            }
            Jobs.push_back(Item);
            Dirs.push_back(Entry.path());
            StatusBefore.push_back(ReadStatus(Entry.path()));
        }
        if (Jobs.empty() == true)
        {
            return Jobs;
        }
        std::map<std::string, TaskInfo> Tasks;
        const bool TasksRead = QueryTasks(Tasks);
        for (size_t Index = 0; Index < Jobs.size(); Index++)
        {
            Job& Item = Jobs[Index];
            const std::map<std::string, TaskInfo>::const_iterator Found = Tasks.find(TaskLeaf(ProjectDir, Item.Name));
            if (Found != Tasks.end())
            {
                Item.TaskState = Found->second.State;
                Item.NextRun = Found->second.NextRun;
            }
            // Read before and after the query: a run that started or finished in between must not look like a crash.
            const std::string Status = ReadStatus(Dirs[Index]);
            const size_t Bar = Status.find('|');
            Item.LastResult = Status.substr(0, Bar);
            Item.LastTime = Bar == std::string::npos ? std::string() : Status.substr(Bar + 1);
            if (TasksRead == true && Item.LastResult == RUNNING && Status == StatusBefore[Index] && Item.IsRunning() == false)
            {
                Item.LastResult = "Stopped unexpectedly";
            }
            Item.HasLog = std::filesystem::exists(Dirs[Index] / LOG_FILE, ErrorCode);
            Item.HasOutput = Item.OutputDir.empty() == false && std::filesystem::exists(Item.OutputDir, ErrorCode) == true;
        }
        return Jobs;
    }
};
