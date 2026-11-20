import argparse
from pathlib import Path

import torch
import torchaudio


class SfxGenerator:
    def __init__(self, Device):
        from mmaudio.eval_utils import all_model_cfg
        from mmaudio.model.flow_matching import FlowMatching
        from mmaudio.model.networks import get_my_mmaudio
        from mmaudio.model.utils.features_utils import FeaturesUtils

        Config = all_model_cfg["large_44k_v2"]
        Config.download_if_needed()
        self.Device = Device
        self.Dtype = torch.bfloat16
        self.Net = get_my_mmaudio(Config.model_name).to(Device, self.Dtype).eval()
        self.Net.load_weights(torch.load(Config.model_path, map_location=Device, weights_only=True))
        self.Features = FeaturesUtils(
            tod_vae_ckpt=Config.vae_path,
            synchformer_ckpt=Config.synchformer_ckpt,
            enable_conditions=True,
            mode=Config.mode,
            bigvgan_vocoder_ckpt=Config.bigvgan_16k_path,
            need_vae_encoder=False,
        ).to(Device, self.Dtype).eval()
        self.Config = Config
        self.FlowMatching = FlowMatching

    def Generate(self, Prompt, Negative, Seconds, Seed, VideoPath):
        from mmaudio.eval_utils import generate, load_video

        Generator = torch.Generator(device=self.Device).manual_seed(Seed)
        Flow = self.FlowMatching(min_sigma=0, inference_mode="euler", num_steps=25)
        Clip = None
        if VideoPath is not None:
            Info = load_video(VideoPath, Seconds)
            Clip = Info.clip_frames.unsqueeze(0)
            Seconds = Info.duration_sec
        self.Config.seq_cfg.duration = Seconds
        self.Net.update_seq_lengths(self.Config.seq_cfg.latent_seq_len, self.Config.seq_cfg.clip_seq_len, self.Config.seq_cfg.sync_seq_len)
        with torch.no_grad():
            Audios = generate(Clip, None, [Prompt], negative_text=[Negative], feature_utils=self.Features, net=self.Net, fm=Flow, rng=Generator, cfg_strength=4.5)
        return Audios.float().cpu()[0], self.Config.seq_cfg.sampling_rate


def Main():
    Parser = argparse.ArgumentParser(description="Text (or video) to sound effect with MMAudio")
    Parser.add_argument("prompt")
    Parser.add_argument("output")
    Parser.add_argument("--seconds", type=float, default=4.0)
    Parser.add_argument("--seed", type=int, default=0)
    Parser.add_argument("--count", type=int, default=1)
    Parser.add_argument("--negative", default="music, speech")
    Parser.add_argument("--video", default=None)
    Args = Parser.parse_args()
    Generator = SfxGenerator("cuda")
    Output = Path(Args.output)
    Output.parent.mkdir(parents=True, exist_ok=True)
    for Index in range(Args.count):
        Audio, Rate = Generator.Generate(Args.prompt, Args.negative, Args.seconds, Args.seed + Index, Args.video)
        Target = Output if Args.count == 1 else Output.with_name(f"{Output.stem}_{Index + 1:02d}{Output.suffix}")
        torchaudio.save(str(Target), Audio, Rate)
        print("wrote", Target)


if __name__ == "__main__":
    Main()
