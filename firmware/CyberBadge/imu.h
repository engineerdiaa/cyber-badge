// Cyber Badge by @diaawastaken · github.com/engineerdiaa/cyber-badge · MIT
//
// LSM6DS3 on the OLED's I2C bus. A task reads it, decides whether the badge has been shaken
// on purpose or is lying on a table, and splits each reading into gravity and shaking.
//
// INT1 is wired to GPIO 0 for later; polling is all this needs.
#pragma once
#include <stdint.h>
#include "dots.h"

namespace imu {

// The screen's axes as seen by the sensor: x right, y down, z into the screen.
struct Axes {
  float x[3], y[3], z[3];
};

// ---------- tuning ----------

// How the sensor sits on the board: chip towards the back of the badge, its y axis pointing
// at the top edge. If yours sits another way, the balls fall to the ceiling or run the wrong
// way when you tilt it, and this is the line to change. Keep it right-handed: y x z = x.
constexpr Axes MOUNT = {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}};

constexpr float SWING_G = 0.35f;      // video playing: a swing along the screen this hard (gravity taken out) counts
constexpr int   WAKE_SWINGS = 3;      // this many, each the other way: there, back and there again. A pick-up is two
constexpr int   SWING_MS = 400;       // each within this of the last, or the count starts again
constexpr float CALM_G = 0.03f;       // samples within this of each other are holding still
constexpr float CALM_DPS = 8.0f;
constexpr float STEADY_G = 0.002f;    // and two windows in a row this close: a slow turn passes the two above, down to 1.5 dps
constexpr float TABLE_DPS = 2.0f;     // lying on a table: the gyro keeps within this. In a hand it never got under 4
constexpr float TABLE_G = 0.03f;      // and the accelerometer within this
constexpr float TABLE_FLAT_G = 0.17f; // and gravity pulls along the screen by less than this: within 10 degrees of flat
constexpr int   TABLE_WINDOWS = 3;    // for this many windows in a row, so a moment's pause isn't a table
constexpr int   TABLE_IN_MS = 200;    // then the balls get the pull of standing upright, over this
constexpr int   TABLE_OUT_MS = 500;   // and let go of it over this once it moves
constexpr int   SAMPLE_MS = 5;        // balls showing
constexpr int   SLOW_SAMPLE_MS = 40;  // video playing: enough to feel a shake, and the decoder needs the CPU
constexpr int   WINDOW = 16;          // samples looked at together to decide it's holding still
constexpr float GRAVITY_SECONDS = 1.0f;  // how fast the idea of which way is down settles on the accelerometer

// Decides whether the badge has been shaken on purpose or lies on a table, and works out
// which part of the reading is gravity.
class Watch {
 public:
  // a in g, g in degrees/s, sensor axes, ms since the last sample.
  void feed(const float a[3], const float g[3], int ms);
  bool shaken() const { return swings >= WAKE_SWINGS; }  // shaken back and forth on purpose
  bool lying() const { return still >= TABLE_WINDOWS; }  // lying still on a table
  const float *gravity() const { return down; }          // what the accelerometer would read at rest
  const float *ballGravity() const { return shown; }     // the same, but as if upright while it lies on a table
  const float *gyroRest() const { return restG; }

 private:
  float restG[3] = {};
  float sumA[3] = {}, sumG[3] = {}, lastMeanA[3] = {};
  float lowA[3] = {}, highA[3] = {}, lowG[3] = {}, highG[3] = {};
  float down[3] = {}, shown[3] = {};
  float lastSwing[2] = {};
  float onTable = 0;  // how much of the upright pull the balls get, 0 to 1
  int filled = 0, swings = 0, sinceSwing = SWING_MS, still = 0;
  bool started = false, zeroKnown = false;
};

void toScreen(const float sensor[3], float screen[3]);

// ---------- the sensor itself ----------
bool begin();                  // call after Wire.begin(); false if there is no sensor
void setFast(bool fast);       // SAMPLE_MS or SLOW_SAMPLE_MS; slowing down also forgets any shake
bool working();                // false once it stops answering
bool shaken();                 // shaken on purpose since the last setFast()
void restart();                // forget readings that piled up while nobody was taking them
bool takeMotion(dots::Motion &m);  // false if there is nothing new
void gravityNow(dots::Motion &m);  // just which way is down at the latest reading, no shake or turn

}  // namespace imu
