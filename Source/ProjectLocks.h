#pragma once
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "SessionRequests.h"

// Named locks so sessions working in the same project take turns on builds, test runs and Blender instead of colliding.
// Sessions run their command through Lock.ps1, which waits for the lock, runs the command and always releases it.
class ProjectLocks
{
public:
    static std::filesystem::path Dir()
    {
        return SessionRequests::InboxDir() / "Locks";
    }

    static std::filesystem::path ScriptPath()
    {
        return Dir() / "Lock.ps1";
    }

    static void EnsureScript()
    {
        std::error_code ErrorCode;
        std::filesystem::create_directories(Dir(), ErrorCode);
        std::ifstream Existing(ScriptPath(), std::ios::binary);
        const std::string Current((std::istreambuf_iterator<char>(Existing)), std::istreambuf_iterator<char>());
        if (Current == SCRIPT)
        {
            return;
        }
        std::ofstream Output(ScriptPath(), std::ios::binary | std::ios::trunc);
        Output << SCRIPT;
    }

    static std::string Guidance(const std::string& Tag)
    {
        return "Shared resources: other sessions may work in the same project at the same time. Run every build, test run and Blender job "
               "through the lock script so only one session uses it at a time, for example: powershell -NoProfile -File \"" + ScriptPath().string() +
               "\" -Name <ProjectFolderName>-Build -Tag " + Tag + " -Command \"build.bat\". The script waits its turn, runs the command and "
               "releases the lock even if the command fails; use the names <ProjectFolderName>-Build, -Test and -Blender. Keep each locked "
               "command short and never hold a lock while idle. The lock is refreshed while the command runs, and counts as abandoned after 30 minutes without a refresh or when its owner is gone. When a build or test "
               "fails in files you did not touch, they are probably another session's unfinished work: do not fix or revert them, and do not "
               "report them as your failures; mention them in one line and carry on with your own files.";
    }

private:
    static constexpr const char* SCRIPT = R"PS(param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$Tag,
    [Parameter(Mandatory = $true)][string]$Command,
    [int]$StaleMinutes = 30
)
$LockPath = Join-Path $PSScriptRoot (($Name -replace '[^A-Za-z0-9._-]', '_') + '.lock')
$Owner = $Tag + "`n" + (Get-Date).ToString('o') + "`n" + $PID + "`n" + [guid]::NewGuid().ToString()
$Told = $false
while ($true)
{
    try
    {
        $Stream = [System.IO.File]::Open($LockPath, 'CreateNew', 'Write', 'None')
        $Bytes = [System.Text.Encoding]::UTF8.GetBytes($Owner)
        $Stream.Write($Bytes, 0, $Bytes.Length)
        $Stream.Close()
        break
    }
    catch [System.IO.IOException]
    {
        $Text = $null
        try { $Text = [System.IO.File]::ReadAllText($LockPath) } catch { }
        $Info = Get-Item $LockPath -ErrorAction SilentlyContinue
        if ($null -ne $Text -and $null -ne $Info)
        {
            $Lines = $Text -split "`n"
            $OwnerAlive = $true
            $OwnerPid = 0
            if ($Lines.Count -ge 3 -and [int]::TryParse($Lines[2].Trim(), [ref]$OwnerPid))
            {
                $OwnerAlive = ($null -ne (Get-Process -Id $OwnerPid -ErrorAction SilentlyContinue))
            }
            $Aged = ((Get-Date) - $Info.LastWriteTime).TotalMinutes -gt $StaleMinutes
            if ($Aged -or $OwnerAlive -eq $false)
            {
                $Aside = $LockPath + '.stale.' + [guid]::NewGuid().ToString()
                try { Move-Item $LockPath $Aside -ErrorAction Stop } catch { continue }
                $Moved = $null
                try { $Moved = [System.IO.File]::ReadAllText($Aside) } catch { }
                if ($Moved -ne $Text)
                {
                    try { Move-Item $Aside $LockPath -ErrorAction Stop } catch { }
                }
                Remove-Item $Aside -Force -ErrorAction SilentlyContinue
                continue
            }
        }
        if ($Told -eq $false)
        {
            $Holder = if ($null -ne $Text) { ($Text -split "`n")[0] } else { '?' }
            Write-Host "Waiting for lock '$Name' held by $Holder..."
            $Told = $true
        }
        Start-Sleep -Seconds 3
    }
}
$ExitCode = 1
try
{
    $Child = Start-Process -FilePath 'cmd.exe' -ArgumentList @('/c', $Command) -NoNewWindow -PassThru
    $null = $Child.Handle
    while ($Child.WaitForExit(10000) -eq $false)
    {
        try { [System.IO.File]::SetLastWriteTimeUtc($LockPath, [DateTime]::UtcNow) } catch { }
    }
    $ExitCode = $Child.ExitCode
}
finally
{
    $Current = $null
    try { $Current = [System.IO.File]::ReadAllText($LockPath) } catch { }
    if ($Current -eq $Owner)
    {
        Remove-Item $LockPath -Force -ErrorAction SilentlyContinue
    }
}
exit $ExitCode
)PS";
};
