import argparse
import shutil
from pathlib import Path

import torch


class MusicGenerator:
    def __init__(self, CheckpointDir):
        from acestep.pipeline_ace_step import ACEStepPipeline

        self.Pipeline = ACEStepPipeline(checkpoint_dir=CheckpointDir, dtype="bfloat16", torch_compile=False, cpu_offload=False)

    def Generate(self, Style, Lyrics, Seconds, Seed, Steps, OutputPath):
        self.Pipeline(
            audio_duration=Seconds,
            prompt=Style,
            lyrics=Lyrics,
            infer_step=Steps,
            guidance_scale=15.0,
            scheduler_type="euler",
            cfg_type="apg",
            omega_scale=10.0,
            manual_seeds=[Seed],
            guidance_interval=0.5,
            guidance_interval_decay=0.0,
            min_guidance_scale=3.0,
            use_erg_tag=True,
            use_erg_lyric=True,
            use_erg_diffusion=True,
            guidance_scale_text=0.0,
            guidance_scale_lyric=0.0,
            save_path=str(OutputPath),
        )


def Main():
    Parser = argparse.ArgumentParser(description="Text to music with ACE-Step (Apache 2.0)")
    Parser.add_argument("style", help='comma separated tags, e.g. "ambient, dark, slow, strings, no vocals"')
    Parser.add_argument("output")
    Parser.add_argument("--seconds", type=float, default=60.0)
    Parser.add_argument("--seed", type=int, default=0)
    Parser.add_argument("--count", type=int, default=1)
    Parser.add_argument("--steps", type=int, default=60)
    Parser.add_argument("--lyrics", default="[instrumental]")
    Parser.add_argument("--checkpoints", default="D:/AudioGen/AceStepCheckpoints")
    Args = Parser.parse_args()
    Generator = MusicGenerator(Args.checkpoints)
    Output = Path(Args.output)
    Output.parent.mkdir(parents=True, exist_ok=True)
    for Index in range(Args.count):
        Target = Output if Args.count == 1 else Output.with_name(f"{Output.stem}_{Index + 1:02d}{Output.suffix}")
        Generator.Generate(Args.style, Args.lyrics, Args.seconds, Args.seed + Index, Args.steps, Target)
        print("wrote", Target)


if __name__ == "__main__":
    Main()
