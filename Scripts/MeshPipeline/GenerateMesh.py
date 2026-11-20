"""Image -> textured PBR GLB with TRELLIS.2. Run with D:\MeshGen\Venv\Scripts\python.exe (see GenerateMesh.bat)."""
import argparse
import os
import sys
import time

os.environ["OPENCV_IO_ENABLE_OPENEXR"] = "1"
os.environ["PYTORCH_CUDA_ALLOC_CONF"] = "expandable_segments:True"
os.environ.setdefault("HF_HOME", r"D:\MeshGen\HfCache")
FORK_DIR = r"D:\MeshGen\Trellis2Fork"
sys.path.insert(0, FORK_DIR)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.chdir(FORK_DIR)

from PIL import Image
import torch
from Cutout import CutoutPlainBackground


class NoRembg(torch.nn.Module):
    """Stand-in for RMBG-2.0 (non-commercial licence): input images must already carry alpha."""
    def __init__(self, *Args, **Kwargs):
        super().__init__()

    def forward(self, Image_):
        raise RuntimeError("Input image has no alpha; cut it out first")


def InstallFlexGemmShim() -> None:
    """The Windows flex_gemm wheel ships without its triton kernels; texture sampling only needs this one."""
    import types
    import flex_gemm.kernels as Kernels

    def IndiceWeighedSumFwd(Feats, Indices, Weights):
        if Indices.dtype == torch.uint32:
            Indices = Indices.view(torch.int32)
        Safe = Indices.long().clamp(min=0, max=Feats.shape[0] - 1)
        Valid = (Indices >= 0) & (Indices < Feats.shape[0])
        Gathered = Feats[Safe]
        return (Gathered * (Weights * Valid).unsqueeze(-1).to(Feats.dtype)).sum(dim=1)

    if hasattr(Kernels, "triton") == False:
        Shim = types.ModuleType("flex_gemm.kernels.triton")
        Shim.indice_weighed_sum_fwd = IndiceWeighedSumFwd
        Kernels.triton = Shim


def Main() -> None:
    Parser = argparse.ArgumentParser()
    Parser.add_argument("Image")
    Parser.add_argument("Output")
    Parser.add_argument("--seed", type=int, default=1)
    Parser.add_argument("--seeds", type=int, default=1, help="N>1 writes <Output stem>_s<seed>.glb per seed")
    Parser.add_argument("--pipeline", default="1024_cascade", choices=["512", "1024", "1024_cascade", "1536_cascade"])
    Parser.add_argument("--texture", type=int, default=4096)
    Parser.add_argument("--faces", type=int, default=1000000)
    Args = Parser.parse_args()

    import trellis2.pipelines.rembg as RembgModule
    RembgModule.BiRefNet = NoRembg
    from trellis2.pipelines import Trellis2ImageTo3DPipeline
    import o_voxel
    InstallFlexGemmShim()

    print("[PROGRESS] load 0", flush=True)
    Start = time.time()
    Pipeline = Trellis2ImageTo3DPipeline.from_pretrained("microsoft/TRELLIS.2-4B")
    Pipeline.cuda()
    LoadSeconds = time.time() - Start
    print("[PROGRESS] load 100", flush=True)

    Source = Image.open(Args.Image)
    if Source.mode != "RGBA" or min(Source.getchannel("A").getextrema()) == 255:
        Source = CutoutPlainBackground(Source)

    for Index in range(Args.seeds):
        Seed = Args.seed + Index
        if Args.seeds > 1:
            Stem, Extension = os.path.splitext(Args.Output)
            OutPath = f"{Stem}_s{Seed}{Extension}"
        else:
            OutPath = Args.Output
        print(f"[PROGRESS] generate {Index * 100 // Args.seeds}", flush=True)
        Start = time.time()
        Mesh = Pipeline.run(Source, seed=Seed, pipeline_type=Args.pipeline)[0]
        Mesh.simplify(16777216)
        GenSeconds = time.time() - Start
        print(f"[PROGRESS] export {(Index * 100 + 60) // Args.seeds}", flush=True)

        Start = time.time()
        Glb = o_voxel.postprocess.to_glb(
            vertices=Mesh.vertices, faces=Mesh.faces, attr_volume=Mesh.attrs, coords=Mesh.coords,
            attr_layout=Mesh.layout, voxel_size=Mesh.voxel_size,
            aabb=[[-0.5, -0.5, -0.5], [0.5, 0.5, 0.5]],
            decimation_target=Args.faces, texture_size=Args.texture,
            remesh=True, remesh_band=1, remesh_project=0, verbose=False)
        os.makedirs(os.path.dirname(os.path.abspath(OutPath)), exist_ok=True)
        Glb.export(OutPath, extension_webp=False)
        print(f"[GenerateMesh] load {LoadSeconds:.0f}s generate {GenSeconds:.0f}s export {time.time() - Start:.0f}s "
              f"peak VRAM {torch.cuda.max_memory_allocated() / 2**30:.1f} GB -> {OutPath}", flush=True)
        print(f"[PROGRESS] export {(Index + 1) * 100 // Args.seeds}", flush=True)


if __name__ == "__main__":
    Main()
