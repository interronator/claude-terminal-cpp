#pragma once
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "LiveView.h"
#include "ScreenshotFinder.h"

// A live picture of the 3D viewport of a Blender a session drives: a Python helper inside Blender saves a screenshot about
// once a second as <tag>_live.png in the session's screenshot folder, but only while the app keeps touching <tag>_live_wanted.
class BlenderLive
{
public:
    static constexpr const char* SCRIPT_NAME = "BlenderLiveCapture.py";
    static constexpr double STALE_FRAME_SECONDS = 6.0;
    static constexpr int SCRIPT_VERSION = 3;  // keep equal to LIVE_CAPTURE_VERSION inside SCRIPT
    static constexpr int STEERING_SCRIPT_VERSION = 2;  // first script that understands <tag>_live_view.json
    static constexpr double SNAPSHOT_TIMEOUT_SECONDS = 4.0;

    // True when the folder, or a folder directly inside it, holds a .blend file.
    static bool IsBlenderProject(const std::string& ProjectDir)
    {
        std::error_code ErrorCode;
        for (const std::filesystem::directory_entry& Item : std::filesystem::directory_iterator(ProjectDir, ErrorCode))
        {
            if (Item.path().extension() == ".blend")
            {
                return true;
            }
            if (Item.is_directory(ErrorCode) == false)
            {
                continue;
            }
            std::error_code InnerError;
            for (const std::filesystem::directory_entry& Inner : std::filesystem::directory_iterator(Item.path(), InnerError))
            {
                if (Inner.path().extension() == ".blend")
                {
                    return true;
                }
            }
        }
        return false;
    }

    static std::filesystem::path ScriptPath(const std::string& ProjectDir)
    {
        return std::filesystem::path(ProjectDir) / ScreenshotFinder::SHOT_DIRECTORY_NAME / SCRIPT_NAME;
    }

    static std::filesystem::path WantedPath(const std::string& OwnDirectory, const std::string& Tag)
    {
        return std::filesystem::path(OwnDirectory) / (Tag + "_live_wanted");
    }

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

    // Tells the capture script that someone is looking, so it keeps saving frames.
    static void TouchWanted(const std::string& OwnDirectory, const std::string& Tag)
    {
        std::error_code ErrorCode;
        std::filesystem::create_directories(OwnDirectory, ErrorCode);
        std::ofstream Stream(WantedPath(OwnDirectory, Tag), std::ios::binary | std::ios::trunc);
        Stream << "1";
    }

    static std::string StartLine(const std::string& ProjectDir, const std::string& OwnDirectory, const std::string& Tag)
    {
        return "exec(open(r'" + ScriptPath(ProjectDir).string() + "').read()); StartLiveCapture(r'" + OwnDirectory + "', '" + Tag + "')";
    }

    static std::string Guidance(const std::string& ProjectDir, const std::string& OwnDirectory, const std::string& Tag)
    {
        return "Blender live view: the terminal app can show a live picture of this session's Blender 3D viewport and lets the user mark areas "
               "they are unhappy with. When the user asks you to connect Blender, or when you start working in Blender, run this once in "
               "Blender through the Blender tool that executes Python (execute_blender_code): " +
               StartLine(ProjectDir, OwnDirectory, Tag) +
               " . It only saves " + Tag + "_live.png while the app asks for it; never rename, move or delete it, and don't treat it as one of "
               "your screenshots. Run the same exec(open(...).read()) followed by StopLiveCapture() in Blender when you are done. When the user sends you marked-up pictures, they come "
               "as one or more numbered snapshots of the model from different angles, with one note per number (numbers are unique across "
               "snapshots): open each marked image, look at each numbered mark and fix what its note says, then take a new "
               "screenshot to check the result.";
    }

    static std::string ConnectPrompt(const std::string& ProjectDir, const std::string& OwnDirectory, const std::string& Tag)
    {
        return "Please connect the terminal app's live Blender view: run this in Blender with the Blender Python tool (execute_blender_code), "
               "then reply with one short line: " +
               StartLine(ProjectDir, OwnDirectory, Tag);
    }

    static std::string FramePathFor(const std::string& OwnDirectory, const std::string& Tag)
    {
        return LiveView::FramePath(OwnDirectory, Tag).string();
    }

    static LiveView::Frame Latest(const std::string& OwnDirectory, const std::string& Tag)
    {
        return LiveView::Latest(OwnDirectory, Tag);
    }

private:
    static constexpr const char* SCRIPT = R"PY(# Written by Claude Terminal. Saves a screenshot of the largest 3D viewport about once a second as <tag>_live.png,
# but only while the terminal app keeps touching <tag>_live_wanted. Usage inside Blender Python:
#   exec(open(r'<this file>').read()); StartLiveCapture(r'<session screenshot folder>', '<tag>')
#   exec(open(r'<this file>').read()); StopLiveCapture()
import json
import math
import os
import time

import bpy
from mathutils import Quaternion, Vector

LIVE_CAPTURE_VERSION = 3
LIVE_CAPTURE_FAST_INTERVAL = 0.15
LIVE_CAPTURE_MAX_OBJECTS = 60

# Each call through the Blender tool runs in a fresh namespace, so the state lives where every call can reach it.
LiveCaptureState = bpy.app.driver_namespace.setdefault("ClaudeTerminalLiveCapture", {})


def StopLiveCapture():
    LiveCaptureState["Running"] = False
    OldTick = LiveCaptureState.pop("Tick", None)
    if OldTick is not None:
        try:
            bpy.app.timers.unregister(OldTick)
        except ValueError:
            pass
        print("LIVECAPTURE stopped")


def StartLiveCapture(Folder, Tag, Interval=1.0):
    StopLiveCapture()
    os.makedirs(Folder, exist_ok=True)
    LiveCaptureState.update(Folder=Folder, Tag=Tag, Interval=Interval, SlowInterval=Interval, Running=True, ViewMtime=0.0, Applied=None, SnapId=None)
    OldRequest = LiveCaptureReadRequest()
    if OldRequest is not None:
        LiveCaptureState["SnapId"] = OldRequest.get("Id")
    with open(os.path.join(Folder, Tag + "_live_info.json"), "w") as Handle:
        json.dump({"Version": LIVE_CAPTURE_VERSION}, Handle)
    LiveCaptureState["Tick"] = LiveCaptureTick
    bpy.app.timers.register(LiveCaptureTick, first_interval=Interval, persistent=True)
    print("LIVECAPTURE started " + Folder)


def LiveCaptureLargestViewport():
    Best = None
    BestSize = 0
    for Window in bpy.context.window_manager.windows:
        for Area in Window.screen.areas:
            if Area.type != "VIEW_3D":
                continue
            Size = Area.width * Area.height
            if Size > BestSize:
                Best = (Window, Area)
                BestSize = Size
    return Best


def LiveCaptureApplyView(Window, Area):
    # Returns (the view moved just now, the app is steering the view right now).
    Path = os.path.join(LiveCaptureState["Folder"], LiveCaptureState["Tag"] + "_live_view.json")
    if not os.path.isfile(Path):
        return False, False
    Modified = os.path.getmtime(Path)
    Steering = time.time() - Modified < 2.0
    if Modified == LiveCaptureState.get("ViewMtime"):
        return False, Steering
    LiveCaptureState["ViewMtime"] = Modified
    try:
        with open(Path, "r") as Handle:
            Totals = json.load(Handle)
    except (OSError, ValueError):
        return False, Steering
    Applied = LiveCaptureState.get("Applied")
    if Applied is None or Applied.get("Seq") != Totals.get("Seq"):
        Applied = {"Seq": Totals.get("Seq"), "Yaw": 0.0, "Pitch": 0.0, "Zoom": 0.0, "PanX": 0.0, "PanY": 0.0, "FrameAll": 0}
    Step = {Key: Totals.get(Key, 0.0) - Applied[Key] for Key in ("Yaw", "Pitch", "Zoom", "PanX", "PanY")}
    FrameAll = Totals.get("FrameAll", 0) != Applied["FrameAll"]
    LiveCaptureState["Applied"] = {"Seq": Totals.get("Seq"), "FrameAll": Totals.get("FrameAll", 0), **{Key: Totals.get(Key, 0.0) for Key in Step}}
    Region3D = Area.spaces.active.region_3d
    if Region3D.view_perspective == "CAMERA":
        Region3D.view_perspective = "PERSP"
    Rotation = Quaternion((0.0, 0.0, 1.0), -Step["Yaw"]) @ Region3D.view_rotation
    Rotation = Rotation @ Quaternion((1.0, 0.0, 0.0), -Step["Pitch"])
    Rotation.normalize()
    Region3D.view_rotation = Rotation
    Region3D.view_distance = max(0.001, Region3D.view_distance * math.exp(-Step["Zoom"]))
    Right = Rotation @ Vector((1.0, 0.0, 0.0))
    Up = Rotation @ Vector((0.0, 1.0, 0.0))
    Region3D.view_location = Region3D.view_location + (Up * Step["PanY"] - Right * Step["PanX"]) * Region3D.view_distance
    if FrameAll:
        WindowRegion = next((Region for Region in Area.regions if Region.type == "WINDOW"), None)
        with bpy.context.temp_override(window=Window, area=Area, region=WindowRegion, screen=Window.screen):
            bpy.ops.view3d.view_all(center=False)
    LiveCaptureRedraw(Window, Area)
    return True, True


def LiveCaptureRedraw(Window, Area):
    WindowRegion = next((Region for Region in Area.regions if Region.type == "WINDOW"), None)
    with bpy.context.temp_override(window=Window, area=Area, region=WindowRegion, screen=Window.screen):
        try:
            bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=1)
        except Exception:
            Area.tag_redraw()


def LiveCaptureReadRequest():
    Path = os.path.join(LiveCaptureState["Folder"], LiveCaptureState["Tag"] + "_snap_request.json")
    if not os.path.isfile(Path):
        return None
    try:
        with open(Path, "r") as Handle:
            return json.load(Handle)
    except (OSError, ValueError):
        return None


def LiveCaptureCameraState(Area):
    Space = Area.spaces.active
    Region3D = Space.region_3d
    return {
        "Rotation": list(Region3D.view_rotation),
        "Location": list(Region3D.view_location),
        "Distance": Region3D.view_distance,
        "Perspective": Region3D.view_perspective,
        "Lens": Space.lens,
        "ClipStart": Space.clip_start,
        "ClipEnd": Space.clip_end,
    }


def LiveCaptureSceneState():
    Layer = bpy.context.view_layer
    Active = Layer.objects.active
    ActiveName = Active.name if Active is not None else None
    Selected = [Item.name for Item in Layer.objects if Item.select_get()]
    Objects = []
    for Item in Layer.objects:
        Entry = {
            "Name": Item.name,
            "Type": Item.type,
            "Dimensions": list(Item.dimensions),
            "Location": list(Item.location),
            "Parent": Item.parent.name if Item.parent is not None else None,
            "Visible": Item.visible_get(),
            "Modifiers": [Modifier.name + ":" + Modifier.type for Modifier in Item.modifiers],
            "Materials": [Slot.material.name for Slot in Item.material_slots if Slot.material is not None],
        }
        if Item.type == "MESH":
            Entry["Verts"] = len(Item.data.vertices)
            Entry["Faces"] = len(Item.data.polygons)
        Objects.append(Entry)
    Objects.sort(key=lambda Entry: (Entry["Name"] != ActiveName, Entry["Name"] not in Selected))
    Units = bpy.context.scene.unit_settings
    return {
        "Units": Units.system,
        "LengthUnit": Units.length_unit,
        "UnitScale": Units.scale_length,
        "ObjectCount": len(Objects),
        "Active": ActiveName,
        "Selected": Selected,
        "Objects": Objects[:LIVE_CAPTURE_MAX_OBJECTS],
    }


def LiveCaptureSnapshot(Window, Area, Request):
    Folder = LiveCaptureState["Folder"]
    Tag = LiveCaptureState["Tag"]
    Id = str(Request["Id"])
    LiveCaptureState["SnapId"] = Request["Id"]
    Base = os.path.join(Folder, Tag + "_snap_" + Id)
    LiveCaptureRedraw(Window, Area)
    with bpy.context.temp_override(window=Window, area=Area, screen=Window.screen):
        bpy.ops.screen.screenshot_area(filepath=Base + ".png")
    Blend = None
    if Request.get("SaveBlend"):
        try:
            bpy.ops.wm.save_as_mainfile(filepath=Base + ".blend", copy=True)
            Blend = Base + ".blend"
        except Exception as Error:
            print("LIVECAPTURE blend copy failed: " + str(Error))
    State = {"Id": Id, "Image": Base + ".png", "Blend": Blend, "BlendFile": bpy.data.filepath}
    for Key, Reader in (("Camera", lambda: LiveCaptureCameraState(Area)), ("Scene", LiveCaptureSceneState)):
        try:
            State[Key] = Reader()
        except Exception as Error:
            print("LIVECAPTURE " + Key + " failed: " + str(Error))
    with open(Base + ".json.tmp", "w") as Handle:
        json.dump(State, Handle)
    os.replace(Base + ".json.tmp", Base + ".json")


def RestoreReviewView(StatePath, ShotPath=None):
    with open(StatePath, "r") as Handle:
        Camera = json.load(Handle)["Camera"]
    Found = LiveCaptureLargestViewport()
    if Found is None:
        print("RESTOREVIEW no 3D viewport is open")
        return
    Window, Area = Found
    Space = Area.spaces.active
    Region3D = Space.region_3d
    Region3D.view_perspective = Camera["Perspective"] if Camera["Perspective"] in ("PERSP", "ORTHO") else "PERSP"
    Region3D.view_rotation = Quaternion(Camera["Rotation"])
    Region3D.view_location = Vector(Camera["Location"])
    Region3D.view_distance = Camera["Distance"]
    Space.lens = Camera["Lens"]
    LiveCaptureRedraw(Window, Area)
    if ShotPath:
        with bpy.context.temp_override(window=Window, area=Area, screen=Window.screen):
            bpy.ops.screen.screenshot_area(filepath=ShotPath)
        print("RESTOREVIEW saved " + ShotPath)


def LiveCaptureStep():
    Folder = LiveCaptureState["Folder"]
    Tag = LiveCaptureState["Tag"]
    LiveCaptureState["Interval"] = LiveCaptureState["SlowInterval"]
    Found = LiveCaptureLargestViewport()
    if Found is None:
        return
    Window, Area = Found
    RequestPath = os.path.join(Folder, Tag + "_snap_request.json")
    if os.path.isfile(RequestPath) and time.time() - os.path.getmtime(RequestPath) < 3.0:
        LiveCaptureState["Interval"] = LIVE_CAPTURE_FAST_INTERVAL
        Request = LiveCaptureReadRequest()
        if Request is not None and Request.get("Id") != LiveCaptureState.get("SnapId"):
            LiveCaptureApplyView(Window, Area)
            LiveCaptureSnapshot(Window, Area, Request)
            return
    Wanted = os.path.join(Folder, Tag + "_live_wanted")
    if not os.path.isfile(Wanted) or time.time() - os.path.getmtime(Wanted) > 10.0:
        return
    Moved, Steering = LiveCaptureApplyView(Window, Area)
    if Steering:
        LiveCaptureState["Interval"] = LIVE_CAPTURE_FAST_INTERVAL
    Pending = os.path.join(Folder, Tag + "_live_pending.png")
    if not Moved:
        LiveCaptureRedraw(Window, Area)
    with bpy.context.temp_override(window=Window, area=Area, screen=Window.screen):
        bpy.ops.screen.screenshot_area(filepath=Pending)
    if os.path.isfile(Pending) and os.path.getsize(Pending) > 0:
        os.replace(Pending, os.path.join(Folder, Tag + "_live.png"))


def LiveCaptureTick():
    if LiveCaptureState.get("Running") is not True:
        return None
    try:
        LiveCaptureStep()
    except Exception as Error:
        print("LIVECAPTURE failed, stopping: " + str(Error))
        LiveCaptureState["Running"] = False
        return None
    return LiveCaptureState["Interval"]
)PY";
};
