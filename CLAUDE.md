# Cyber ID Card: notes for Claude Code

An ID card built on an ESP32-C3 Super Mini: a 1.69" ST7789 TFT loops a short video from flash,
a 0.96" SSD1306 OLED shows a name tag. Most people who open this repo want to put in their own
name and video, then build and flash it. The skills in `.claude/skills/` walk through those
(`add-video`, `personalize`, `flash-card`). `README.md` is the build guide; point people to the
right section of it instead of repeating it.

## Layout

- `firmware/CyberID/CyberID.ino`: the sketch. Placeholders are marked `TODO` (`EMAIL`, `NAME`,
  `TITLE`, the video include).
- `firmware/CyberID/video_<name>.h`: generated clips. Never edit by hand; regenerate with
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
- `arduino-cli compile --fqbn <FQBN> firmware/CyberID`, then
  `arduino-cli upload -p <port> --fqbn <FQBN> firmware/CyberID`.
- Serial: `arduino-cli monitor -p <port> -c baudrate=115200,dtr=off,rts=off`. Keep DTR/RTS off
  so opening the port doesn't reset the chip. Every 5 s it prints
  `[main] 16.0 fps, slowest frame 61 ms (DMA)`, and `OLED not found` once at boot if the OLED
  isn't detected.
- Ask before uploading to someone's board, and remind them to switch the card off before
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
  plugged into a PC with no serial monitor open.
- **Frame rate:** the C3 decodes about 17–18 fps at 240×280, so clips are encoded at 16 fps and
  every frame shows. Detailed frames (text, fine patterns) decode slower; watch "slowest frame"
  (should stay under ~70 ms).
- **The C3's pin labels are on the back of the board**, so they're mirrored when it's mounted
  chip side up.
- **Pins:** TFT SCL 5, SDA 6, RES 7, DC 10, CS 20, BLK 21; OLED SDA 3, SCL 4; GPIO 9 is the BOOT
  button (switches clips). If pins change, update the sketch, the README wiring tables and the
  schematic together.
- **OLED colour is physical:** two-colour modules have a yellow top strip; single-colour ones
  show everything in one colour. Code can't change it.

## Style

- Match the existing code: plain C++, short comments that say why.
- Keep README.md accurate when behaviour, settings or pins change.
- People's own videos and photos are personal. `videos/masks/` and previews are gitignored, but
  `.mp4` files in `videos/` aren't: mention that before they commit their own clip.
