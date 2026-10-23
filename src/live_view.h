#pragma once
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "screenshot_finder.h"

// A rough live view of an Unreal editor or game a session runs offscreen: a Python helper inside Unreal saves a small
// screenshot about once a second as <tag>_live.png in the session's screenshot folder, and the pane shows the newest one.
class LiveView
{
public:
    struct Frame
    {
        std::string Path;
        long long Key = 0;  // changes whenever the file is replaced, finer than a second
        double AgeSeconds = 0;
    };

    static constexpr const char* SCRIPT_NAME = "LiveCapture.py";

    static bool IsUnrealProject(const std::string& ProjectDir)
    {
        std::error_code ErrorCode;
        for (const std::filesystem::directory_entry& Item : std::filesystem::directory_iterator(ProjectDir, ErrorCode))
        {
            if (Item.path().extension() == ".uproject")
            {
                return true;
            }
        }
        return false;
    }

    static std::filesystem::path ScriptPath(const std::string& ProjectDir)
    {
        return std::filesystem::path(ProjectDir) / ScreenshotFinder::SHOT_DIRECTORY_NAME / SCRIPT_NAME;
    }

    static std::filesystem::path FramePath(const std::string& OwnDirectory, const std::string& Tag)
    {
        return std::filesystem::path(OwnDirectory) / (Tag + "_live.png");
    }

    // Writes the helper script when it is missing or out of date.
    static void EnsureScript(const std::string& ProjectDir)
    {
        const std::filesystem::path Path = ScriptPath(ProjectDir);
        std::error_code ErrorCode;
        if (std::filesystem::exists(Path, ErrorCode) == true)
        {
            std::ifstream Existing(Path, std::ios::binary);
            std::stringstream Buffer;
            Buffer << Existing.rdbuf();
            if (Buffer.str() == SCRIPT)
            {
                return;
            }
        }
        std::filesystem::create_directories(Path.parent_path(), ErrorCode);
        std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
        Stream << SCRIPT;
    }

    static std::string Guidance(const std::string& ProjectDir, const std::string& OwnDirectory, const std::string& Tag)
    {
        const std::string Script = ScriptPath(ProjectDir).string();
        const std::string Start = "exec(open(r'" + Script + "').read()); StartLiveCapture(r'" + OwnDirectory + "', '" + Tag + "')";
        return "Live view: the terminal app shows a live picture of any Unreal editor or game this session runs. Whenever you launch an "
               "editor or play check for this session, make the Python that runs inside it (the script passed with -ExecCmds) run this first: " +
               Start + " . It needs -RenderOffScreen (a -nullrhi editor draws nothing). In a shared editor that is already open (a remote "
               "execution bridge), run the same line when you start working in it and run StopLiveCapture() when you are done, so you never "
               "leave a capture running in an editor other sessions use. The capture only saves " + Tag + "_live.png; never rename, move or "
               "delete it, and don't treat it as one of your screenshots.";
    }

    // The session's newest complete live frame, or an empty path when there is none.
    static Frame Latest(const std::string& OwnDirectory, const std::string& Tag)
    {
        Frame Result;
        const std::filesystem::path Path = FramePath(OwnDirectory, Tag);
        std::error_code ErrorCode;
        const std::filesystem::file_time_type Modified = std::filesystem::last_write_time(Path, ErrorCode);
        if (ErrorCode)
        {
            return Result;
        }
        Result.Path = Path.string();
        Result.Key = static_cast<long long>(Modified.time_since_epoch().count());
        Result.AgeSeconds = std::chrono::duration<double>(std::filesystem::file_time_type::clock::now() - Modified).count();
        return Result;
    }

private:
    // Requests each frame under a pending name and only renames it to <tag>_live.png once its size stops changing, so the
    // app never reads a half-written file.
    static constexpr const char* SCRIPT = R"PY(# Written by Claude Terminal. Saves a small screenshot about once a second as <tag>_live.png so the
# terminal app can show what this Unreal editor or game is doing. Usage inside Unreal Python:
#   exec(open(r'<this file>').read()); StartLiveCapture(r'<session screenshot folder>', '<tag>')
#   StopLiveCapture()
import glob
import os
import time

import unreal

LiveCaptureState = globals().setdefault("LiveCaptureState", {})


def StartLiveCapture(Folder, Tag, Interval=1.0, Width=960, Height=540):
    StopLiveCapture()
    os.makedirs(Folder, exist_ok=True)
    LiveCaptureState.update(Folder=Folder, Tag=Tag, Interval=Interval, Width=Width, Height=Height,
                            NextAt=0.0, RequestedAt=None, PendingSize=-1)
    LiveCaptureState["Handle"] = unreal.register_slate_post_tick_callback(LiveCaptureTick)
    unreal.log("LIVECAPTURE started " + Folder)


def StopLiveCapture():
    Handle = LiveCaptureState.pop("Handle", None)
    if Handle is not None:
        unreal.unregister_slate_post_tick_callback(Handle)
        unreal.log("LIVECAPTURE stopped")


def LiveCapturePendingPath():
    return os.path.join(LiveCaptureState["Folder"], LiveCaptureState["Tag"] + "_live_pending.png")


def LiveCaptureFind():
    Pending = LiveCapturePendingPath()
    if os.path.isfile(Pending):
        return Pending
    # Some capture paths ignore the folder and write under Saved/Screenshots, keeping the file name.
    ShotDir = unreal.Paths.convert_relative_path_to_full(unreal.Paths.screen_shot_dir())
    Pattern = os.path.join(ShotDir, "**", "*" + LiveCaptureState["Tag"] + "_live_pending*.png")
    Matches = [Path for Path in glob.glob(Pattern, recursive=True) if os.path.getmtime(Path) >= LiveCaptureState["RequestedAt"] - 1.0]
    return max(Matches, key=os.path.getmtime) if Matches else None


def LiveCaptureStep():
    Now = time.time()
    if LiveCaptureState["RequestedAt"] is not None:
        Found = LiveCaptureFind()
        if Found is None:
            if Now - LiveCaptureState["RequestedAt"] > 10.0:
                LiveCaptureState["RequestedAt"] = None
            return
        Size = os.path.getsize(Found)
        if Size == 0 or Size != LiveCaptureState["PendingSize"]:
            LiveCaptureState["PendingSize"] = Size
            return
        os.replace(Found, os.path.join(LiveCaptureState["Folder"], LiveCaptureState["Tag"] + "_live.png"))
        LiveCaptureState["RequestedAt"] = None
        LiveCaptureState["PendingSize"] = -1
        return
    if Now < LiveCaptureState["NextAt"]:
        return
    LiveCaptureState["NextAt"] = Now + LiveCaptureState["Interval"]
    LiveCaptureState["RequestedAt"] = Now
    unreal.AutomationLibrary.take_high_res_screenshot(LiveCaptureState["Width"], LiveCaptureState["Height"], LiveCapturePendingPath())


def LiveCaptureTick(DeltaSeconds):
    try:
        LiveCaptureStep()
    except Exception as Error:
        unreal.log_warning("LIVECAPTURE failed, stopping: " + str(Error))
        StopLiveCapture()
)PY";
};
