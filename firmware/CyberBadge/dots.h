// Cyber Badge by @diaawastaken · github.com/engineerdiaa/cyber-badge · MIT
//
// Dots: a pool of small balls on the TFT that flows like water and feels the real motion of
// the board, coloured in layers from its surface down to the floor. Physics and drawing
// only: the sensor lives in imu.h, the mode switching in CyberBadge.ino.
#pragma once
#include <stdint.h>

namespace dots {

// ---------- tuning ----------
constexpr int   COUNT = 300;              // balls; the serial stats show how hard the C3 is working
constexpr int   RADIUS_PX = 3;            // sprites are 2r+1 px wide, keep it at 3 or below
constexpr int   CORNER_PX = 24;           // the panel's rounded corners, so balls never hide behind them
constexpr int   STEPS_PER_SECOND = 240;   // physics rate; much below 200 the liquid shakes itself apart
constexpr float GRAVITY_PX = 3500.0f;     // px/s^2 at 1 g: a fall of about 0.4 s, slow enough to watch
constexpr float SHAKE_PX = 20000.0f;      // px/s^2 per g of real shaking: a shake of 1 cm throws the pool up the screen
constexpr float SHAKE_KNEE_G = 0.15f;     // moves well below this count the same as gravity, well above it they count as shaking
constexpr float MAX_SHAKE_PX = 12000.0f;  // the hardest shakes level off here: top speed comes in 0.1 s anyway, more only crushes the pool
constexpr float TURN_KNEE = 3.0f;         // rad/s: turning well below this leaves the liquid to gravity alone
constexpr float REACH_PX = 12.0f;         // how far a ball feels its neighbours; the cost goes with its square
constexpr float REST_CROWD = 0.5f;        // how crowded a ball is happy to be: lower spreads the pool
constexpr float PRESSURE = 3.0f;          // how hard crowded balls push, px per step; above about 4 the pool fizzes
constexpr float NEAR_PRESSURE = 2.0f;     // how hard very close balls push, so they stay apart
constexpr float CLING = 0.3f;             // surface tension: how much of a sparse ball's pull is kept
constexpr float VISCOSITY = 0.02f;        // how much of their speed difference neighbours share per step
constexpr float WATER_DRAG = 0.8f;        // speed lost per second, as a fraction: sets how long a slosh lasts
constexpr float SLOW_DRAG = 8.0f;         // more of it for a ball slower than SLOW_PX, so the last ripples stop
constexpr float SLOW_PX = 30.0f;          // px/s
constexpr float MOVING_PX = 40.0f;        // px/s: slower than this a ball counts as still. A steady hand stays under it
constexpr int   SHADES = 16;              // steps in a gradient, from the top of the pool down to the floor
constexpr float LAYER_G = 0.2f;           // g along the screen the layers need to turn; lying flat, they keep their last way up
constexpr bool  MIXING = true;            // colours travel with the balls, so a shake stirs the layers. false: they never mix
constexpr float SETTLE_S = 0.5f;          // mixed colours settle back into layers, most of the way in 3x this, once balls are still
constexpr float BLAST_PX = 1000.0f;       // px/s: fire comes in as a fireball that explodes this hard, out to every wall
constexpr float POUR_S = 1.0f;            // ocean comes in held up along the top this long, pouring off both ends
constexpr float BURST_PX = 60.0f;         // px/s: jungle comes in as a balloon that pops this hard, breaking into drops as it falls

constexpr int WIDTH = 240, HEIGHT = 280;

// How the board is moving, in screen axes: x right, y down, z into the screen.
struct Motion {
  float gravityX, gravityY;  // g: what the accelerometer reads at rest (+1 on an axis pointing straight up)
  float shakeX, shakeY;      // g: the rest of its reading
  float wx, wy, wz;          // gyro reading in rad/s, right-hand rule
  float dt;                  // seconds this covers, to get angular acceleration
};

using PushFn = void (*)(int x, int y, int w, int h, uint16_t *pixels);  // panel byte order
using IdleFn = void (*)();

void begin(int palette = 0);         // lays the balls on the floor and builds the sprites in that palette
void setMotion(const Motion &m);     // latest board motion, used by every step until the next call
void step();                         // one physics step of 1/STEPS_PER_SECOND seconds
int movingBalls();                   // how many balls are visibly moving
void redrawAll();                    // the next draw() repaints the whole screen
bool draw(PushFn push, IdleFn idle); // sends what changed; idle() runs while each row goes out by DMA
void dropIn(int palette);            // a fresh pool in palette 0 to paletteCount() - 1: fire explodes, ocean pours down the sides, jungle's balloon pops
int paletteCount();
const char *paletteName(int palette);

// Read-only state, for tests. All of a ball in one place: on this chip a field of a
// struct is one load, an element of a global array is three instructions, and the
// solver does little else.
struct alignas(32) Ball {
  int32_t x, y;        // Q16 px
  int32_t vx, vy;      // Q16 px per step
  int32_t oldX, oldY;  // where it was before this step
  int32_t crowd, closeCrowd;  // how crowded it is within REACH_PX, then the pressures from that
};
extern Ball balls[COUNT];

}  // namespace dots
