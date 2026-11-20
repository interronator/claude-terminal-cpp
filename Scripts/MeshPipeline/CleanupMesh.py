"""Blender headless: high-poly textured GLB -> game-ready FBX + baked textures.

blender -b -P CleanupMesh.py -- --input in.glb --name Crate --kind prop --size 1.2 [--tris 8000] [--tex 2048]
Outputs to Meshes/Generated/<SM_|SK_><Name>/ : FBX, T_<Name>_BaseColor/Normal/ORM PNGs, Report.json, Renders/.
"""
import bpy
import bmesh
import json
import math
import os
import sys
import time
import argparse
import numpy as np
from mathutils import Vector

ROOT = os.path.join(os.path.expanduser("~"), "ClaudeGeneral")


class CleanupPipeline:
    def __init__(self, Args):
        self.Args = Args
        Prefix = "SK_" if Args.kind == "character" else "SM_"
        self.AssetName = Prefix + Args.name
        self.OutDir = os.path.join(Args.out or os.path.join(ROOT, "Meshes", "Generated"), self.AssetName)
        os.makedirs(os.path.join(self.OutDir, "Renders"), exist_ok=True)
        self.Report = {"asset": self.AssetName, "input": Args.input}
        self.High = None
        self.Low = None

    def Run(self):
        Start = time.time()
        bpy.ops.wm.read_factory_settings(use_empty=True)
        self.ImportHigh()
        self.Orient()
        self.BuildLow()
        self.Unwrap()
        self.BakeAll()
        self.Check()
        self.Export()
        self.Report["seconds"] = round(time.time() - Start, 1)
        self.Report["renders"] = [f"Renders/{Label}.png" for Label in ("front", "threequarter", "side", "back")]
        with open(os.path.join(self.OutDir, "Report.json"), "w") as File:
            json.dump(self.Report, File, indent=2)
        self.RenderPreview()
        print("[CleanupMesh] done", json.dumps(self.Report))

    def ImportHigh(self):
        bpy.ops.import_scene.gltf(filepath=self.Args.input)
        Meshes = [Obj for Obj in bpy.context.scene.objects if Obj.type == "MESH"]
        for Obj in bpy.context.scene.objects:
            Obj.select_set(Obj.type == "MESH")
        bpy.context.view_layer.objects.active = Meshes[0]
        if len(Meshes) > 1:
            bpy.ops.object.join()
        self.High = bpy.context.view_layer.objects.active
        self.High.name = self.AssetName + "_High"
        for Obj in list(bpy.context.scene.objects):
            if Obj != self.High:
                bpy.data.objects.remove(Obj)
        self.High.parent = None
        bpy.ops.object.select_all(action="DESELECT")
        self.High.select_set(True)
        bpy.context.view_layer.objects.active = self.High
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        self.Report["high_tris"] = sum(len(Poly.vertices) - 2 for Poly in self.High.data.polygons)

    def Bounds(self, Obj):
        Corners = [Obj.matrix_world @ Vector(Corner) for Corner in Obj.bound_box]
        Low = Vector((min(C.x for C in Corners), min(C.y for C in Corners), min(C.z for C in Corners)))
        High = Vector((max(C.x for C in Corners), max(C.y for C in Corners), max(C.z for C in Corners)))
        return Low, High

    def Orient(self):
        self.High.rotation_euler[2] = math.radians(self.Args.rotate_z)
        bpy.ops.object.transform_apply(rotation=True)
        Low, High = self.Bounds(self.High)
        Extent = High - Low
        Reference = Extent.z if self.Args.kind == "character" else max(Extent.x, Extent.y, Extent.z)
        Factor = self.Args.size / Reference
        self.High.scale = (Factor, Factor, Factor)
        bpy.ops.object.transform_apply(scale=True)
        Low, High = self.Bounds(self.High)
        Center = (Low + High) * 0.5
        self.High.location = Vector((-Center.x, -Center.y, -Low.z))
        bpy.ops.object.transform_apply(location=True)
        Low, High = self.Bounds(self.High)
        self.Report["bounds_m"] = [round(V, 4) for V in (High - Low)]

    def BuildLow(self):
        self.Low = self.High.copy()
        self.Low.data = self.High.data.copy()
        self.Low.name = self.AssetName
        self.Low.data.name = self.AssetName
        bpy.context.collection.objects.link(self.Low)
        self.Low.data.materials.clear()
        bpy.ops.object.select_all(action="DESELECT")
        self.Low.select_set(True)
        bpy.context.view_layer.objects.active = self.Low
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.mesh.remove_doubles(threshold=0.00005)
        bpy.ops.object.mode_set(mode="OBJECT")
        Mid = self.Args.tris * 6
        if len(self.Low.data.polygons) > Mid:
            Mod = self.Low.modifiers.new("Pre", "DECIMATE")
            Mod.ratio = Mid / len(self.Low.data.polygons)
            bpy.ops.object.modifier_apply(modifier=Mod.name)
        Mod = self.Low.modifiers.new("Final", "DECIMATE")
        Mod.ratio = min(1.0, self.Args.tris / max(len(self.Low.data.polygons), 1))
        Mod.use_collapse_triangulate = True
        bpy.ops.object.modifier_apply(modifier=Mod.name)
        self.FillHoles()
        bpy.ops.object.shade_smooth()
        self.Report["low_tris"] = len(self.Low.data.polygons)

    def BoundaryEdges(self):
        Mesh = bmesh.new()
        Mesh.from_mesh(self.Low.data)
        Count = sum(1 for Edge in Mesh.edges if len(Edge.link_faces) == 1)
        Mesh.free()
        return Count

    def FillHoles(self):
        Before = self.BoundaryEdges()
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.mesh.fill_holes(sides=16)
        bpy.ops.mesh.normals_make_consistent(inside=False)
        bpy.ops.object.mode_set(mode="OBJECT")
        self.Report["holes_filled"] = max(0, Before - self.BoundaryEdges())

    def Unwrap(self):
        bpy.context.view_layer.objects.active = self.Low
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=0.004, area_weight=0.6)
        bpy.ops.uv.pack_islands(margin=0.004)
        bpy.ops.object.mode_set(mode="OBJECT")

    def SetupCycles(self):
        Scene = bpy.context.scene
        Scene.render.engine = "CYCLES"
        Scene.cycles.samples = 16
        Scene.cycles.device = "CPU"
        self.Report["bake_device"] = "CPU"
        Prefs = bpy.context.preferences.addons["cycles"].preferences
        for Kind in ("OPTIX", "CUDA"):
            try:
                Prefs.compute_device_type = Kind
                Prefs.get_devices()
                if len([D for D in Prefs.devices if D.type == Kind]) == 0:
                    continue
                for Device in Prefs.devices:
                    Device.use = Device.type == Kind
                Scene.cycles.device = "GPU"
                self.Report["bake_device"] = Kind
                return
            except Exception:
                continue

    def NewImage(self, Name, Colorspace):
        Image = bpy.data.images.new(Name, self.Args.tex, self.Args.tex, alpha=False)
        Image.colorspace_settings.name = Colorspace
        return Image

    def BakeTo(self, Image, BakeType):
        Nodes = self.Low.data.materials[0].node_tree.nodes
        Node = Nodes.new("ShaderNodeTexImage")
        Node.image = Image
        Nodes.active = Node
        Bake = bpy.context.scene.render.bake
        Bake.use_selected_to_active = True
        Bake.cage_extrusion = self.Args.size * 0.02
        Bake.max_ray_distance = self.Args.size * 0.1
        Bake.margin = 16
        Bake.use_clear = True
        if BakeType == "DIFFUSE":
            Bake.use_pass_direct = False
            Bake.use_pass_indirect = False
            Bake.use_pass_color = True
        if BakeType == "NORMAL":
            Bake.normal_space = "TANGENT"
        bpy.ops.object.select_all(action="DESELECT")
        self.High.select_set(True)
        self.Low.select_set(True)
        bpy.context.view_layer.objects.active = self.Low
        bpy.ops.object.bake(type=BakeType)
        Nodes.remove(Node)

    def ChannelPass(self, Name, Input):
        for Mat in self.High.data.materials:
            Nodes = Mat.node_tree.nodes
            Bsdf = next(N for N in Nodes if N.type == "BSDF_PRINCIPLED")
            Emit = Nodes.new("ShaderNodeEmission")
            Output = next(N for N in Nodes if N.type == "OUTPUT_MATERIAL")
            Source = Bsdf.inputs[Input]
            if Source.is_linked:
                Mat.node_tree.links.new(Source.links[0].from_socket, Emit.inputs["Color"])
            else:
                Value = Source.default_value
                Emit.inputs["Color"].default_value = (Value, Value, Value, 1.0)
            Mat.node_tree.links.new(Emit.outputs[0], Output.inputs["Surface"])
        Image = self.NewImage(Name, "Non-Color")
        self.BakeTo(Image, "EMIT")
        for Mat in self.High.data.materials:
            Nodes = Mat.node_tree.nodes
            Bsdf = next(N for N in Nodes if N.type == "BSDF_PRINCIPLED")
            Output = next(N for N in Nodes if N.type == "OUTPUT_MATERIAL")
            Mat.node_tree.links.new(Bsdf.outputs[0], Output.inputs["Surface"])
        return Image

    def Pixels(self, Image):
        Buffer = np.empty(len(Image.pixels), dtype=np.float32)
        Image.pixels.foreach_get(Buffer)
        return Buffer.reshape(self.Args.tex, self.Args.tex, 4)

    def Save(self, Array, FileName, Colorspace="Non-Color"):
        Image = bpy.data.images.new(FileName, self.Args.tex, self.Args.tex, alpha=False)
        Image.colorspace_settings.name = Colorspace
        Out = np.ones((self.Args.tex, self.Args.tex, 4), dtype=np.float32)
        Out[:, :, : Array.shape[2]] = Array
        Image.pixels.foreach_set(Out.ravel())
        Image.filepath_raw = os.path.join(self.OutDir, FileName)
        Image.file_format = "PNG"
        Image.save()
        return Image

    def BakeAll(self):
        self.SetupCycles()
        Mat = bpy.data.materials.new("M_" + self.Args.name)
        Mat.use_nodes = True
        self.Low.data.materials.append(Mat)
        Name = self.Args.name
        Color = self.NewImage("Bake_Color", "sRGB")
        self.BakeTo(Color, "DIFFUSE")
        Rough = self.NewImage("Bake_Rough", "Non-Color")
        self.BakeTo(Rough, "ROUGHNESS")
        Metal = self.ChannelPass("Bake_Metal", "Metallic")
        Normal = self.NewImage("Bake_Normal", "Non-Color")
        self.BakeTo(Normal, "NORMAL")
        Ao = self.NewImage("Bake_Ao", "Non-Color")
        self.BakeTo(Ao, "AO")

        ColorPix = self.Pixels(Color)[:, :, :3]
        RoughPix = self.Pixels(Rough)[:, :, 0]
        MetalPix = self.Pixels(Metal)[:, :, 0]
        AoPix = self.Pixels(Ao)[:, :, 0]
        NormalPix = self.Pixels(Normal)[:, :, :3].copy()
        if self.Args.flip_green:
            NormalPix[:, :, 1] = 1.0 - NormalPix[:, :, 1]
        OrmPix = np.dstack([AoPix, RoughPix, MetalPix])

        BaseImage = self.Save(ColorPix, f"T_{Name}_BaseColor.png", "sRGB")
        NormalImage = self.Save(NormalPix, f"T_{Name}_Normal.png")
        OrmImage = self.Save(OrmPix, f"T_{Name}_ORM.png")
        self.Report["textures"] = [f"T_{Name}_BaseColor.png", f"T_{Name}_Normal.png", f"T_{Name}_ORM.png"]
        self.Report["metallic_mean"] = round(float(MetalPix.mean()), 4)
        self.Report["roughness_mean"] = round(float(RoughPix.mean()), 4)

        Nodes = Mat.node_tree.nodes
        Links = Mat.node_tree.links
        Nodes.clear()
        Out = Nodes.new("ShaderNodeOutputMaterial")
        Bsdf = Nodes.new("ShaderNodeBsdfPrincipled")
        Links.new(Bsdf.outputs[0], Out.inputs[0])
        BaseTex = Nodes.new("ShaderNodeTexImage")
        BaseTex.image = BaseImage
        Links.new(BaseTex.outputs["Color"], Bsdf.inputs["Base Color"])
        OrmTex = Nodes.new("ShaderNodeTexImage")
        OrmTex.image = OrmImage
        Split = Nodes.new("ShaderNodeSeparateColor")
        Links.new(OrmTex.outputs["Color"], Split.inputs[0])
        Links.new(Split.outputs[1], Bsdf.inputs["Roughness"])
        Links.new(Split.outputs[2], Bsdf.inputs["Metallic"])
        NormalTex = Nodes.new("ShaderNodeTexImage")
        NormalTex.image = NormalImage
        NormalMap = Nodes.new("ShaderNodeNormalMap")
        Links.new(NormalTex.outputs["Color"], NormalMap.inputs["Color"])
        Links.new(NormalMap.outputs["Normal"], Bsdf.inputs["Normal"])

    def Check(self):
        Mesh = bmesh.new()
        Mesh.from_mesh(self.Low.data)
        NonManifold = sum(1 for Edge in Mesh.edges if len(Edge.link_faces) != 2)
        Loose = sum(1 for Vert in Mesh.verts if len(Vert.link_edges) == 0)
        Degenerate = sum(1 for Face in Mesh.faces if Face.calc_area() < 1e-10)
        UvLayer = Mesh.loops.layers.uv.active
        OutOfRange = 0
        for Face in Mesh.faces:
            for Loop in Face.loops:
                U, V = Loop[UvLayer].uv
                if U < -0.001 or U > 1.001 or V < -0.001 or V > 1.001:
                    OutOfRange += 1
        Mesh.free()
        Low, High = self.Bounds(self.Low)
        self.Report["check"] = {
            "non_manifold_edges": NonManifold, "loose_verts": Loose, "degenerate_faces": Degenerate,
            "uv_out_of_range_loops": OutOfRange, "min_z": round(Low.z, 5),
            "bounds_m": [round(V, 3) for V in (High - Low)],
        }

    def Export(self):
        bpy.ops.object.select_all(action="DESELECT")
        self.Low.select_set(True)
        bpy.context.view_layer.objects.active = self.Low
        Path = os.path.join(self.OutDir, self.AssetName + ".fbx")
        bpy.ops.export_scene.fbx(
            filepath=Path, use_selection=True, object_types={"MESH"}, apply_scale_options="FBX_SCALE_UNITS",
            axis_forward="-Y", axis_up="Z", mesh_smooth_type="FACE", path_mode="COPY", embed_textures=False)
        self.Report["fbx"] = Path
        bpy.ops.wm.save_as_mainfile(filepath=os.path.join(self.OutDir, self.AssetName + ".blend"))

    def RenderPreview(self):
        Scene = bpy.context.scene
        self.High.hide_render = True
        self.High.hide_viewport = True
        for Engine in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
            try:
                Scene.render.engine = Engine
                break
            except TypeError:
                continue
        Scene.render.resolution_x = 1024
        Scene.render.resolution_y = 1024
        World = bpy.data.worlds.new("W")
        World.use_nodes = True
        World.node_tree.nodes["Background"].inputs[0].default_value = (0.55, 0.57, 0.6, 1)
        World.node_tree.nodes["Background"].inputs[1].default_value = 1.2
        Scene.world = World
        Sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
        Sun.data.energy = 3.0
        Sun.rotation_euler = (math.radians(50), 0, math.radians(35))
        bpy.context.collection.objects.link(Sun)
        Cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("Cam"))
        bpy.context.collection.objects.link(Cam)
        Scene.camera = Cam
        Low, High = self.Bounds(self.Low)
        Center = (Low + High) * 0.5
        Radius = max((High - Low).length * 1.3, 0.5)
        for Label, Angle in (("front", -90), ("threequarter", -45), ("side", 0), ("back", 90)):
            Rad = math.radians(Angle)
            Cam.location = Center + Vector((math.cos(Rad) * Radius, math.sin(Rad) * Radius, Radius * 0.25))
            Direction = Center - Cam.location
            Cam.rotation_euler = Direction.to_track_quat("-Z", "Y").to_euler()
            Scene.render.filepath = os.path.join(self.OutDir, "Renders", f"{Label}.png")
            bpy.ops.render.render(write_still=True)


def Main():
    Argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    Parser = argparse.ArgumentParser()
    Parser.add_argument("--input", required=True)
    Parser.add_argument("--name", required=True)
    Parser.add_argument("--kind", choices=["prop", "character"], default="prop")
    Parser.add_argument("--size", type=float, required=True, help="character: height in m; prop: longest side in m")
    Parser.add_argument("--tris", type=int, default=0)
    Parser.add_argument("--tex", type=int, default=2048)
    Parser.add_argument("--rotate-z", type=float, default=0.0)
    Parser.add_argument("--flip-green", type=int, default=1, help="1 = DirectX normal map for Unreal")
    Parser.add_argument("--out", default="")
    Args = Parser.parse_args(Argv)
    if Args.tris == 0:
        Args.tris = 30000 if Args.kind == "character" else 12000
    CleanupPipeline(Args).Run()


Main()
