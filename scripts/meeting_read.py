"""Read a local text snapshot with Piper; playback occurs only when invoked."""

import argparse
import io
import json
from pathlib import Path
import wave


def synthesize(voice, text, playback):
    if not text.strip():
        raise ValueError("There is no text to read")
    frames = 0
    count = 0
    for chunk in voice.synthesize(text):
        buffer = io.BytesIO()
        with wave.open(buffer, "wb") as output:
            output.setnchannels(chunk.sample_channels)
            output.setsampwidth(chunk.sample_width)
            output.setframerate(chunk.sample_rate)
            output.writeframes(chunk.audio_int16_bytes)
        if playback is not None:
            playback(buffer.getvalue())
        frames += len(chunk.audio_int16_bytes) // (chunk.sample_width * chunk.sample_channels)
        count += 1
        print(f"Piper: completed speech block {count}", flush=True)
    return {"blocks": count, "audio_frames": frames}


def play_windows(audio):
    import winsound
    winsound.PlaySound(audio, winsound.SND_MEMORY | winsound.SND_SYNC)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--render-only", action="store_true", help="Synthesize without playing audio, for diagnostics")
    options = parser.parse_args()
    text = options.input.read_text(encoding="utf-8-sig")
    if not text.strip():
        parser.error("There is no text to read")
    from piper import PiperVoice
    print("Piper: loading local voice on CPU", flush=True)
    voice = PiperVoice.load(str(options.model), use_cuda=False)
    result = synthesize(voice, text, None if options.render_only else play_windows)
    print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
