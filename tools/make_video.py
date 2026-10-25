"""Turn a phone video into an MJPEG header the ESP32-C3 plays straight from flash.

    python tools/make_video.py videos/my_clip.mp4 main
    python tools/make_video.py videos/my_other_clip.mp4 alt --masks main

Writes firmware/CyberID/video_<name>.h and videos/<name>_preview.jpg (a contact
sheet so you can check the result before flashing). If videos/masks/<masks>/
exists (made once by tools/make_masks.py), the per-scene effects in SCENE_EFFECT
can use it: text flowing around the person, or a sticker outline.
"""
import argparse
import io
import math
import pathlib
import random
import subprocess
import tempfile

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont, ImageStat

ROOT = pathlib.Path(__file__).resolve().parent.parent

W, H = 240, 280          # 1.69" ST7789 panel
FPS = 16                 # the C3 draws ~18 fps, so every frame lands on time
SOURCE_FPS = 60          # frames pulled from the phone video before retiming
QUALITY = 70             # JPEG quality; higher = sharper but bigger and slower

# One entry per scene (clip between hard cuts); the last entry repeats.
# The video is 9:16 and the panel 6:7, so each frame is scaled to 240 wide and a
# 280-tall window is cut out of it. 0.0 keeps the top, 1.0 the bottom.
SCENE_CROP = [0.5]
SCENE_LENGTH = [None]  # seconds on the card; None = original length, longer = slowed down
# Per scene: "orbit" = ORBIT_TEXT flowing around you, "sticker" = white outline, None = nothing.
SCENE_EFFECT = ["orbit", None, None]
LABELS = ["TRAIN", "FIGHT", "BUILD"]

ORBIT_TEXT = "aura"      # the word that flows around you
# Agency FB (labels) and Segoe UI (orbit text) ship with Windows. On macOS or
# Linux point these at any .ttf you like; without them Pillow's built-in font is used.
ORBIT_FONT_FILE = pathlib.Path(r"C:\Windows\Fonts\segoeuib.ttf")
ORBIT_SIZE = 11          # px
ORBIT_SPEED = 7          # px the text travels around you per frame
ORBIT_SPACING = 14       # px of empty path between repeats of the word

FONT_FILE = pathlib.Path(r"C:\Windows\Fonts\AGENCYB.TTF")
YELLOW, GREY, WHITE = (255, 214, 10), (150, 150, 150), (255, 255, 255)
CHARS_PER_FRAME = 2      # label typing speed
GLITCH_FRAMES = 2        # RGB-split frames as each label appears


def font(size, file=FONT_FILE):
    try:
        return ImageFont.truetype(str(file), size)
    except OSError:
        return ImageFont.load_default(size)


BIG, SMALL = font(22), font(15)
ORBIT_FONT = font(ORBIT_SIZE, ORBIT_FONT_FILE)


def extract_frames(src, out_dir):
    subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(src),
         "-vf", f"fps={SOURCE_FPS},scale={W}:-2:flags=lanczos",
         str(out_dir / "%04d.png")],
        check=True)
    return sorted(out_dir.glob("*.png"))


def scene_ids(frames):
    """Label each frame with its scene number by spotting hard cuts."""
    # Blurred, so animated overlays like the speed lines don't look like cuts.
    thumbs = [f.convert("L").filter(ImageFilter.GaussianBlur(4)).resize((12, 14)) for f in frames]
    diffs = [ImageStat.Stat(ImageChops.difference(a, b)).mean[0]
             for a, b in zip(thumbs, thumbs[1:])]
    typical = sorted(diffs)[len(diffs) // 2] or 1
    ids, scene = [0], 0
    for d in diffs:
        if d > max(4 * typical, 20):
            scene += 1
        ids.append(scene)
    return ids


def retime(ids):
    """(source frame, scene, frame within scene) for every output frame."""
    bounds, start = [], 0
    for i in range(1, len(ids) + 1):
        if i == len(ids) or ids[i] != ids[start]:
            bounds.append((start, i))
            start = i
    picks = []
    for s, (a, b) in enumerate(bounds):
        seconds = SCENE_LENGTH[min(s, len(SCENE_LENGTH) - 1)] or (b - a) / SOURCE_FPS
        n = max(1, round(seconds * FPS))
        picks += [(a + k * (b - a) // n, s, k) for k in range(n)]
    return picks


def trace(blob):
    """Clockwise boundary of the topmost blob, starting mid-way along its top edge."""
    w, h = blob.size
    px = blob.load()
    box = blob.getbbox()
    if not box:
        return []
    top = [x for x in range(box[0], box[2]) if px[x, box[1]]]
    start = (top[len(top) // 2], box[1])
    ring = [(0, -1), (1, -1), (1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1)]  # clockwise from N
    path, (x, y), back = [start], start, 0  # back = direction of the last outside pixel
    for _ in range(8 * (w + h)):
        for i in range(1, 9):
            d = (back + i) % 8
            nx, ny = x + ring[d][0], y + ring[d][1]
            if 0 <= nx < w and 0 <= ny < h and px[nx, ny]:
                ox, oy = x + ring[(d - 1) % 8][0], y + ring[(d - 1) % 8][1]
                x, y = nx, ny
                back = ring.index((ox - x, oy - y))
                break
        else:
            break
        if (x, y) == start:
            break
        path.append((x, y))
    return path


def smooth_loop(path, r=4):
    n = len(path)
    return [(sum(path[(i + j) % n][0] for j in range(-r, r + 1)) / (2 * r + 1),
             sum(path[(i + j) % n][1] for j in range(-r, r + 1)) / (2 * r + 1)) for i in range(n)]


def resample_loop(path, step=1.0):
    """Points every `step` px along a closed path."""
    out, carry = [path[0]], 0.0
    for (x0, y0), (x1, y1) in zip(path, path[1:] + path[:1]):
        seg = math.hypot(x1 - x0, y1 - y0)
        t = step - carry
        while t <= seg:
            out.append((x0 + (x1 - x0) * t / seg, y0 + (y1 - y0) * t / seg))
            t += step
        carry = seg - (t - step)
    return out


def orbit(frame, mask, k):
    """Soft glow around you, with ORBIT_TEXT flowing clockwise along your outline."""
    solid = mask.point(lambda v: 255 if v >= 128 else 0)
    glow = solid.filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.GaussianBlur(4))
    out = frame.copy()
    out.paste(WHITE, mask=glow.point(lambda v: v * 6 // 10))
    out.paste(frame, mask=solid.filter(ImageFilter.GaussianBlur(0.6)))

    # A smoothed path about 8 px outside the body, so letters don't crowd into tight corners.
    blob = solid.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(4)).point(lambda v: 255 if v > 60 else 0)
    path = resample_loop(smooth_loop(trace(blob)))
    n = len(path)
    if n < 50:
        return out
    chars = list(ORBIT_TEXT) + [" "]
    adv = [ORBIT_FONT.getlength(c) for c in ORBIT_TEXT] + [ORBIT_SPACING]
    reps = max(1, round(n / sum(adv)))
    scale = n / (reps * sum(adv))  # whole repeats around the loop, so there's no seam

    layer = Image.new("RGBA", frame.size, (0, 0, 0, 0))
    c = ORBIT_SIZE  # half-size of the canvas each letter is drawn and rotated on
    s = k * ORBIT_SPEED
    for _ in range(reps):
        for ch, a in zip(chars, adv):
            mid, s = s + a * scale / 2, s + a * scale
            if ch == " ":
                continue
            i = int(mid) % n
            x, y = path[i]
            (x2, y2), (x1, y1) = path[(i + 3) % n], path[(i - 3) % n]
            g = Image.new("RGBA", (2 * c, 2 * c), (0, 0, 0, 0))
            ImageDraw.Draw(g).text((c, c), ch, font=ORBIT_FONT, fill=WHITE + (255,), anchor="mm")
            g = g.rotate(-math.degrees(math.atan2(y2 - y1, x2 - x1)), resample=Image.BICUBIC)
            layer.paste(g, (round(x) - c, round(y) - c), g)

    # Dark soft edge under the letters so they read on the light wall too.
    shadow = Image.new("RGBA", frame.size, (0, 0, 0, 0))
    shadow.putalpha(layer.getchannel("A").filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(1.2))
                    .point(lambda v: v * 7 // 10))
    out = Image.alpha_composite(out.convert("RGBA"), shadow)
    return Image.alpha_composite(out, layer).convert("RGB")


def sticker(frame, mask):
    """White sticker border around the person, with a soft dark halo so it reads on light walls."""
    solid = mask.point(lambda v: 255 if v >= 128 else 0)
    border = solid.filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.GaussianBlur(0.7))
    halo = solid.filter(ImageFilter.MaxFilter(11)).filter(ImageFilter.GaussianBlur(2.5))
    out = frame.copy()
    out.paste((0, 0, 0), mask=halo.point(lambda v: v * 6 // 10))
    out.paste(WHITE, mask=border)
    out.paste(frame, mask=solid.filter(ImageFilter.GaussianBlur(0.6)))
    return out


def crop(frame, scene):
    pos = SCENE_CROP[min(scene, len(SCENE_CROP) - 1)]
    top = round((frame.height - H) * pos)
    return frame.crop((0, top, W, top + H))


def label(frame, scene, k):
    """'01 // TRAIN' typing itself in at the top-left, then a blinking cursor."""
    parts = [(f"{scene + 1:02d}", SMALL, YELLOW), (" // ", SMALL, GREY),
             (LABELS[min(scene, len(LABELS) - 1)], BIG, WHITE)]
    budget = (k + 1) * CHARS_PER_FRAME
    typed = []
    for text, f, color in parts:
        typed.append((text[:max(0, budget)], f, color))
        budget -= len(text)
    typing = budget < 0

    layer = Image.new("RGBA", frame.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    top, bottom = BIG.getbbox("T")[1], BIG.getbbox("T")[3]
    x, y, h = 14, 14, bottom - top + 12
    base = y + h - 6
    width = 8 + sum(d.textlength(t, font=f) for t, f, _ in typed) + 12
    d.rectangle((x, y, x + width, y + h), fill=(0, 0, 0, 150))
    d.rectangle((x, y, x + 2, y + h), fill=YELLOW + (255,))
    pen = x + 8
    for text, f, color in typed:
        if text:
            d.text((pen, base), text, font=f, fill=color, anchor="ls")
            pen += d.textlength(text, font=f)
    if typing or (k // 4) % 2 == 0:  # solid while typing, then blinks every 0.25 s
        d.rectangle((pen + 2, base - (bottom - top), pen + 7, base), fill=WHITE + (255,))

    if k < GLITCH_FRAMES:  # red/cyan ghosts and a couple of torn rows as the label lands
        rnd = random.Random(scene * 31 + k)
        alpha = layer.getchannel("A")
        ghosts = Image.new("RGBA", frame.size, (0, 0, 0, 0))
        for color, dx in (((255, 40, 90), -3), ((40, 230, 255), 3)):
            ghost = Image.new("RGBA", frame.size, color + (0,))
            ghost.putalpha(ImageChops.offset(alpha, dx, 0).point(lambda v: v * 7 // 10))
            ghosts = Image.alpha_composite(ghosts, ghost)
        for _ in range(2):
            row = rnd.randrange(y, y + h - 3)
            band = layer.crop((0, row, W, row + 3))
            layer.paste((0, 0, 0, 0), (0, row, W, row + 3))
            layer.paste(band, (rnd.choice((-6, -4, 4, 6)), row))
        layer = Image.alpha_composite(ghosts, layer)
    return Image.alpha_composite(frame.convert("RGBA"), layer).convert("RGB")


def write_header(name, jpegs, path):
    offsets = [0]
    for j in jpegs:
        offsets.append(offsets[-1] + len(j))
    blob = b"".join(jpegs)
    lines = [
        f"// Generated by tools/make_video.py - do not edit by hand.",
        f"// {len(jpegs)} frames, {W}x{H} @ {FPS} fps, {len(blob) // 1024} KB",
        "#pragma once",
        '#include "video.h"',
        "",
        f"static const uint32_t {name}_offsets[] = {{",
    ]
    for i in range(0, len(offsets), 12):
        lines.append("  " + ", ".join(str(o) for o in offsets[i:i + 12]) + ",")
    lines += ["};", "", f"alignas(4) static const uint8_t {name}_data[] = {{"]
    for i in range(0, len(blob), 24):
        lines.append("  " + ",".join(str(b) for b in blob[i:i + 24]) + ",")
    lines += [
        "};", "",
        f"static const Video {name}_video = {{",
        f'  "{name}", {FPS}, {len(jpegs)}, {name}_offsets, {name}_data',
        "};", "",
    ]
    path.write_text("\n".join(lines), newline="\n")
    return len(blob)


def write_preview(frames, path, cols=10):
    thumbs = [f.resize((W // 2, H // 2)) for f in frames]
    rows = -(-len(thumbs) // cols)
    sheet = Image.new("RGB", (cols * W // 2, rows * H // 2))
    for i, t in enumerate(thumbs):
        sheet.paste(t, ((i % cols) * W // 2, (i // cols) * H // 2))
    sheet.save(path, quality=85)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src", type=pathlib.Path)
    ap.add_argument("name", help="C identifier, e.g. clean")
    ap.add_argument("--masks", help="mask folder under videos/masks (default: same as name)")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        paths = extract_frames(args.src, pathlib.Path(tmp))
        frames = [Image.open(p).convert("RGB") for p in paths]

    mask_dir = ROOT / "videos" / "masks" / (args.masks or args.name)
    masks = sorted(mask_dir.glob("*.png")) if mask_dir.is_dir() else []

    ids = scene_ids(frames)
    picks = retime(ids)
    out = []
    for src, scene, k in picks:
        f = frames[src]
        effect = SCENE_EFFECT[min(scene, len(SCENE_EFFECT) - 1)]
        if masks and effect:
            # mask clips can differ by a frame in length or a pixel in height, so map by position
            m = Image.open(masks[round(src * (len(masks) - 1) / (len(frames) - 1))]).convert("L")
            m = m.resize(f.size, Image.LANCZOS) if m.size != f.size else m
            f = orbit(f, m, k) if effect == "orbit" else sticker(f, m)
        out.append(label(crop(f, scene), scene, k))

    jpegs = []
    for f in out:
        buf = io.BytesIO()
        f.save(buf, "JPEG", quality=QUALITY, optimize=True, subsampling=2)
        jpegs.append(buf.getvalue())

    header = ROOT / "firmware" / "CyberID" / f"video_{args.name}.h"
    size = write_header(args.name, jpegs, header)
    preview = ROOT / "videos" / f"{args.name}_preview.jpg"
    write_preview(out, preview)

    per_scene = [sum(1 for _, s, _ in picks if s == i) for i in range(ids[-1] + 1)]
    print(f"{args.name}: {len(jpegs)} frames ({', '.join(f'{n / FPS:.2f}s' for n in per_scene)}), "
          f"effects {'/'.join(str(e) for e in SCENE_EFFECT) if masks else 'off (no masks)'}, {size // 1024} KB "
          f"(avg {size // len(jpegs) // 1024} KB/frame) -> {header.name}")


if __name__ == "__main__":
    main()
