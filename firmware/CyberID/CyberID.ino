// Cyber ID card - ESP32-C3 Super Mini
//
// TFT  (1.69" ST7789, 240x280): loops a video stored in flash as JPEG frames,
//      sent over SPI with DMA while the next block decodes.
// OLED (0.96" SSD1306, 128x64): your email or handle scrolls along the top
//      strip (yellow on two-colour modules), name + title sit below it.
// BOOT button: switches video. The choice survives power-off.
//
// Arduino IDE settings (Tools menu):
//   Board:            Nologo ESP32C3 Super Mini
//   Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)   <- needed, videos are big
//   USB CDC On Boot:  Enabled
//
// Libraries: Adafruit GFX, Adafruit SSD1306, Adafruit ST7735 and ST7789, JPEGDEC,
//            bb_spi_lcd
//
// Make it yours: search this file for TODO, and see "Make it yours" in the README
// for swapping in your own video.

#include <SPI.h>
#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <bb_spi_lcd.h>  // after Adafruit_GFX, so it reuses Adafruit's font types
#include <JPEGDEC.h>

#include "video_main.h"  // TODO: made from your own clip by tools/make_video.py

// ---------- your details ----------
const char *EMAIL = "you@example.com";  // TODO: email or @handle, scrolls along the top strip
const char *NAME  = "NAME";             // TODO: your name; about 6 characters fit at this size
const char *TITLE = "your title";       // TODO: one short line under the name, ~14 characters

// ---------- pins ----------
// TFT labels say SCL/SDA but it is SPI, not I2C: SCL = clock, SDA = data (MOSI).
// Same order as the TFT header, so the wires never cross. GPIO 8 (LED) and
// 9 (BOOT) are skipped.
constexpr int TFT_SCL = 5;
constexpr int TFT_SDA = 6;
constexpr int TFT_RES = 7;
constexpr int TFT_DC  = 10;
constexpr int TFT_CS  = 20;  // -1 if CS measures ~0 ohm to GND on the TFT (see the README)
constexpr int TFT_BLK = 21;

constexpr int OLED_SDA = 3;
constexpr int OLED_SCL = 4;

constexpr int BUTTON = 9;  // the BOOT button already on the Super Mini

// ---------- tuning ----------
constexpr bool     TFT_USE_DMA = true;     // false = older driver without DMA (~14 fps), see the README
constexpr uint32_t TFT_SPI_HZ = 40000000;  // plenty: with DMA the transfer hides behind decoding
constexpr uint8_t  BACKLIGHT = 255;        // 0-255, 255 = full brightness; lower = longer battery life
constexpr uint8_t  TFT_ROTATION = 2;       // 0 or 2: switch if the picture is upside down
constexpr uint32_t OLED_I2C_HZ = 400000;
constexpr uint32_t MARQUEE_STEP_MS = 40;   // ms between scroll steps
constexpr int      MARQUEE_STEP_PX = 2;    // px per step: same speed as 1 px / 20 ms, half the redraws
constexpr int      MARQUEE_GAP = 48;       // blank px between repeats of the email

// Add more clips here; BOOT cycles through them and the first one plays by default.
const Video *VIDEOS[] = {&main_video};
constexpr uint8_t VIDEO_COUNT = sizeof(VIDEOS) / sizeof(VIDEOS[0]);

// The TFT normally runs on bb_spi_lcd with DMA. Adafruit_ST7789 (no DMA) is the
// fallback: bb_spi_lcd drives this panel in SPI mode 0 only, and without a CS
// line the ST7789 needs mode 3.
constexpr bool TFT_DMA_ON = TFT_USE_DMA && TFT_CS >= 0;
BB_SPI_LCD lcd;
Adafruit_ST7789 tft(&SPI, TFT_CS, TFT_DC, TFT_RES);
// Same clock during and after transfers so the I2C bus is never reconfigured.
Adafruit_SSD1306 oled(128, 64, &Wire, -1, OLED_I2C_HZ, OLED_I2C_HZ);
JPEGDEC jpeg;
Preferences prefs;

uint8_t oledAddr = 0;
int oledChunk = 64;  // data bytes per I2C transfer; 256 if the Wire buffer could be enlarged
uint8_t current = 0;
uint32_t videoStart = 0;
uint32_t lastFrame = UINT32_MAX;

// ---------- TFT video ----------

int drawBlock(JPEGDRAW *d) {
  if (TFT_DMA_ON) {
    lcd.setAddrWindow(d->x, d->y, d->iWidth, d->iHeight);  // waits for the previous block to finish sending
    lcd.pushPixels(d->pPixels, d->iWidth * d->iHeight, DRAW_TO_LCD | DRAW_WITH_DMA);  // returns immediately
  } else {
    tft.setAddrWindow(d->x, d->y, d->iWidth, d->iHeight);
    tft.writePixels(d->pPixels, d->iWidth * d->iHeight, true, true);
  }
  return 1;
}

void drawFrame(const Video *v, uint32_t i) {
  const uint8_t *jpg = v->data + v->offsets[i];
  int len = v->offsets[i + 1] - v->offsets[i];
  // The previous frame's last block may still be going out of JPEGDEC's
  // buffer, and openRAM() clears that buffer.
  if (TFT_DMA_ON) lcd.waitDMA();
  if (!jpeg.openRAM((uint8_t *)jpg, len, drawBlock)) return;
  jpeg.setPixelType(RGB565_BIG_ENDIAN);  // panel byte order, so no swapping per pixel
  if (TFT_DMA_ON) {
    jpeg.decode(0, 0, JPEG_USES_DMA);  // alternate between two halves of the pixel buffer
  } else {
    tft.startWrite();
    jpeg.decode(0, 0, 0);
    tft.endWrite();
  }
  jpeg.close();
}

void switchVideo() {
  current = (current + 1) % VIDEO_COUNT;
  prefs.putUChar("clip", current);
  videoStart = millis();
  lastFrame = UINT32_MAX;
}

void pollButton() {
  static bool wasDown = false;
  static uint32_t changedAt = 0;
  bool down = digitalRead(BUTTON) == LOW;
  if (down != wasDown && millis() - changedAt > 30) {
    changedAt = millis();
    wasDown = down;
    if (down) switchVideo();
  }
}

// ---------- OLED ----------

uint8_t findOled() {
  for (uint8_t addr : {0x3C, 0x3D}) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) return addr;
  }
  return 0;
}

void printCentered(const char *s, int baseline) {
  int16_t x1, y1;
  uint16_t w, h;
  oled.getTextBounds(s, 0, baseline, &x1, &y1, &w, &h);
  oled.setCursor((oled.width() - w) / 2 - x1, baseline);
  oled.print(s);
}

// Blue area (rows 16-63) is drawn once; only the yellow strip changes after.
void drawNameCard() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setFont(&FreeSansBold18pt7b);
  printCentered(NAME, 43);
  oled.setFont(&FreeSans9pt7b);
  printCentered(TITLE, 59);
  oled.display();
}

// Sends just pages 0-1 (the 16px yellow strip): 256 bytes instead of 1 KB.
void pushTopStrip() {
  Wire.beginTransmission(oledAddr);
  Wire.write(0x00);  // command stream
  Wire.write(SSD1306_PAGEADDR);
  Wire.write(0);
  Wire.write(1);
  Wire.write(SSD1306_COLUMNADDR);
  Wire.write(0);
  Wire.write(127);
  Wire.endTransmission();

  const uint8_t *buf = oled.getBuffer();
  for (int i = 0; i < 256; i += oledChunk) {
    Wire.beginTransmission(oledAddr);
    Wire.write(0x40);  // data stream
    Wire.write(buf + i, min(oledChunk, 256 - i));
    Wire.endTransmission();
  }
}

// Runs as its own task: while it waits on I2C the CPU keeps decoding video.
void marqueeTask(void *) {
  oled.setFont(nullptr);
  oled.setTextSize(2);  // 12x16 px characters, exactly the height of the yellow strip
  oled.setTextWrap(false);
  const int period = strlen(EMAIL) * 12 + MARQUEE_GAP;
  int offset = 0;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    oled.fillRect(0, 0, oled.width(), 16, SSD1306_BLACK);
    for (int x = -offset; x < oled.width(); x += period) {
      oled.setCursor(x, 0);
      oled.print(EMAIL);
    }
    pushTopStrip();
    offset = (offset + MARQUEE_STEP_PX) % period;
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(MARQUEE_STEP_MS));
  }
}

// ---------- main ----------

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Plugged into a PC with nothing reading the port, each print would wait up
  // to 2 s for buffer space and freeze the video. Drop the text instead.
  Serial.setTxTimeoutMs(0);
#endif
  pinMode(BUTTON, INPUT_PULLUP);

  ledcAttach(TFT_BLK, 5000, 8);
  ledcWrite(TFT_BLK, 0);  // keep backlight off until the panel is cleared

  if (TFT_DMA_ON) {
    // spilcdInit() rather than lcd.begin(): begin() swaps a MISO of -1 for the
    // board default, which on the Super Mini is GPIO 5, our SPI clock.
    spilcdInit(lcd.getLCDStruct(), LCD_ST7789_280, FLAGS_NONE, TFT_SPI_HZ,
               TFT_CS, TFT_DC, TFT_RES, -1, -1, TFT_SDA, TFT_SCL, 1);
    // bb_spi_lcd's 180 is the MADCTL value Adafruit uses for rotation 0, so
    // TFT_ROTATION means the same thing with either driver.
    lcd.setRotation(TFT_ROTATION == 0 ? 180 : 0);
    lcd.fillScreen(0, DRAW_TO_LCD);
  } else {
    SPI.begin(TFT_SCL, -1, TFT_SDA, -1);
    // Without a CS line the ST7789 only stays in sync in SPI mode 3.
    tft.init(240, 280, TFT_CS >= 0 ? SPI_MODE0 : SPI_MODE3);
    tft.setRotation(TFT_ROTATION);
    tft.setSPISpeed(TFT_SPI_HZ);
    tft.fillScreen(ST77XX_BLACK);
  }
  ledcWrite(TFT_BLK, BACKLIGHT);

  // Room for the whole 256-byte strip plus the control byte, so each scroll
  // step is one I2C transfer instead of four. Must be set before begin().
  if (Wire.setBufferSize(260) >= 257) oledChunk = 256;
  Wire.begin(OLED_SDA, OLED_SCL, OLED_I2C_HZ);
  oledAddr = findOled();
  if (oledAddr && oled.begin(SSD1306_SWITCHCAPVCC, oledAddr, true, false)) {
    drawNameCard();
    xTaskCreate(marqueeTask, "marquee", 4096, nullptr, 2, nullptr);
  } else {
    Serial.println("OLED not found - check SDA/SCL wiring");
  }

  prefs.begin("cyberid");
  current = prefs.getUChar("clip", 0) % VIDEO_COUNT;  // last clip picked with BOOT
  videoStart = millis();
}

void loop() {
  pollButton();

  // Pick the frame by wall-clock time, so if decoding falls behind we skip
  // frames instead of playing in slow motion.
  const Video *v = VIDEOS[current];
  uint32_t frame = (uint64_t)(millis() - videoStart) * v->fps / 1000 % v->frameCount;
  if (frame == lastFrame) {
    delay(1);
    return;
  }
  drawFrame(v, frame);
  lastFrame = frame;

  static uint32_t drawn = 0, statsAt = 0, lastDrawAt = 0, slowest = 0;
  uint32_t now = millis();
  if (lastDrawAt) slowest = max(slowest, now - lastDrawAt);  // longest time the picture sat still
  lastDrawAt = now;
  drawn++;
  if (now - statsAt >= 5000) {
    if (Serial) Serial.printf("[%s] %.1f fps, slowest frame %lu ms (%s)\n", v->name, drawn * 1000.0f / (now - statsAt),
                              (unsigned long)slowest, TFT_DMA_ON ? "DMA" : "no DMA");
    drawn = 0;
    slowest = 0;
    statsAt = millis();
  }
}
