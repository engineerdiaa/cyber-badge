---
name: add-video
description: Replace the Cyber ID card's placeholder clip with the user's own video, tune the crop and timing per scene, add labels and effects, and get it onto the card. Use when someone wants their own video, clip or face on the card's screen.
---

# Add your own video

Goal: turn the user's phone video into `firmware/CyberID/video_main.h`, check it looks right,
and flash it.

1. **Check the tools.** `python --version` (3.10+), `ffmpeg -version`, and
   `python -c "import PIL; print(PIL.__version__)"` (10.1+). Help install whatever is missing:
   `pip install -r tools/requirements.txt`, and ffmpeg from ffmpeg.org or a package manager.

2. **Get the video.** Ask the user to put it in `videos/`. A vertical 9:16 phone clip of about
   3 seconds works best; hard cuts split it into scenes, and each scene gets its own crop,
   label and effect. Probe it with `ffprobe` (size, duration, fps) and tell them what you found.

3. **First pass.** `python tools/make_video.py videos/<file> main`. Its last line reports frames,
   scene lengths, effects and size.

4. **Review and tune.** Read `videos/main_preview.jpg`. For each scene, check the subject is in
   frame (the panel is 6:7, so the 9:16 frame is cropped):
   - `SCENE_CROP`: 0.0 keeps the top of the frame, 1.0 the bottom. One value per scene.
   - `SCENE_LENGTH`: seconds per scene; `None` keeps the original. Longer slows it down.
   Edit the values at the top of `tools/make_video.py`, re-run, and look again. Tell the user
   what you changed and why. To compare options, crop one frame several ways side by side.

5. **Labels.** `LABELS` holds one per scene. Short uppercase words read best (`TRAIN`, `BUILD`).

6. **Effects (optional).** Per scene in `SCENE_EFFECT`: `"orbit"` (`ORBIT_TEXT` flowing around
   the person), `"sticker"` (white outline) or `None`. Both need cut-out masks:
   - `pip install "rembg[cpu]"`, then `python tools/make_masks.py videos/<file> main`.
     On Windows, if importing rembg fails deep inside numba with a "No such file or directory"
     error, set `NUMBA_DISABLE_JIT=1` and retry (Windows path-length limit).
   - Re-run `make_video.py` and check the effect on the preview at full size, not just the
     thumbnails. If a scene loses part of the person (dark background, motion blur), redo just
     that scene: `--scene N --model birefnet-general-lite` (slower, sharper). If a head or limb
     is still cut off, switch the effect off for that scene. Close-ups where the person fills
     the frame look better with `None`.

7. **Size.** A 3 s clip is about 0.4–0.6 MB and the app partition is 3 MB, so 3–4 clips fit.
   For more clips: `make_video.py videos/<file> <name>`, then `#include "video_<name>.h"` and
   add `&<name>_video` to `VIDEOS[]` in the sketch. The BOOT button cycles through them.

8. **Build and flash** with the `flash-card` skill. Then read the serial stats: about 16 fps with
   "slowest frame" under ~70 ms. If it's slower, lower `QUALITY` or simplify the effects.

Their video is personal: the `.mp4` in `videos/` isn't gitignored, so mention it before they
commit or push.
