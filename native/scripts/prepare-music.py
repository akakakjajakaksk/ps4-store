#!/usr/bin/env python3
"""Convert the user's repository tracks into the native player's bounded PCM format."""

import argparse
import math
from pathlib import Path
import subprocess
import tempfile
import wave

TRACKS = (("fight-ps4.mp3", "fight.wav"), ("acendaofarol-ps4.mp3", "acendaofarol.wav"))
MAX_SOURCE_BYTES = 64 * 1024 * 1024
MAX_OUTPUT_BYTES = 128 * 1024 * 1024


def convert(source: Path, destination: Path):
    if not source.is_file() or not 0 < source.stat().st_size <= MAX_SOURCE_BYTES:
        raise ValueError(f"Missing or oversized repository music: {source.name}")
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", str(source)],
        check=True, capture_output=True, text=True, timeout=30,
    )
    seconds = float(probe.stdout.strip())
    if not math.isfinite(seconds) or not 0 < seconds <= 600:
        raise ValueError(f"Track must have a valid duration of at most 10 minutes: {source.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix=destination.stem + "-", suffix=".wav", dir=destination.parent, delete=False) as temp:
        temp_path = Path(temp.name)
    try:
        subprocess.run(
            ["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", str(source), "-map", "0:a:0", "-vn",
             "-map_metadata", "-1", "-fflags", "+bitexact", "-flags:a", "+bitexact", "-ac", "2",
             "-ar", "48000", "-acodec", "pcm_s16le", "-t", "601", "-f", "wav", str(temp_path)],
            check=True, timeout=180,
        )
        with wave.open(str(temp_path), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) != (2, 2, 48000, "NONE"):
                raise ValueError(f"Unexpected converted PCM format: {source.name}")
            if not 0 < wav.getnframes() <= 600 * 48000:
                raise ValueError(f"Invalid converted track length: {source.name}")
            duration = wav.getnframes() / wav.getframerate()
        if temp_path.stat().st_size > MAX_OUTPUT_BYTES:
            raise ValueError(f"Converted track is too large: {source.name}")
        temp_path.replace(destination)
        print(f"Prepared {destination.name}: PCM16 stereo 48000 Hz, {duration:.1f} s, {destination.stat().st_size} bytes")
    finally:
        temp_path.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "assets/music")
    args = parser.parse_args()
    for original, packaged in TRACKS:
        convert(args.repo_root / original, args.output / packaged)


if __name__ == "__main__":
    main()
