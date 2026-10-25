"""Cuts the person out of every frame, for the sticker outline. Run once per clip
with a Python that has rembg installed (pip install "rembg[cpu]"):

    python tools/make_masks.py videos/my_clip.mp4 main
    python tools/make_masks.py videos/my_clip.mp4 main --scene 1 --model birefnet-general-lite

Writes videos/masks/main/0000.png ... (one per source frame at 60 fps), which
tools/make_video.py picks up. The masks are plain PNGs, so make_video.py itself
doesn't need rembg. --scene redoes one scene (0 = first) with another model,
e.g. the slower but sharper BiRefNet for a dark or blurry shot.

On Windows, if importing rembg fails with a "No such file or directory" error
deep inside numba, set NUMBA_DISABLE_JIT=1 first (it's a path-length limit).
"""
import argparse
import pathlib
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageChops, ImageFilter, ImageStat
from rembg import new_session, remove
from scipy import ndimage

ROOT = pathlib.Path(__file__).resolve().parent.parent

SOURCE_FPS = 60          # must match make_video.py
W = 240                  # mask width, same as the frames make_video.py works on
WORK_W = 480             # segment at 2x for cleaner edges
MODEL = "u2net_human_seg"


def extract(src, out_dir):
    subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(src),
         "-vf", f"fps={SOURCE_FPS},scale={WORK_W}:-2:flags=lanczos",
         str(out_dir / "%04d.png")],
        check=True)
    return sorted(out_dir.glob("*.png"))


def cut_points(frames):
    """Indices where a new scene starts, so smoothing never blends across a cut."""
    thumbs = [f.convert("L").filter(ImageFilter.GaussianBlur(8)).resize((12, 21)) for f in frames]
    diffs = [ImageStat.Stat(ImageChops.difference(a, b)).mean[0] for a, b in zip(thumbs, thumbs[1:])]
    return {i + 1 for i, d in enumerate(diffs) if d > 20}


def solidify(soft):
    """Threshold, drop specks, fill holes."""
    solid = soft > 0.5
    labels, n = ndimage.label(solid)
    if n > 1:
        sizes = ndimage.sum(solid, labels, range(1, n + 1))
        solid = np.isin(labels, 1 + np.flatnonzero(sizes >= 0.15 * sizes.max()))
    return ndimage.binary_fill_holes(solid)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src", type=pathlib.Path)
    ap.add_argument("name")
    ap.add_argument("--model", default=MODEL)
    ap.add_argument("--scene", type=int, help="only redo this scene (0 = first)")
    args = ap.parse_args()
    out = ROOT / "videos" / "masks" / args.name
    out.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        frames = [Image.open(p).convert("RGB") for p in extract(args.src, pathlib.Path(tmp))]
    cuts = cut_points(frames)
    scene_of = [sum(1 for c in cuts if c <= i) for i in range(len(frames))]
    todo = [i for i in range(len(frames)) if args.scene is None or scene_of[i] == args.scene]
    session = new_session(args.model)
    soft = {}
    for n, i in enumerate(todo):
        soft[i] = np.asarray(remove(frames[i], session=session, only_mask=True), dtype=np.float32) / 255
        print(f"\r{args.name}: segmented {n + 1}/{len(todo)}", end="", flush=True)
    print()

    h = round(frames[0].height * W / WORK_W)
    for i in todo:
        # Average with neighbours in the same scene so the outline doesn't flicker.
        acc, weight = soft[i] * 2, 2
        for j in (i - 1, i + 1):
            if j in soft and scene_of[j] == scene_of[i]:
                acc, weight = acc + soft[j], weight + 1
        mask = Image.fromarray(solidify(acc / weight).astype(np.uint8) * 255)
        mask.resize((W, h), Image.LANCZOS).save(out / f"{i:04d}.png")
    print(f"{args.name}: {len(todo)} masks with {args.model}, {len(cuts) + 1} scenes -> {out.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
