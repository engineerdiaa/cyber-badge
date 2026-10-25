---
name: flash-card
description: Compile the Cyber ID card firmware, upload it to the ESP32-C3 Super Mini, and check the frame rate over serial. Use when someone wants to build, flash, upload or test the card.
---

# Build and flash

FQBN: `esp32:esp32:nologo_esp32c3_super_mini:PartitionScheme=huge_app,CDCOnBoot=cdc`

1. **Find arduino-cli.** Try `arduino-cli version`. Arduino IDE 2 bundles one under
   `resources/app/lib/backend/resources/` in its install folder. If there's none, install it, or
   have the user upload from the Arduino IDE with the settings in the README's
   "Programming the C3" section.

2. **One-time setup**, if `arduino-cli core list` doesn't show `esp32:esp32`:
   ```
   arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core update-index
   arduino-cli core install esp32:esp32
   arduino-cli lib install "Adafruit GFX Library" "Adafruit SSD1306" "Adafruit ST7735 and ST7789 Library" JPEGDEC
   ```
   bb_spi_lcd is installed along with JPEGDEC.

3. **Compile:** `arduino-cli compile --fqbn <FQBN> firmware/CyberID`. Report the size line. If it
   says the sketch is too big, the partition scheme isn't Huge APP or there are too many clips.

4. **Find the port:** `arduino-cli board list`. The C3 shows up as a USB serial port from
   Espressif (VID 303A).

5. **Upload.** Ask the user first, and if the battery is connected, have them switch the card
   off before plugging in USB. Then
   `arduino-cli upload -p <port> --fqbn <FQBN> firmware/CyberID`.
   If the port doesn't show or the upload fails: hold BOOT, tap RESET, release BOOT, retry.

6. **Check it.** `arduino-cli monitor -p <port> -c baudrate=115200,dtr=off,rts=off` for about
   15 seconds (DTR/RTS off so it doesn't reset the chip). Expect
   `[main] 16.0 fps, slowest frame 60–70 ms (DMA)`. `OLED not found` means the OLED's SDA/SCL
   (pins 3 and 4) or power. For anything else, use the README's troubleshooting table.
