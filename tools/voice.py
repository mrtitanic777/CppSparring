"""CppSparring's voice for Bit: Kokoro (kokoro-onnx), run on this computer.

    python voice.py KOKORO_FOLDER [VOICE] [SPEED]

Prints READY once the voice is loaded (or NO and why). Then each line read from stdin is "ID TEXT"; it is said into a WAV file and
"WAV ID PATH" is printed (or "ERR ID WHY"). The app plays the file and deletes it.

KOKORO_FOLDER holds kokoro-*.onnx and voices-*.bin (CodeMonkey's speech folder works: its packages folder next to it is used too).
"""
import glob
import os
import shutil
import sys
import tempfile
import wave


def main() -> None:
    sys.stdin.reconfigure(encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    base = sys.argv[1] if len(sys.argv) > 1 else ""
    chosen = sys.argv[2] if len(sys.argv) > 2 else "af_heart"
    speed = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
    packages = os.path.join(os.path.dirname(os.path.abspath(base)), "python")
    if os.path.isdir(packages) and packages not in sys.path:
        sys.path.append(packages)
    models = sorted(glob.glob(os.path.join(base, "kokoro-*.onnx")), key=lambda p: (".int8" in p, ".fp16" in p, p))
    voices = sorted(glob.glob(os.path.join(base, "voices-*.bin")))
    if not models or not voices:
        print("NO no Kokoro files in " + base, flush=True)
        return
    try:
        import numpy as np
        import kokoro_onnx
    except Exception as exc:  # noqa: BLE001
        print("NO " + type(exc).__name__ + ": " + str(exc).replace("\n", " "), flush=True)
        return

    longest = getattr(kokoro_onnx, "MAX_PHONEME_LENGTH", 510)
    rate = getattr(kokoro_onnx, "SAMPLE_RATE", 24000)

    class Kokoro(kokoro_onnx.Kokoro):
        """Runs the model with the input names and types it declares (model files and package versions disagree on them)."""

        def _create_audio(self, phonemes, voice, speed):
            phonemes = phonemes[:longest]
            tokens = np.array(self.tokenizer.tokenize(phonemes), dtype=np.int64)
            style = np.array(voice[len(tokens)], dtype=np.float32)
            kinds = {i.name: i.type for i in self.sess.get_inputs()}
            ids = "input_ids" if "input_ids" in kinds else "tokens"
            pace = np.array([speed], dtype=np.float32 if "float" in kinds.get("speed", "float") else np.int32)
            audio = self.sess.run(None, {ids: [[0, *tokens, 0]], "style": style, "speed": pace})[0]
            return audio, rate

    try:
        engine = Kokoro(models[0], voices[0])
        names = engine.get_voices()
        if chosen not in names:
            chosen = "af_heart" if "af_heart" in names else names[0]
        engine.create("Hi.", voice=chosen, speed=speed, lang="en-us")   # the first one is slow: get it out of the way
    except Exception as exc:  # noqa: BLE001
        print("NO " + type(exc).__name__ + ": " + str(exc).replace("\n", " "), flush=True)
        return
    print("READY " + chosen, flush=True)

    folder = tempfile.mkdtemp(prefix="cppsparring-voice-")
    lang = "en-gb" if chosen[:1] == "b" else "en-us"
    for line in sys.stdin:
        ident, _, text = line.strip().partition(" ")
        if not ident or not text:
            continue
        try:
            samples, sr = engine.create(text[:1500], voice=chosen, speed=speed, lang=lang)
            pcm = (np.clip(samples, -1.0, 1.0) * 32767).astype("<i2")
            path = os.path.join(folder, ident + ".wav")
            with wave.open(path, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(int(sr))
                w.writeframes(pcm.tobytes())
            print("WAV " + ident + " " + path, flush=True)
        except Exception as exc:  # noqa: BLE001
            print("ERR " + ident + " " + type(exc).__name__ + ": " + str(exc).replace("\n", " "), flush=True)
    shutil.rmtree(folder, ignore_errors=True)   # the app is gone: tidy up what it didn't play


if __name__ == "__main__":
    main()
