import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

TARGET_LUFS = {"sfx": -18.0, "voice": -20.0, "music": -23.0}
PEAK_DB = -1.0
SAMPLE_RATE = 48000


class AudioCleaner:
    def __init__(self, Kind, LoopSeconds, FadeMs):
        self.Kind = Kind
        self.LoopSeconds = LoopSeconds
        self.FadeMs = FadeMs

    def Run(self, InputPath, OutputPath):
        Trim = "silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.05,areverse,silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.05,areverse"
        Stats = self.Measure(InputPath, Trim)
        Target = TARGET_LUFS[self.Kind]
        Norm = (
            f"loudnorm=I={Target}:TP={PEAK_DB}:LRA=11:linear=true"
            f":measured_I={Stats['input_i']}:measured_TP={Stats['input_tp']}"
            f":measured_LRA={Stats['input_lra']}:measured_thresh={Stats['input_thresh']}"
            f":offset={Stats['target_offset']}"
        )
        Filters = [Trim, Norm, f"aresample={SAMPLE_RATE}"]
        if self.LoopSeconds > 0:
            Filters.append(self.LoopFilter())
        else:
            Fade = self.FadeMs / 1000.0
            Filters.append(f"afade=t=in:d={Fade}")
            Filters.append(f"areverse,afade=t=in:d={Fade},areverse")
        self.Ffmpeg(["-i", str(InputPath), "-af", ",".join(Filters), "-ar", str(SAMPLE_RATE), "-c:a", "pcm_s16le", str(OutputPath)])

    def LoopFilter(self):
        Cross = self.LoopSeconds
        return (
            f"asplit=2[A][B];[A]atrim=start=0:end={Cross},asetpts=PTS-STARTPTS[Head];"
            f"[B]asetpts=PTS-STARTPTS[Body];[Body][Head]acrossfade=d={Cross}"
        )

    def Measure(self, InputPath, Trim):
        Filter = f"{Trim},loudnorm=I={TARGET_LUFS[self.Kind]}:TP={PEAK_DB}:LRA=11:print_format=json"
        Process = subprocess.run(["ffmpeg", "-hide_banner", "-nostats", "-i", str(InputPath), "-af", Filter, "-f", "null", "-"], capture_output=True, text=True)
        Match = re.search(r"\{[^{}]*\"input_i\"[^{}]*\}", Process.stderr, re.S)
        if Match is None:
            sys.exit("loudness measurement failed:\n" + Process.stderr[-1500:])
        return json.loads(Match.group(0))

    def Ffmpeg(self, Args):
        Process = subprocess.run(["ffmpeg", "-y", "-hide_banner", "-loglevel", "error"] + Args, capture_output=True, text=True)
        if Process.returncode != 0:
            sys.exit(Process.stderr)


def Main():
    Parser = argparse.ArgumentParser(description="Trim, loudness-normalize and export a game-ready 48 kHz 16-bit WAV")
    Parser.add_argument("input")
    Parser.add_argument("output")
    Parser.add_argument("--kind", choices=sorted(TARGET_LUFS), default="sfx")
    Parser.add_argument("--loop-seconds", type=float, default=0.0, help="crossfade length that makes the file loop seamlessly (0 = no loop)")
    Parser.add_argument("--fade-ms", type=float, default=8.0)
    Args = Parser.parse_args()
    Output = Path(Args.output)
    Output.parent.mkdir(parents=True, exist_ok=True)
    AudioCleaner(Args.kind, Args.loop_seconds, Args.fade_ms).Run(Path(Args.input), Output)
    print("wrote", Output)


if __name__ == "__main__":
    Main()
