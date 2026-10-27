---
name: personalize
description: Put the user's name, title and email or handle on the Cyber Badge's OLED, and change the scene labels and the word that flows around them in the video. Use when someone wants to change the text on the badge.
---

# Personalize the badge

1. **OLED text** is at the top of `firmware/CyberBadge/CyberBadge.ino` (marked `TODO`):
   - `NAME`: drawn in FreeSansBold18pt across 128 px, so about 6 characters fit. If theirs is
     longer, suggest a short form, or switch that line to `FreeSansBold12pt7b` (add the
     `#include <Fonts/FreeSansBold12pt7b.h>` and use it in `drawNameCard()`).
   - `TITLE`: FreeSans9pt, about 14 characters.
   - `EMAIL`: an email or @handle. It scrolls, so any length works.
   To check a fit before flashing, measure with Adafruit GFX's `getTextBounds()` logic: the
   text must be at most 128 px wide.

2. **Video text** lives in `tools/make_video.py` and needs a re-run of
   `python tools/make_video.py videos/<file> main` (see the `add-video` skill):
   - `LABELS`: one per scene, shown as `01 // LABEL`.
   - `ORBIT_TEXT`: the word that flows around them. `ORBIT_SPACING` sets the gap between
     repeats.

3. **Build and flash** with the `flash-badge` skill.

If they're publishing their own fork, remind them that the docs are CC BY 4.0, so the credit
line in the README stays.
