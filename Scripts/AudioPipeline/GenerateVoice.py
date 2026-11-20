import argparse
import csv
from pathlib import Path

import numpy
import soundfile


class VoiceGenerator:
    def __init__(self, Engine, VoicePath, Emotion):
        self.Engine = Engine
        self.VoicePath = VoicePath
        self.Emotion = Emotion
        if Engine == "chatterbox":
            from chatterbox.tts import ChatterboxTTS

            self.Model = ChatterboxTTS.from_pretrained(device="cuda")
            self.Rate = self.Model.sr
        else:
            from kokoro import KPipeline

            self.Model = KPipeline(lang_code="a")
            self.Rate = 24000

    def Speak(self, Text):
        if self.Engine == "chatterbox":
            Wave = self.Model.generate(Text, audio_prompt_path=self.VoicePath, exaggeration=self.Emotion)
            return Wave.squeeze().cpu().numpy()
        Parts = [numpy.asarray(Audio) for _, _, Audio in self.Model(Text, voice=self.VoicePath or "af_heart")]
        return numpy.concatenate(Parts)


def ReadLines(Source):
    Path_ = Path(Source)
    if Path_.suffix.lower() != ".csv" or Path_.exists() == False:
        return [("Line01", Source)]
    with open(Path_, newline="", encoding="utf-8") as File:
        return [(Row[0].strip(), Row[1].strip()) for Row in csv.reader(File) if len(Row) >= 2]


def Main():
    Parser = argparse.ArgumentParser(description="Text to voice lines (Chatterbox or Kokoro)")
    Parser.add_argument("source", help='a sentence, or a CSV with "name,text" rows')
    Parser.add_argument("outdir")
    Parser.add_argument("--engine", choices=["chatterbox", "kokoro"], default="chatterbox")
    Parser.add_argument("--voice", default=None, help="reference .wav (chatterbox) or voice name such as af_heart (kokoro)")
    Parser.add_argument("--emotion", type=float, default=0.5)
    Args = Parser.parse_args()
    Out = Path(Args.outdir)
    Out.mkdir(parents=True, exist_ok=True)
    Generator = VoiceGenerator(Args.engine, Args.voice, Args.emotion)
    for Name, Text in ReadLines(Args.source):
        Target = Out / f"{Name}.wav"
        soundfile.write(str(Target), Generator.Speak(Text), Generator.Rate)
        print("wrote", Target)


if __name__ == "__main__":
    Main()
