"""Turn a local video into the RGB frame data embedded in wmfishcpu."""
import sys
import zlib
from pathlib import Path
from subprocess import run

WIDTH, HEIGHT, FPS = 60, 46, 24


def main(video, output):
    filters = (
        f"fps={FPS},scale={WIDTH}:{HEIGHT}:"
        "force_original_aspect_ratio=decrease:flags=lanczos,"
        f"pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2:color=white"
    )
    frames = run(
        ["ffmpeg", "-v", "error", "-nostdin", "-i", video, "-map", "0:v:0",
         "-vf", filters, "-frames:v", "720", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"],
        check=True, capture_output=True,
    ).stdout
    count, remainder = divmod(len(frames), WIDTH * HEIGHT * 3)
    if count < 2 or remainder:
        raise SystemExit("The video must contain at least two complete frames.")

    compressed = zlib.compress(frames, 9)
    target = Path(output)
    temp = target.with_suffix(".h.tmp")
    with temp.open("w", encoding="ascii") as stream:
        stream.write(
            f"/* Generated locally from the user's video. */\n"
            f"#define FISH_WIDTH {WIDTH}\n#define FISH_HEIGHT {HEIGHT}\n"
            f"#define FISH_COUNT {count}\n#define FISH_BYTES {len(frames)}UL\n"
            f"#define FISH_FPS {FPS}.0\n"
            "static const unsigned char fish_compressed[] = {\n"
        )
        for i in range(0, len(compressed), 20):
            stream.write(",".join(map(str, compressed[i:i + 20])) + ",\n")
        stream.write("};\n")
    temp.replace(target)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: python3 generate_frames.py input.mp4 fish_frames.h")
    main(*sys.argv[1:])
