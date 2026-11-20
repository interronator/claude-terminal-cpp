"""Concept images with Z-Image-Turbo (Apache 2.0, local). Run with D:\\MeshGen\\VenvImage\\Scripts\\python.exe.

GenerateConcepts.py --kind prop --subject "wooden military ammo crate with rope handles" --count 6 --out DIR
GenerateConcepts.py --kind character --subject "US army rifleman, olive uniform, helmet, plate carrier" --count 6 --out DIR
Writes DIR/Concept_<seed>.png plus ContactSheet.png (numbered) for the pick-the-best step.
"""
import argparse
import os
import time

os.environ.setdefault("HF_HOME", r"D:\MeshGen\HfCache")
import torch
from PIL import Image, ImageDraw
from diffusers import ZImagePipeline

TEMPLATES = {
    "prop": (
        "{subject}, single object, centered, full object visible with margin, three-quarter front view from slightly above, "
        "photorealistic game asset reference, realistic worn materials, physically based, detailed surface, "
        "even soft studio lighting, plain flat light grey background, no shadow on background, no text, no other objects"
    ),
    "character": (
        "{subject}, full body head to toe, standing in a neutral A-pose with arms slightly away from the body and legs "
        "slightly apart, facing the camera straight on, front view, centered, photorealistic realistic proportions, "
        "detailed fabric and gear, character concept for a 3D game model, even soft studio lighting, "
        "plain flat light grey background, no shadow on background, no text, no weapon in hands, no other objects"
    ),
}


def Main() -> None:
    Parser = argparse.ArgumentParser()
    Parser.add_argument("--kind", choices=list(TEMPLATES), default="prop")
    Parser.add_argument("--subject", required=True)
    Parser.add_argument("--count", type=int, default=6)
    Parser.add_argument("--out", required=True)
    Parser.add_argument("--seed", type=int, default=100)
    Parser.add_argument("--size", type=int, default=1024)
    Args = Parser.parse_args()
    os.makedirs(Args.out, exist_ok=True)
    Prompt = TEMPLATES[Args.kind].format(subject=Args.subject)
    Width, Height = (Args.size, Args.size) if Args.kind == "prop" else (Args.size * 3 // 4 // 16 * 16, Args.size)

    Start = time.time()
    Pipe = ZImagePipeline.from_pretrained("Tongyi-MAI/Z-Image-Turbo", torch_dtype=torch.bfloat16)
    Pipe.to("cuda")
    Paths = []
    for Index in range(Args.count):
        Seed = Args.seed + Index
        Image_ = Pipe(prompt=Prompt, height=Height, width=Width, num_inference_steps=9, guidance_scale=0.0,
                      generator=torch.Generator("cuda").manual_seed(Seed)).images[0]
        Path = os.path.join(Args.out, f"Concept_{Seed}.png")
        Image_.save(Path)
        Paths.append(Path)
    Thumb = 384
    Cols = min(3, len(Paths))
    Rows = (len(Paths) + Cols - 1) // Cols
    Sheet = Image.new("RGB", (Cols * Thumb, Rows * Thumb), "white")
    for Index, Path in enumerate(Paths):
        Tile = Image.open(Path)
        Tile.thumbnail((Thumb, Thumb))
        Sheet.paste(Tile, ((Index % Cols) * Thumb, (Index // Cols) * Thumb))
        ImageDraw.Draw(Sheet).text(((Index % Cols) * Thumb + 6, (Index // Cols) * Thumb + 4), str(Index + 1), fill="red")
    Sheet.save(os.path.join(Args.out, "ContactSheet.png"))
    print(f"[GenerateConcepts] {len(Paths)} images in {time.time() - Start:.0f}s, peak VRAM "
          f"{torch.cuda.max_memory_allocated() / 2**30:.1f} GB")


if __name__ == "__main__":
    Main()
