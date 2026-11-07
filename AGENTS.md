# Cyber Badge: notes for Claude and Codex

A badge built on an ESP32-C3 Super Mini: a 1.69" ST7789 TFT loops a short video from flash,
a 0.96" SSD1306 OLED shows a name tag, and an optional LSM6DS3 motion sensor turns a shake into
a pool of balls on the TFT that flows like water, in layers of colour. Most people who open
this repo want to put in their own name and video, then build and flash it. The skills in
`.claude/skills/` walk through those (`add-video`, `personalize`, `flash-badge`). `README.md` is
the build guide; point people to the right section of it instead of repeating it.

## Shared context (Claude and Codex)

Claude Code and Codex use this file as the shared playbook. CLAUDE.md only imports it.
- **Project brain:** `C:\Users\dia2a\Documents\Brain\Projects\Cyber ID card\`. For substantial work, read `00 Overview.md` and `03 Status.md`; check `02 Decisions.md` before changing how something works.
- **Skills:** Claude's originals are in `.claude/skills/`; Codex equivalents already exist in `.agents/skills/` (`add-video`, `personalize`, `flash-badge`). Keep the original skill paths above accurate.
- **Handoffs:** write a new, uniquely named note in the brain's `Sessions/` folder. During concurrent work, use Sessions notes instead of both editing shared status or decisions. Routine completion excerpts also use the global append-only automatic-memory setup.
- **Concurrent work:** assign separate files or worktrees. Do not edit a file the other assistant is editing.
- **Claude's memory:** `C:\Users\dia2a\.claude\projects\C--Users-dia2a-Desktop-Cyber-ID-card\memory\` is read-only for Codex. Never edit Claude's settings, hooks, skills or memory.
- **Git:** do not commit or push unless Diaa explicitly asks. The local CLAUDE.md import depends on AGENTS.md; review both together before any future commit. Diaa approved keeping AGENTS.md and CLAUDE.md in Git and ignoring only the root CLAUDE.md.bak backup. Keep both instruction files together in any future commit.

## Layout

- `firmware/CyberBadge/CyberBadge.ino`: the sketch: video, OLED, BOOT, and switching to the balls
  and back. Placeholders are marked `TODO` (`EMAIL`, `NAME`, `TITLE`, the video include).
- `firmware/CyberBadge/dots.cpp`, `dots.h`: the balls. Tuning is at the top of `dots.h`; the
  colours are `PALETTES` in `dots.cpp` (four stops from the surface to the floor, and a glint).
- `firmware/CyberBadge/imu.cpp`, `imu.h`: the sensor task: the wake-up shake, lying on a table,
  and splitting each reading into gravity and shaking. `MOUNT` says how the sensor sits.
- `firmware/CyberBadge/video_<name>.h`: generated clips. Never edit by hand; regenerate with
  `tools/make_video.py`.
- `tools/make_video.py`: any video → header. Tall, wide or square clips all work (frames are
  scaled to cover 240×280 and cropped). Per-scene settings at the top (`SCENE_CROP`,
  `SCENE_LENGTH`, `SCENE_EFFECT`, `LABELS`, `ORBIT_*`). Writes `videos/<name>_preview.jpg`, a
  contact sheet: read it to check crops and effects. The three TRAIN/FIGHT/BUILD scenes are only
  the example; people can use any clip, any number of scenes, labels or none.
- `tools/make_masks.py`: rembg cut-outs (`videos/masks/<name>/`) for the `"orbit"` and
  `"sticker"` effects. Only needed for effects.
- `videos/placeholder.mp4`: source of the placeholder clip.

## Build and flash

- FQBN: `esp32:esp32:nologo_esp32c3_super_mini:PartitionScheme=huge_app,CDCOnBoot=cdc`
  (esp32 core 3.x).
- Libraries: Adafruit GFX, Adafruit SSD1306, Adafruit ST7735 and ST7789, JPEGDEC, bb_spi_lcd.
- `arduino-cli compile --fqbn <FQBN> firmware/CyberBadge`, then
  `arduino-cli upload -p <port> --fqbn <FQBN> firmware/CyberBadge`.
- Serial: `arduino-cli monitor -p <port> -c baudrate=115200,dtr=off,rts=off`. Keep DTR/RTS off
  so opening the port doesn't reset the chip. Every 5 s it prints
  `[main] 16.0 fps, slowest frame 61 ms (DMA)`, and `OLED not found` once at boot if the OLED
  isn't detected. While the balls show, it's
  `[dots] 43.6 fps, physics 49% of the CPU, running at 100% of real time` instead (300 balls:
  28–58 fps, 36–56% of the CPU, and it should stay at 99–100% of real time), plus
  `[dots] <colours>` on each BOOT press. Without a sensor: `Sensor not found - video only` once
  at boot.
- Ask before uploading to someone's board, and remind them to switch the badge off before
  plugging USB into the C3 when the battery is connected (USB 5V and the battery meet at the
  5V pin).

## Things that bite

- **Partition scheme must be Huge APP.** The default leaves 1.2 MB for the app; one 3 s clip is
  about 0.5 MB.
- **Don't call `lcd.begin()`.** bb_spi_lcd's `begin()` swaps a MISO of -1 for the board default,
  which on the Super Mini is GPIO 5, the TFT clock. The sketch calls `spilcdInit()` directly.
- **`JPEGDEC::openRAM()` clears its pixel buffer**, which DMA may still be reading from. Keep the
  `lcd.waitDMA()` before opening the next frame.
- **`Serial.setTxTimeoutMs(0)`** stops prints from freezing the video for ~2 s when USB is
  plugged into a PC with no serial monitor open. It also drops lines the PC doesn't take in
  time, so a missing or repeated-looking `[dots] <colours>` line isn't a button bug.
- **Frame rate:** the C3 decodes about 17–18 fps at 240×280, so clips are encoded at 16 fps and
  every frame shows. Detailed frames (text, fine patterns) decode slower; watch "slowest frame"
  (should stay under ~70 ms).
- **The C3's pin labels are on the back of the board**, so they're mirrored when it's mounted
  chip side up.
- **Pins:** TFT SCL 5, SDA 6, RES 7, DC 10, CS 20, BLK 21; OLED SDA 3, SCL 4, shared with the
  motion sensor (0x6A or 0x6B), whose INT1 is wired to GPIO 0 but unused (the firmware polls);
  GPIO 9 is the BOOT button: it switches clips while the video plays, and drops in a fresh pool
  in the next colours while the balls show. If pins change, update the sketch, the README wiring
  tables and the schematic together.
- **The sensor's mount is in code.** `MOUNT` in `imu.h` is chip to the back of the badge, y axis
  to the top edge. Balls that fall to the top or run the wrong way when tilted mean the sensor
  sits another way.
- **Tests (Diaa's machine):** a host-side rig for `dots` and `imu`, with planted-bug runs, is in
  the gitignored `private/dots-tests` (see its README). Rerun it after changing either.
- **OLED colour is physical:** two-colour modules have a yellow top strip; single-colour ones
  show everything in one colour. Code can't change it.

## Style

- Match the existing code: plain C++, short comments that say why.
- Keep README.md accurate when behaviour, settings or pins change.
- People's own videos and photos are personal. `videos/masks/` and previews are gitignored, but
  `.mp4` files in `videos/` aren't: mention that before they commit their own clip.
