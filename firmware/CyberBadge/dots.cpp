// Cyber Badge by @diaawastaken · github.com/engineerdiaa/cyber-badge · MIT
//
// The model: a shallow pool in a thin flat bottle the shape of the screen, each ball a drop
// of it. Everything is worked out in the bottle's own frame, so the sensor feeds straight in:
//   - gravity and shaking are the opposite of what the accelerometer reads
//   - the gyro gives the Coriolis, centrifugal and Euler forces of a turning bottle
//
// The liquid is double density relaxation (Clavet et al., 2005). Each ball adds up how crowded
// it is from its neighbours within REACH_PX. Crowded balls push their neighbours away, sparse
// ones pull a little, and that pull is surface tension. A second, sharper sum keeps balls from
// sitting on top of each other.
//
// Hard balls can't do this. They pack into a crystal that holds its shape until it avalanches,
// so a slow tilt comes in lurches, and a slosh dies in the first row that hits a wall. Here the
// push only depends on how crowded a ball is, so layers slide over each other and a shove
// travels through the pool as a wave, up the far wall and back.
//
// Each step every ball moves freely, is then pushed, and its velocity is whatever movement
// resulted (position based, Mueller et al.). The pressures must be from this step: a step old,
// the pool never settles.
//
// Fixed point throughout: the C3 has no FPU. Positions are Q16 px, velocities Q16 px per step,
// accelerations Q16 px per step^2. Everything two balls do to each other is equal and opposite
// to the last bit, so momentum is exactly conserved.
#pragma GCC optimize("O2")  // Arduino builds with -Os, and this is the hottest code on the chip

#include "dots.h"
#include <math.h>
#include <string.h>

#ifdef ARDUINO
#include <esp_attr.h>
#define HOT IRAM_ATTR  // out of flash: no cache misses in the solver
#else
#define HOT
#endif

namespace dots {

constexpr int32_t ONE = 1 << 16;
constexpr int32_t R = RADIUS_PX * ONE;
constexpr int32_t MIN_X = R, MAX_X = (WIDTH << 16) - R;
constexpr int32_t MIN_Y = R, MAX_Y = (HEIGHT << 16) - R;
constexpr int32_t CENTER_X = (WIDTH << 16) / 2, CENTER_Y = (HEIGHT << 16) / 2;
constexpr int32_t ARC = CORNER_PX * ONE;                    // corner arc centres, inset from the edges
constexpr int32_t ARC_REACH_Q10 = (CORNER_PX - RADIUS_PX) << 10;  // how far a ball's centre gets from one

constexpr int32_t MAX_SPEED = 23 * ONE / 4;  // px per step: about 1400 px/s, past anything worth seeing
constexpr int32_t PULL_LIMIT = (int32_t)((MAX_SHAKE_PX + GRAVITY_PX) / (STEPS_PER_SECOND * STEPS_PER_SECOND) * 65536.0f);

constexpr int32_t WATER_Q16 = (int32_t)(WATER_DRAG / STEPS_PER_SECOND * 65536.0f + 0.5f);
constexpr int32_t SLOW_SPEED = (int32_t)(SLOW_PX / STEPS_PER_SECOND * 65536.0f);
constexpr int32_t SLOW_SLOPE = (int32_t)(SLOW_DRAG / STEPS_PER_SECOND * 65536.0f * 4096.0f / SLOW_SPEED);
constexpr int32_t MOVING_SPEED = (int32_t)(MOVING_PX / STEPS_PER_SECOND * 65536.0f);

// Neighbours. The table holds SCALE / distance: one multiply then gives both the distance as a
// Q15 fraction of REACH_PX and the normal, with no square root. A unit normal reads UNIT.
constexpr int32_t REACH = (int32_t)(REACH_PX * ONE);
constexpr int32_t REACH_Q11 = REACH >> 5;
constexpr uint32_t REACH2_Q22 = (uint32_t)REACH_Q11 * (uint32_t)REACH_Q11;
constexpr int64_t SCALE = (1LL << 41) / REACH_Q11;
constexpr int32_t UNIT = (int32_t)(SCALE >> 12);
constexpr int TABLE_SHIFT = 21;
constexpr int TABLE_SIZE = (REACH2_Q22 >> TABLE_SHIFT) + 2;
constexpr uint32_t TABLE_START = 1u << TABLE_SHIFT;  // closer than 0.7 px the table runs out

// The normal is UNIT long, not a round number, so the pressures are scaled up by MOVE_SCALE to
// make up for it: pressure * normal then reads as if the normal were Q15.
constexpr float MOVE_SCALE = 32768.0f / UNIT;
constexpr int32_t REST_Q15 = (int32_t)(REST_CROWD * 32768.0f);
constexpr int32_t PRESSURE_Q4 = (int32_t)(PRESSURE * MOVE_SCALE * 16.0f + 0.5f);
constexpr int32_t NEAR_Q4 = (int32_t)(NEAR_PRESSURE * MOVE_SCALE * 16.0f + 0.5f);
constexpr int32_t CLING_Q8 = (int32_t)(CLING * 256.0f + 0.5f);
constexpr int32_t VISCOSITY_Q16 = (int32_t)(VISCOSITY * 65536.0f + 0.5f);
constexpr int32_t MOST_PRESSURE = 1 << 16;  // keeps the sums in range; only a pool crushed against a wall gets here

// Water doesn't compress, and this pool can't be allowed to: squeezed past a point, the pushes
// overshoot and it bursts. Past EASY_PRESSURE a ball takes on less and less of the pull, none at
// all FADE_PRESSURE further on. The most any pull can do is hold the pool there, however hard
// and however deep, while the same pull throws a pool that has room to move at full strength.
constexpr int32_t EASY_PRESSURE = 6000;     // a pool at rest in a corner, the deepest it gets, reaches 5600
constexpr int FADE_SHIFT = 11;
constexpr int32_t FADE_PRESSURE = 1 << FADE_SHIFT;
constexpr int32_t MOST_PUSH = 3 << 11;      // Q12, about 1 px: any more and a squeezed ball jumps clean past its neighbours

// Finding who is near whom costs as much as a whole step, so the list of near pairs is kept
// until it could have gone stale. SKIN is how much further apart a pair can be and still be
// listed; the grid's cells are at least as wide as that.
constexpr int32_t SKIN = 2 * ONE;
constexpr int32_t LISTED = REACH + SKIN;
constexpr uint32_t LISTED2_Q22 = (uint32_t)(LISTED >> 5) * (uint32_t)(LISTED >> 5);
constexpr int CELL_SHIFT = 20;
static_assert((1 << CELL_SHIFT) >= LISTED, "a cell must hold everything within reach");
constexpr int GRID_W = (WIDTH + 15) >> 4, GRID_H = (HEIGHT + 15) >> 4;
constexpr uint16_t NONE = 0xFFFF;

constexpr int MAX_PAIRS = COUNT * 12;  // a pool squeezed by a shake needs 7.5; past this, far pairs just go unfelt

struct Pair {
  uint16_t a, b;
};

struct Close {  // a pair within reach this step
  uint16_t a, b;
  int16_t nx, ny;  // from a towards b, UNIT long
  uint16_t weight;  // Q15: 1 touching, 0 at REACH_PX
};

Ball balls[COUNT];
static uint16_t cellHead[GRID_W * GRID_H], cellNext[COUNT];
static uint16_t inverseTable[TABLE_SIZE];
static int32_t builtX[COUNT], builtY[COUNT];  // where each ball was when the list was made
static Pair pairs[MAX_PAIRS];
static int pairCount;
static bool pairsStale = true;
static Close closeBy[MAX_PAIRS];
static int closeCount;

// This frame's forces, set by setMotion().
static int32_t forceX, forceY;       // Q16 px per step^2
static bool turning;
static int32_t turnXX, turnXY, turnYX, turnYY;  // Q24 per step^2: centrifugal and Euler
static int32_t coriolis;             // Q16 radians per step
static uint32_t stepsSinceColour;
static float upX = 0, upY = -1;      // which way the colour layers stack
static uint32_t noise = 2463534242u; // so no two fresh pools come in quite the same way

// The ocean's ledge, while ledgeSteps counts down: a low roof that holds the water up along
// the top, all but a gap at each end. It slopes down both ways from a ridge through the middle
// of the screen, so the water runs off both ends. Heights and places across are Q8 px from the
// middle of the screen.
static int ledgeSteps;
static int32_t ledgeUpX, ledgeUpY;          // Q14: which way was up when it went in
static int32_t ledgeHeight;                 // of the ridge
static int32_t ledgeFrom, ledgeTo;          // where across it ends, short of the screen's edges
static int32_t ledgeSlope;                  // Q14: px down per px out from the ridge
static int32_t ledgeOutX[2], ledgeOutY[2];  // Q14: which way a ball is put back on it, each side

static inline int32_t absolute(int32_t v) { return v < 0 ? -v : v; }

// Within -3% and +7%, without a square root.
static inline int32_t HOT roughLength(int32_t x, int32_t y) {
  x = absolute(x);
  y = absolute(y);
  return x > y ? x + (y >> 2) + (y >> 3) : y + (x >> 2) + (x >> 3);
}

static uint32_t isqrt(uint32_t v) {
  uint32_t root = 0, bit = 1u << 30;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= root + bit) {
      v -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

// SCALE / distance for a Q22 squared distance, interpolated.
static inline uint32_t HOT inverseDistance(uint32_t d2) {
  uint32_t i = d2 >> TABLE_SHIFT;
  int32_t a = inverseTable[i], b = inverseTable[i + 1];
  return a + (((b - a) * (int32_t)((d2 >> (TABLE_SHIFT - 8)) & 255)) >> 8);
}

static void HOT pushOutOfCorner(Ball &ball, int32_t ax, int32_t ay) {
  ax >>= 6;
  ay >>= 6;
  uint32_t d2 = ax * ax + ay * ay;  // Q20
  if (d2 <= (uint32_t)ARC_REACH_Q10 * (uint32_t)ARC_REACH_Q10) return;
  int32_t d = isqrt(d2);
  int32_t nx = (ax << 15) / d, ny = (ay << 15) / d;
  int32_t c = d - ARC_REACH_Q10;  // Q10
  ball.x -= (c * nx) >> 9;
  ball.y -= (c * ny) >> 9;
}

// Walls just stop a ball: what it had along the wall carries on, which is what sends water up
// the far side.
static void HOT keepInside() {
  for (int i = 0; i < COUNT; i++) {
    Ball &ball = balls[i];
    int32_t x = ball.x, y = ball.y;
    if ((uint32_t)x - MIN_X > (uint32_t)(MAX_X - MIN_X)) x = ball.x = x < MIN_X ? MIN_X : MAX_X;
    if ((uint32_t)y - MIN_Y > (uint32_t)(MAX_Y - MIN_Y)) y = ball.y = y < MIN_Y ? MIN_Y : MAX_Y;
    if ((uint32_t)x - ARC > (uint32_t)((WIDTH << 16) - 2 * ARC) &&
        (uint32_t)y - ARC > (uint32_t)((HEIGHT << 16) - 2 * ARC)) {
      pushOutOfCorner(ball, x < ARC ? x - ARC : x - ((WIDTH << 16) - ARC), y < ARC ? y - ARC : y - ((HEIGHT << 16) - ARC));
    }
  }
}

static inline void HOT addPair(int a, int b) {
  int32_t dx = balls[b].x - balls[a].x;
  if ((uint32_t)dx + LISTED >= 2 * LISTED) return;  // two cells away is too far to square in 32 bits
  int32_t dy = balls[b].y - balls[a].y;
  if ((uint32_t)dy + LISTED >= 2 * LISTED) return;
  int32_t sx = dx >> 5, sy = dy >> 5;  // Q11
  if ((uint32_t)(sx * sx) + (uint32_t)(sy * sy) >= LISTED2_Q22 || pairCount == MAX_PAIRS) return;
  pairs[pairCount].a = a;
  pairs[pairCount].b = b;
  pairCount++;
}

static void HOT findPairs() {
  memset(cellHead, 0xFF, sizeof(cellHead));
  for (int i = 0; i < COUNT; i++) {
    int cell = (balls[i].y >> CELL_SHIFT) * GRID_W + (balls[i].x >> CELL_SHIFT);
    cellNext[i] = cellHead[cell];
    cellHead[cell] = i;
    builtX[i] = balls[i].x;
    builtY[i] = balls[i].y;
  }

  pairCount = 0;
  for (int row = 0; row < GRID_H; row++) {
    for (int col = 0; col < GRID_W; col++) {
      int cell = row * GRID_W + col;
      for (int a = cellHead[cell]; a != NONE; a = cellNext[a]) {
        for (int b = cellNext[a]; b != NONE; b = cellNext[b]) addPair(a, b);
        if (col + 1 < GRID_W) {
          for (int b = cellHead[cell + 1]; b != NONE; b = cellNext[b]) addPair(a, b);
        }
        if (row + 1 < GRID_H) {
          int below = cell + GRID_W;
          if (col > 0) {
            for (int b = cellHead[below - 1]; b != NONE; b = cellNext[b]) addPair(a, b);
          }
          for (int b = cellHead[below]; b != NONE; b = cellNext[b]) addPair(a, b);
          if (col + 1 < GRID_W) {
            for (int b = cellHead[below + 1]; b != NONE; b = cellNext[b]) addPair(a, b);
          }
        }
      }
    }
  }
  pairsStale = false;
}

// A pair left off the list was at least LISTED apart, so it can't come within reach until the
// two have closed by SKIN. Only movement relative to each other counts: if every ball has
// drifted less than SKIN / 2 from how ball 0 drifted, no two can have closed by more than SKIN.
// That way the whole pool falling together doesn't cost a new list every step.
static bool HOT pairsStillGood() {
  int32_t driftX = balls[0].x - builtX[0], driftY = balls[0].y - builtY[0];
  for (int i = 1; i < COUNT; i++) {
    int32_t dx = balls[i].x - builtX[i] - driftX, dy = balls[i].y - builtY[i] - driftY;
    if (absolute(dx) + absolute(dy) > SKIN / 2) return false;
  }
  return true;
}

// Half of Coriolis: to first order it turns the velocity by the board's rotation in half a
// step, and half goes each side of the kick. With both halves, a ball touching nothing keeps a
// perfectly straight line in the room however the board spins. Three shears make the rotation
// area-preserving, so it can't add energy.
static inline void HOT turnVelocity(int32_t &vx, int32_t &vy) {
  vx += (coriolis * (vy >> 4) + (1 << 12)) >> 13;
  vy -= (coriolis * (vx >> 4) + (1 << 11)) >> 12;
  vx += (coriolis * (vy >> 4) + (1 << 12)) >> 13;
}

// Balls nearly on top of each other are the ones that most need a push in the right direction.
static void HOT veryCloseNormal(Close &c, int32_t sx, int32_t sy, uint32_t s2) {
  int32_t r = isqrt(s2);  // Q11
  c.nx = r ? sx * UNIT / r : UNIT;  // dead on top of each other: any direction will do
  c.ny = r ? sy * UNIT / r : 0;
}

// Adds up how crowded each ball is, and keeps the pairs within reach for pushApart().
static void HOT feelNeighbours() {
  closeCount = 0;
  for (int k = 0; k < pairCount; k++) {
    int ia = pairs[k].a, ib = pairs[k].b;
    Ball &a = balls[ia], &b = balls[ib];
    int32_t dx = b.x - a.x;
    if ((uint32_t)dx + REACH >= 2 * REACH) continue;
    int32_t dy = b.y - a.y;
    if ((uint32_t)dy + REACH >= 2 * REACH) continue;
    int32_t sx = dx >> 5, sy = dy >> 5;  // Q11
    uint32_t s2 = (uint32_t)(sx * sx) + (uint32_t)(sy * sy);
    if (s2 >= REACH2_Q22) continue;

    uint32_t inverse = inverseDistance(s2);
    int32_t far = (int32_t)(((s2 >> 10) * inverse) >> 16);  // Q15 of REACH_PX
    int32_t weight = far < 32768 ? 32768 - far : 0;
    int32_t weight2 = (weight * weight) >> 15;
    int32_t weight3 = (weight2 * weight) >> 15;
    a.crowd += weight2;
    b.crowd += weight2;
    a.closeCrowd += weight3;
    b.closeCrowd += weight3;

    Close &c = closeBy[closeCount++];
    c.a = ia;
    c.b = ib;
    if (s2 >= TABLE_START) {
      c.nx = (sx * (int32_t)inverse) >> 12;
      c.ny = (sy * (int32_t)inverse) >> 12;
    } else {
      veryCloseNormal(c, sx, sy, s2);
    }
    c.weight = weight;
  }
}

// Turns each ball's two sums into its two pressures, in place.
static void HOT findPressures() {
  for (int i = 0; i < COUNT; i++) {
    Ball &ball = balls[i];
    int32_t over = ball.crowd - REST_Q15;
    if (over < 0) over = (over * CLING_Q8) >> 8;
    int32_t pressure = (over * PRESSURE_Q4) >> 8;
    int32_t closeness = (ball.closeCrowd * NEAR_Q4) >> 8;
    ball.crowd = pressure < MOST_PRESSURE ? pressure : MOST_PRESSURE;
    ball.closeCrowd = closeness < MOST_PRESSURE ? closeness : MOST_PRESSURE;
  }
}

// Pressure moves the two apart along the line between them. Viscosity moves them towards the
// same velocity, which takes the fizz out of the pool: it works on neighbours, so it stops
// jitter at once and leaves a wave as wide as the screen hardly touched.
static void HOT pushApart() {
  for (int k = 0; k < closeCount; k++) {
    const Close &c = closeBy[k];
    Ball &a = balls[c.a], &b = balls[c.b];
    int32_t weight = c.weight >> 3;  // Q12
    int32_t weight2 = (weight * weight) >> 12;
    // Q11 pressure * Q12 weight >> 11 is Q12, and each ball takes a quarter of the sum.
    int32_t push = ((a.crowd + b.crowd) * weight + (a.closeCrowd + b.closeCrowd) * weight2) >> 13;
    if (push > MOST_PUSH) push = MOST_PUSH;
    int32_t mx = (push * c.nx) >> 11, my = (push * c.ny) >> 11;
    int32_t share = (c.weight * VISCOSITY_Q16) >> 15;
    int32_t ux = (((b.vx - a.vx) >> 4) * share) >> 12, uy = (((b.vy - a.vy) >> 4) * share) >> 12;
    a.x += ux - mx;
    a.y += uy - my;
    b.x += mx - ux;
    b.y += my - uy;
  }
}

static inline int32_t HOT ledgeUp(int32_t x, int32_t y) {
  return (((x - CENTER_X) >> 8) * ledgeUpX + ((y - CENTER_Y) >> 8) * ledgeUpY) >> 14;
}

static inline int32_t HOT ledgeAcross(int32_t x, int32_t y) {
  return (((y - CENTER_Y) >> 8) * ledgeUpX - ((x - CENTER_X) >> 8) * ledgeUpY) >> 14;
}

static inline int32_t HOT ledgeAt(int32_t across) {  // its height there
  return ledgeHeight - ((absolute(across) * ledgeSlope) >> 14);
}

// A ball that started the step on the ledge or above it can't go through: it is put back on
// it square to the slope, so the water slides down it as down a floor and runs off the ends.
// A ball that has fallen past an end, or was already below, is free.
static void HOT holdUpLake() {
  constexpr int32_t SLACK = 64;  // Q8: a quarter pixel, for rounding in the last step's put-back
  for (int i = 0; i < COUNT; i++) {
    Ball &ball = balls[i];
    int32_t s = ledgeAcross(ball.x, ball.y);
    if (s < ledgeFrom || s > ledgeTo) continue;
    int32_t under = ledgeAt(s) - ledgeUp(ball.x, ball.y);
    if (under <= 0) continue;
    if (ledgeUp(ball.oldX, ball.oldY) - ledgeAt(ledgeAcross(ball.oldX, ball.oldY)) < -SLACK) continue;
    int side = s > 0;  // Q8 px times Q14 is Q22, a shift of 6 from Q16
    ball.x += (under * ledgeOutX[side]) >> 6;
    ball.y += (under * ledgeOutY[side]) >> 6;
  }
}

void HOT step() {
  // Forces, then a free move to where each ball wants to go.
  for (int i = 0; i < COUNT; i++) {
    Ball &ball = balls[i];
    int32_t ax = forceX, ay = forceY;
    int32_t vx = ball.vx, vy = ball.vy;
    if (turning) {
      int32_t rx = (ball.x - CENTER_X) >> 10, ry = (ball.y - CENTER_Y) >> 10;  // Q6 px
      ax += (turnXX * rx + turnXY * ry + (1 << 13)) >> 14;
      ay += (turnYX * rx + turnYY * ry + (1 << 13)) >> 14;
      ax = ax > PULL_LIMIT ? PULL_LIMIT : ax < -PULL_LIMIT ? -PULL_LIMIT : ax;
      ay = ay > PULL_LIMIT ? PULL_LIMIT : ay < -PULL_LIMIT ? -PULL_LIMIT : ay;
      turnVelocity(vx, vy);
    }
    int32_t squeezed = ball.crowd - EASY_PRESSURE;  // still last step's pressure here
    if (squeezed > 0) {
      int32_t left = squeezed < FADE_PRESSURE ? FADE_PRESSURE - squeezed : 0;
      ax = (ax * left) >> FADE_SHIFT;
      ay = (ay * left) >> FADE_SHIFT;
    }
    vx += ax;
    vy += ay;
    if (turning) turnVelocity(vx, vy);
    int32_t speed = absolute(vx) + absolute(vy);
    int32_t drag = WATER_Q16;
    if (speed < SLOW_SPEED) drag += ((SLOW_SPEED - speed) * SLOW_SLOPE) >> 12;
    vx -= (vx * drag + (1 << 15)) >> 16;
    vy -= (vy * drag + (1 << 15)) >> 16;
    ball.vx = vx;
    ball.vy = vy;
    ball.oldX = ball.x;
    ball.oldY = ball.y;
    ball.x += vx;
    ball.y += vy;
    ball.crowd = ball.closeCrowd = 0;
  }
  keepInside();
  if (ledgeSteps) holdUpLake();

  if (pairsStale || !pairsStillGood()) findPairs();
  feelNeighbours();
  findPressures();
  pushApart();
  keepInside();
  if (ledgeSteps) {
    holdUpLake();
    ledgeSteps--;
  }

  // Velocities come from what actually happened.
  for (int i = 0; i < COUNT; i++) {
    Ball &ball = balls[i];
    int32_t vx = ball.x - ball.oldX, vy = ball.y - ball.oldY;
    if (absolute(vx) + absolute(vy) > MAX_SPEED) {
      int32_t length = roughLength(vx, vy);
      if (length > MAX_SPEED) {
        int32_t keep = (MAX_SPEED << 6) / (length >> 6);
        vx = (int32_t)(((int64_t)vx * keep) >> 12);
        vy = (int32_t)(((int64_t)vy * keep) >> 12);
      }
    }
    ball.vx = vx;
    ball.vy = vy;
  }
  stepsSinceColour++;
}

// Past the gyro's full scale on both axes at once, and the step's sums would overflow.
static int32_t turnLimit(float v) {
  constexpr float MOST = 119000.0f;
  return (int32_t)(v > MOST ? MOST : v < -MOST ? -MOST : v);
}

void setMotion(const Motion &m) {
  constexpr float H = 1.0f / STEPS_PER_SECOND;
  constexpr float PER_STEP = H * H * 65536.0f;  // px/s^2 to Q16 px per step^2

  // In the bottle's frame, gravity and whatever the bottle is doing appear as the opposite of
  // what the accelerometer reads. Gentle moves count the same as gravity, so the swing of the
  // hand in a slow tilt stays as small next to the tilt as it really is. Only a real shake gets
  // the extra strength that makes it worth doing, and that levels off.
  float shaken2 = m.shakeX * m.shakeX + m.shakeY * m.shakeY;
  float gain = GRAVITY_PX + (SHAKE_PX - GRAVITY_PX) * shaken2 / (shaken2 + SHAKE_KNEE_G * SHAKE_KNEE_G);
  float keep = gain / sqrtf(1 + shaken2 * gain * gain / (MAX_SHAKE_PX * MAX_SHAKE_PX));
  forceX = (int32_t)lroundf(-(m.gravityX * GRAVITY_PX + m.shakeX * keep) * PER_STEP);
  forceY = (int32_t)lroundf(-(m.gravityY * GRAVITY_PX + m.shakeY * keep) * PER_STEP);

  // The layers stack against gravity. Lying flat there's too little of it to go by, so they
  // keep the way up they had.
  float along2 = m.gravityX * m.gravityX + m.gravityY * m.gravityY;
  if (along2 > LAYER_G * LAYER_G) {
    float n = 1.0f / sqrtf(along2);
    upX = m.gravityX * n;
    upY = m.gravityY * n;
  }

  // Turning: the same idea. The bottle doesn't know where the wrist is, and a slow turn is
  // nearly all tilt, which gravity has already taken care of.
  static float lastWz = 0;
  float turned2 = m.wx * m.wx + m.wy * m.wy + m.wz * m.wz;
  float share = turned2 / (turned2 + TURN_KNEE * TURN_KNEE);
  float alpha = m.dt > 0 ? (m.wz - lastWz) / m.dt : 0;
  lastWz = m.wz;
  turning = share > 0.001f;
  if (turning) {
    const float q24 = H * H * 16777216.0f * share;
    float xy = m.wx * m.wy;
    coriolis = (int32_t)lroundf(m.wz * share * H * 65536.0f);
    float z2 = m.wz * m.wz;
    turnXX = turnLimit((m.wy * m.wy + z2) * q24);
    turnYY = turnLimit((m.wx * m.wx + z2) * q24);
    turnXY = turnLimit((alpha - xy) * q24);
    turnYX = turnLimit((-alpha - xy) * q24);
  }
}

int movingBalls() {
  int moving = 0;
  for (int i = 0; i < COUNT; i++) {
    if (absolute(balls[i].vx) + absolute(balls[i].vy) > MOVING_SPEED) moving++;
  }
  return moving;
}

// ---------- drawing ----------
//
// The panel keeps its picture, so only 8x8 tiles a ball has left, entered or changed
// colour in are resent. Each row of tiles is built in a small buffer and goes out by
// DMA while the next one is built. There is no frame buffer: it would take 134 KB of RAM.

constexpr int TILE = 8;
constexpr int TILE_COLS = WIDTH / TILE, TILE_ROWS = HEIGHT / TILE;
constexpr int SPRITE = 2 * RADIUS_PX + 1;
constexpr int MERGE_GAP = 3;  // a new address window costs about as much as sending this many tiles

static int16_t shownX[COUNT], shownY[COUNT];  // quarter px
static uint8_t shade[COUNT], shownShade[COUNT];
static uint32_t dirty[TILE_ROWS];
static uint16_t sprites[SHADES][16][SPRITE * SPRITE];  // four sub-pixel offsets per axis
static uint8_t spriteKeys[16][SPRITE * SPRITE];        // how bright each pixel is, whatever the colour
static uint16_t rowBalls[COUNT * 2], rowStart[TILE_ROWS + 1];
static uint16_t rowBuffer[2][WIDTH * TILE] __attribute__((aligned(4)));
static uint8_t rowKeys[WIDTH * TILE];
static uint8_t activeBuffer;

// ---------- colours ----------
//
// A ball's colour comes from its place in its own column of the pool, along gravity: how
// much of the column lies above it against how much lies below. The top of every column
// gets the first colour and the floor the last, so a wave keeps its bright crest, a thrown
// pool keeps its layers in the air, and a pool in a corner is darkest in the corner.
// Columns are strips STRIP_PX wide across gravity. Each ball counts in the two nearest, in
// proportion, so its colour doesn't jump as it drifts from one to the next.
//
// With MIXING, the colour is the ball's own: it travels with it, so a shake stirs the
// layers, and it settles back to the ball's place once the ball is still.

constexpr int STRIP_PX = 8;
constexpr int HALF_DIAGONAL_PX = 185;  // no ball gets further than this from the middle of the screen
constexpr int STRIPS = 2 * HALF_DIAGONAL_PX / STRIP_PX + 2;

static uint16_t order[COUNT];          // balls from the top down, nearly sorted already from the last frame
static int32_t heightOf[COUNT];        // Q8 px along the way up
static uint16_t stripOf[COUNT];        // Q8 strips across it
static int32_t aboveOf[COUNT];         // Q8 balls above it in its column
static int32_t columnMass[STRIPS + 1];
static uint16_t carried[COUNT];        // Q15: 0 is the top of the pool, 32768 the floor

// fresh: no settling and no holding a shade, for a pool that has just been laid out.
static void findColours(bool fresh) {
  int32_t ux = (int32_t)lroundf(upX * 16384), uy = (int32_t)lroundf(upY * 16384);
  for (int i = 0; i < COUNT; i++) {
    int32_t px = (balls[i].x - CENTER_X) >> 8, py = (balls[i].y - CENTER_Y) >> 8;  // Q8 px
    heightOf[i] = (px * ux + py * uy) >> 14;
    stripOf[i] = (uint16_t)((((px * uy - py * ux) >> 14) + (HALF_DIAGONAL_PX << 8)) / STRIP_PX);
  }
  for (int k = 1; k < COUNT; k++) {
    uint16_t b = order[k];
    int32_t h = heightOf[b];
    int j = k;
    for (; j > 0 && heightOf[order[j - 1]] < h; j--) order[j] = order[j - 1];
    order[j] = b;
  }

  // Top down, then bottom up: how much of each ball's column is above it, then below it.
  memset(columnMass, 0, sizeof(columnMass));
  for (int k = 0; k < COUNT; k++) {
    int i = order[k], s = stripOf[i] >> 8, f = stripOf[i] & 255;
    aboveOf[i] = ((256 - f) * columnMass[s] + f * columnMass[s + 1]) >> 8;
    columnMass[s] += 256 - f;
    columnMass[s + 1] += f;
  }
  int32_t settle = 32768;
  if (MIXING && !fresh) {
    float share = stepsSinceColour / (SETTLE_S * STEPS_PER_SECOND);
    settle = share < 1 ? (int32_t)(share * 32768) : 32768;
  }
  stepsSinceColour = 0;
  memset(columnMass, 0, sizeof(columnMass));
  for (int k = COUNT - 1; k >= 0; k--) {
    int i = order[k], s = stripOf[i] >> 8, f = stripOf[i] & 255;
    int32_t below = ((256 - f) * columnMass[s] + f * columnMass[s + 1]) >> 8;
    columnMass[s] += 256 - f;
    columnMass[s + 1] += f;
    // A ball takes the colour at its middle: half of it counts above, half below.
    int32_t place = (int32_t)(((uint32_t)(aboveOf[i] + 128) << 15) / (uint32_t)(aboveOf[i] + below + 256));

    // Still balls settle at the full rate. Past four times the speed of a still one, the
    // colour just travels with the ball.
    int32_t rate = settle;
    int32_t speed = absolute(balls[i].vx) + absolute(balls[i].vy);
    if (MIXING && !fresh && speed > MOVING_SPEED) {
      rate = speed >= 4 * MOVING_SPEED ? 0 : (int32_t)((int64_t)rate * (4 * MOVING_SPEED - speed) / (3 * MOVING_SPEED));
    }
    carried[i] = (uint16_t)(carried[i] + (((place - carried[i]) * rate) >> 15));

    // Held until it is three quarters of the way to the next shade, so a ball sitting
    // between two doesn't flicker.
    int32_t want = carried[i] * (SHADES - 1);  // Q15 shades
    if (fresh || absolute(want - (shade[i] << 15)) > 24576) shade[i] = (uint8_t)((want + 16384) >> 15);
  }
}

static void markDirty(int qx, int qy) {
  int x = (qx >> 2) - RADIUS_PX, y = (qy >> 2) - RADIUS_PX;
  int x0 = x / TILE, x1 = (x + SPRITE - 1) / TILE, y0 = y / TILE, y1 = (y + SPRITE - 1) / TILE;
  if (x1 >= TILE_COLS) x1 = TILE_COLS - 1;
  if (y1 >= TILE_ROWS) y1 = TILE_ROWS - 1;
  uint32_t bits = ((2u << x1) - 1) & ~((1u << x0) - 1);
  for (int r = y0; r <= y1; r++) dirty[r] |= bits;
}

void redrawAll() {
  for (int r = 0; r < TILE_ROWS; r++) dirty[r] = (1u << TILE_COLS) - 1;
}

static void HOT blit(uint16_t *buffer, int runX, int runW, int rowY, int ball) {
  int qx = shownX[ball], qy = shownY[ball], offset = (qy & 3) * 4 + (qx & 3);
  const uint16_t *sprite = sprites[shownShade[ball]][offset];
  const uint8_t *keys = spriteKeys[offset];
  int sx = (qx >> 2) - RADIUS_PX, sy = (qy >> 2) - RADIUS_PX;
  int y0 = sy > rowY ? sy : rowY, y1 = sy + SPRITE < rowY + TILE ? sy + SPRITE : rowY + TILE;
  int x0 = sx > runX ? sx : runX, x1 = sx + SPRITE < runX + runW ? sx + SPRITE : runX + runW;
  for (int y = y0; y < y1; y++) {
    int from = (y - sy) * SPRITE + (x0 - sx), to = (y - rowY) * runW + (x0 - runX);
    for (int x = x0; x < x1; x++, from++, to++) {
      // Edges of touching balls overlap: keep the brighter.
      if (keys[from] > rowKeys[to]) {
        rowKeys[to] = keys[from];
        buffer[to] = sprite[from];
      }
    }
  }
}

bool draw(PushFn push, IdleFn idle) {
  findColours(false);
  for (int i = 0; i < COUNT; i++) {
    // Half a sub-pixel of slack, so a ball sitting on a boundary can't flicker
    // between two offsets.
    int32_t dx = balls[i].x - (shownX[i] << 14), dy = balls[i].y - (shownY[i] << 14);
    bool moved = absolute(dx) > 13000 || absolute(dy) > 13000;
    if (moved || shade[i] != shownShade[i]) {
      markDirty(shownX[i], shownY[i]);
      if (moved) {
        shownX[i] = (balls[i].x + (1 << 13)) >> 14;
        shownY[i] = (balls[i].y + (1 << 13)) >> 14;
        markDirty(shownX[i], shownY[i]);
      }
      shownShade[i] = shade[i];
    }
  }

  bool any = false;
  for (int r = 0; r < TILE_ROWS; r++) any |= dirty[r] != 0;
  if (!any) return false;

  // Which balls touch which row of tiles.
  memset(rowStart, 0, sizeof(rowStart));
  for (int i = 0; i < COUNT; i++) {
    int y = (shownY[i] >> 2) - RADIUS_PX;
    int r0 = y / TILE, r1 = (y + SPRITE - 1) / TILE;
    if (r1 >= TILE_ROWS) r1 = TILE_ROWS - 1;
    for (int r = r0; r <= r1; r++) rowStart[r]++;
  }
  for (int r = 0, sum = 0; r <= TILE_ROWS; r++) {
    sum += rowStart[r];
    rowStart[r] = sum;  // end of each row for now; the filling below walks it back to the start
  }
  for (int i = 0; i < COUNT; i++) {
    int y = (shownY[i] >> 2) - RADIUS_PX;
    int r0 = y / TILE, r1 = (y + SPRITE - 1) / TILE;
    if (r1 >= TILE_ROWS) r1 = TILE_ROWS - 1;
    for (int r = r0; r <= r1; r++) rowBalls[--rowStart[r]] = i;
  }

  for (int r = 0; r < TILE_ROWS; r++) {
    uint32_t bits = dirty[r];
    dirty[r] = 0;
    while (bits) {
      // One address window over this dirty tile, the ones after it and small gaps.
      int first = __builtin_ctz(bits), last = first;
      for (int c = first + 1; c < TILE_COLS && c - last <= MERGE_GAP + 1; c++) {
        if (bits >> c & 1) last = c;
      }
      bits &= ~((2u << last) - 1);

      int runX = first * TILE, runW = (last - first + 1) * TILE, rowY = r * TILE;
      uint16_t *buffer = rowBuffer[activeBuffer];
      activeBuffer ^= 1;
      memset(buffer, 0, runW * TILE * sizeof(uint16_t));
      memset(rowKeys, 0, runW * TILE);
      for (int k = rowStart[r]; k < rowStart[r + 1]; k++) blit(buffer, runX, runW, rowY, rowBalls[k]);
      push(runX, rowY, runW, TILE, buffer);
      if (idle) idle();
    }
  }
  return true;
}

// ---------- palettes ----------
//
// Colours at full light, from the top of the pool down to the floor, spread evenly over the
// shades. A channel past 255 saturates most of the ball, which is what makes the green neon.
// The far side of a ball keeps DARK_SIDE of its colour, and the glint goes on top.
//
// Each palette's fresh pool comes in its own way, whether a shake brings up the balls or BOOT
// brings in the next colours.

constexpr int STOPS = 4;

enum Entrance : uint8_t {
  BLAST,    // a fireball in the middle explodes against every wall
  POUR,     // a lake along the top pours off both ends and down the sides
  BALLOON,  // water in a balloon up high pops and falls
};

struct Palette {
  const char *name;
  int16_t lit[STOPS][3];  // RGB
  int16_t glint[3];
  Entrance entrance;
};
static const Palette PALETTES[] = {
    {"fire", {{290, 225, 70}, {285, 135, 15}, {225, 45, 5}, {105, 5, 4}}, {110, 100, 70}, BLAST},       // gold to dark red
    {"ocean", {{120, 240, 290}, {25, 150, 275}, {6, 60, 200}, {3, 10, 72}}, {110, 115, 100}, POUR},     // aqua to deep navy
    {"jungle", {{65, 275, 24}, {14, 212, 70}, {5, 140, 78}, {2, 58, 40}}, {150, 120, 130}, BALLOON},   // the neon green to deep teal
};
constexpr int PALETTE_COUNT = sizeof(PALETTES) / sizeof(PALETTES[0]);
constexpr float DARK_SIDE = 0.7f;

int paletteCount() { return PALETTE_COUNT; }
const char *paletteName(int palette) { return PALETTES[palette % PALETTE_COUNT].name; }

// ---------- setup ----------

// A sprite pixel is 4x4 samples, for a clean edge. Every sample any sprite needs sits on one
// grid of quarter pixels around the ball's centre, so the lighting is worked out once per
// point of it and every shade reuses that.
constexpr int GRID = 8 * RADIUS_PX + 7;
static int16_t gridShade[GRID * GRID], gridGlint[GRID * GRID];  // Q12; shade -1 off the ball

static void lightGrid() {
  const float r = RADIUS_PX;
  for (int my = 0; my < GRID; my++) {
    for (int mx = 0; mx < GRID; mx++) {
      float dx = (mx - GRID / 2 + 0.5f) / 4, dy = (my - GRID / 2 + 0.5f) / 4;
      float d2 = dx * dx + dy * dy;
      int k = my * GRID + mx;
      if (d2 > r * r) {
        gridShade[k] = -1;
        continue;
      }
      // Lit from the top left.
      float nx = dx / r, ny = dy / r, nz = sqrtf(1.0f - d2 / (r * r));
      float light = -0.45f * nx - 0.55f * ny + 0.7f * nz;
      if (light < 0) light = 0;
      float glint = light * light * light * light * light * light;
      gridShade[k] = (int16_t)lroundf((DARK_SIDE + (1 - DARK_SIDE) * light) * 4096);
      gridGlint[k] = (int16_t)lroundf(glint * 4096);
    }
  }
}

// Where sample (sx, sy) of pixel (x, y) falls on the grid, for a ball offset by (qx, qy)
// quarter pixels.
static inline int gridPoint(int x, int y, int sx, int sy, int qx, int qy) {
  return (4 * (y - RADIUS_PX) + sy - qy + GRID / 2) * GRID + 4 * (x - RADIUS_PX) + sx - qx + GRID / 2;
}

// How bright each sprite pixel is, the same in every colour: what keeps the brighter of two
// overlapping edges.
static void buildKeys() {
  for (int q = 0; q < 16; q++) {
    for (int y = 0; y < SPRITE; y++) {
      for (int x = 0; x < SPRITE; x++) {
        int32_t sum = 0;
        for (int sy = 0; sy < 4; sy++) {
          for (int sx = 0; sx < 4; sx++) {
            int g = gridPoint(x, y, sx, sy, q & 3, q >> 2);
            if (gridShade[g] >= 0) sum += gridShade[g] + gridGlint[g];
          }
        }
        spriteKeys[q][y * SPRITE + x] = (uint8_t)((sum * 255) >> 17);  // 16 samples of up to 2.0 in Q12
      }
    }
  }
}

// Each channel stops at 255 per sample, before the 16 are averaged.
static void buildSprites(const Palette &p) {
  for (int s = 0; s < SHADES; s++) {
    int32_t lit[3];
    float at = (float)s * (STOPS - 1) / (SHADES - 1);
    int k = (int)at < STOPS - 1 ? (int)at : STOPS - 2;  // the floor is the far end of the last stretch
    float f = at - k;
    for (int c = 0; c < 3; c++) lit[c] = lroundf(p.lit[k][c] + (p.lit[k + 1][c] - p.lit[k][c]) * f);
    for (int q = 0; q < 16; q++) {
      for (int y = 0; y < SPRITE; y++) {
        for (int x = 0; x < SPRITE; x++) {
          int32_t sum[3] = {0, 0, 0};
          for (int sy = 0; sy < 4; sy++) {
            for (int sx = 0; sx < 4; sx++) {
              int g = gridPoint(x, y, sx, sy, q & 3, q >> 2);
              if (gridShade[g] < 0) continue;
              for (int c = 0; c < 3; c++) {
                int32_t v = lit[c] * gridShade[g] + p.glint[c] * gridGlint[g];
                sum[c] += v < (255 << 12) ? v : (255 << 12);
              }
            }
          }
          // Q12 over 16 samples: >> 16 is the average.
          uint16_t c = (sum[0] >> 19) << 11 | (sum[1] >> 18) << 5 | sum[2] >> 19;
          sprites[s][q][y * SPRITE + x] = (uint16_t)(c >> 8 | c << 8);  // panel byte order
        }
      }
    }
  }
}

static void setPalette(int palette) {
  buildSprites(PALETTES[palette % PALETTE_COUNT]);
  findColours(true);
  memcpy(shownShade, shade, sizeof(shade));
  redrawAll();
}

static bool insideScreen(int32_t x, int32_t y) {
  if (x < MIN_X || x > MAX_X || y < MIN_Y || y > MAX_Y) return false;
  int32_t ax = x < ARC ? x - ARC : x > (WIDTH << 16) - ARC ? x - ((WIDTH << 16) - ARC) : 0;
  int32_t ay = y < ARC ? y - ARC : y > (HEIGHT << 16) - ARC ? y - ((HEIGHT << 16) - ARC) : 0;
  if (!ax || !ay) return true;
  ax >>= 6;
  ay >>= 6;
  return (uint32_t)(ax * ax + ay * ay) <= (uint32_t)ARC_REACH_Q10 * (uint32_t)ARC_REACH_Q10;
}

constexpr float SPACING_PX = 2.1f * RADIUS_PX;  // about how far apart the balls of a settled pool sit
constexpr float ROW_GAP_PX = SPACING_PX * 0.866f;
// A settled pool is squeezed that tight by its own weight. Laid out that tight anywhere else,
// it springs apart. The fireball wants that, but the balloon would burst across the screen in
// a tenth of a second and the lake's top rows would peel off one by one, so those are looser.
constexpr float LOOSE_SPACING_PX = 2.6f * RADIUS_PX;
// The balloon keeps this far from the edges. It keeps off the rounded corners too: 12 px in
// from both edges is 17 px from the corner arc's centre, and a ball's centre may be 21 from it.
constexpr float MARGIN_PX = 12;
constexpr float GAP_PX = 30;    // the ocean's ledge stops this short of each edge, for the water to pour through
constexpr float SLOPE = 0.45f;  // and slopes down this steeply from its ridge, about 24 degrees

static uint32_t nextNoise() {  // xorshift
  noise ^= noise << 13;
  noise ^= noise >> 17;
  noise ^= noise << 5;
  return noise;
}

static float noiseBetween() {  // -1 to 1
  return (nextNoise() & 0xFFFF) * (2.0f / 65535) - 1;
}

// Every ball still, where it was just put.
static void stillFromHere() {
  for (int i = 0; i < COUNT; i++) {
    Ball &ball = balls[i];
    ball.vx = ball.vy = 0;
    ball.oldX = ball.x;
    ball.oldY = ball.y;
    ball.crowd = ball.closeCrowd = 0;
    shownX[i] = (ball.x + (1 << 13)) >> 14;
    shownY[i] = (ball.y + (1 << 13)) >> 14;
    order[i] = i;
  }
  pairsStale = true;
}

// What the pool does as a whole is taken out, so it comes apart where it is.
static void noDrift() {
  int64_t sumX = 0, sumY = 0;
  for (int i = 0; i < COUNT; i++) {
    sumX += balls[i].vx;
    sumY += balls[i].vy;
  }
  int32_t driftX = (int32_t)(sumX / COUNT), driftY = (int32_t)(sumY / COUNT);
  for (int i = 0; i < COUNT; i++) {
    balls[i].vx -= driftX;
    balls[i].vy -= driftY;
  }
}

// A still pool on the floor, in rows from the bottom up.
static void layPool() {
  int placed = 0;
  for (int r = 0; placed < COUNT; r++) {
    float y = HEIGHT - RADIUS_PX - r * ROW_GAP_PX;
    for (float x = SPACING_PX * (r & 1 ? 0.75f : 0.25f); x < WIDTH && placed < COUNT; x += SPACING_PX) {
      int32_t px = (int32_t)(x * ONE), py = (int32_t)(y * ONE);
      if (!insideScreen(px, py)) continue;
      balls[placed].x = px;
      balls[placed].y = py;
      placed++;
    }
  }
  stillFromHere();
}

// A round cluster with its balls about spacing apart. A golden-angle spiral fills the disc
// evenly with no rows to come apart along, and it starts at a new angle each time.
static float discRadius(float spacing) {
  return sqrtf(COUNT * 0.866f * spacing * spacing / 3.14159f);
}

static void layDisc(float cx, float cy, float radius) {
  float turn = (nextNoise() & 0xFFFF) * (6.28319f / 65536);
  for (int i = 0; i < COUNT; i++) {
    float r = radius * sqrtf((i + 0.5f) / COUNT), a = turn + i * 2.39996f;
    balls[i].x = (int32_t)((cx + r * cosf(a)) * ONE);
    balls[i].y = (int32_t)((cy + r * sinf(a)) * ONE);
  }
  stillFromHere();
}

// Jungle: water in a balloon, as far up against gravity as it fits. It pops: a small push each
// its own way makes it break into drops as it falls, instead of swelling as one disc.
static void layBalloon() {
  const float radius = discRadius(LOOSE_SPACING_PX);
  // Across a zero, the room divides to infinity, which never wins.
  float up = fminf((WIDTH / 2 - radius - MARGIN_PX) / fabsf(upX), (HEIGHT / 2 - radius - MARGIN_PX) / fabsf(upY));
  layDisc(WIDTH / 2 + upX * up, HEIGHT / 2 + upY * up, radius);
  constexpr int32_t MOST = (int32_t)(BURST_PX / STEPS_PER_SECOND * 65536.0f);
  for (int i = 0; i < COUNT; i++) {
    balls[i].vx = (int32_t)(nextNoise() % (2 * MOST + 1)) - MOST;
    balls[i].vy = (int32_t)(nextNoise() % (2 * MOST + 1)) - MOST;
  }
  noDrift();
}

// Fire: a fireball in the middle, as tight as a settled pool, explodes. Every ball flies
// straight out, the outside fastest, each a little off in speed and line so it scatters, and
// hits a wall. Its own pressure adds to the blast.
static void layFireball() {
  constexpr float CORE = 0.4f;  // the middle's share of the outside's speed
  constexpr float SPEED = BLAST_PX / STEPS_PER_SECOND * 65536.0f;  // Q16 px per step
  const float radius = discRadius(SPACING_PX);
  layDisc(WIDTH / 2, HEIGHT / 2, radius);
  for (int i = 0; i < COUNT; i++) {
    float dx = balls[i].x * (1.0f / ONE) - WIDTH / 2, dy = balls[i].y * (1.0f / ONE) - HEIGHT / 2;
    float r = sqrtf(dx * dx + dy * dy);  // never 0: the spiral's first ball is off the middle
    float out = SPEED * (CORE + (1 - CORE) * r / radius) * (1 + 0.2f * noiseBetween()) / r;
    float off = 0.2f * noiseBetween();  // across, as a share of the speed out
    balls[i].vx = (int32_t)(out * (dx - off * dy));
    balls[i].vy = (int32_t)(out * (dy + off * dx));
  }
  noDrift();
}

// The point h up and s across from the middle of the screen, in Q16 px, and whether a ball's
// centre can be there.
static bool screenPoint(float h, float s, int32_t &x, int32_t &y) {
  x = (int32_t)((WIDTH / 2 + h * upX - s * upY) * ONE);
  y = (int32_t)((HEIGHT / 2 + h * upY + s * upX) * ONE);
  return insideScreen(x, y);
}

// Ocean: water draped over a low ridge up against gravity. The ridge slopes down both ways and
// stops short of each edge: the water over the gaps falls straight away, the rest slides off
// both ends after it, and it all pours down the sides.
static void layLake() {
  const float top = fabsf(upX) * (WIDTH / 2 - RADIUS_PX) + fabsf(upY) * (HEIGHT / 2 - RADIUS_PX);
  // Rows bent over the ridge, from the top down. A row's middle is ridge high, its sides slope
  // away as the ridge does, and it fills from the middle out, so the last row is centred.
  float ridge = top;
  int placed = 0;
  for (int r = 0; placed < COUNT; r++) {
    ridge = top - r * LOOSE_SPACING_PX * 0.866f;
    for (int k = 0; k < 2 * HALF_DIAGONAL_PX / LOOSE_SPACING_PX && placed < COUNT; k++) {
      float s = r & 1 ? (k >> 1) + 0.5f : (k + 1) >> 1;  // 0, 1, 1, 2, 2... or 0.5, 0.5, 1.5...
      if ((k & 1) != (r & 1)) s = -s;                      // each to both sides in turn
      s *= LOOSE_SPACING_PX;
      int32_t x, y;
      if (!screenPoint(ridge - SLOPE * fabsf(s), s, x, y)) continue;
      balls[placed].x = x;
      balls[placed].y = y;
      placed++;
    }
  }
  stillFromHere();

  // The ledge is the lowest row's ridge, out each way to the edge of the screen less a gap.
  float reach[2];
  for (int side = 0; side < 2; side++) {
    float s = 0, way = side ? 1.0f : -1.0f;
    int32_t x, y;
    while (s < HALF_DIAGONAL_PX && screenPoint(ridge - SLOPE * (s + 0.5f), way * (s + 0.5f), x, y)) s += 0.5f;
    reach[side] = s - GAP_PX;
  }
  ledgeUpX = lroundf(upX * 16384);
  ledgeUpY = lroundf(upY * 16384);
  ledgeHeight = lroundf(ridge * 256);
  ledgeFrom = lroundf(-reach[0] * 256);
  ledgeTo = lroundf(reach[1] * 256);
  ledgeSlope = lroundf(SLOPE * 16384);
  // Square to each side's slope, and as long as a push of the height a ball is under needs to
  // be to put it back on the surface.
  for (int side = 0; side < 2; side++) {
    float out = side ? SLOPE : -SLOPE;
    ledgeOutX[side] = lroundf((upX - out * upY) / (1 + SLOPE * SLOPE) * 16384);
    ledgeOutY[side] = lroundf((upY + out * upX) / (1 + SLOPE * SLOPE) * 16384);
  }
  ledgeSteps = (int)(POUR_S * STEPS_PER_SECOND + 0.5f);
}

void begin(int palette) {
  for (int i = 0; i < TABLE_SIZE; i++) {
    float v = i ? (float)SCALE / sqrtf((float)((uint32_t)i << TABLE_SHIFT)) : 65535.0f;
    inverseTable[i] = v > 65535.0f ? 65535 : (uint16_t)lroundf(v);
  }

  lightGrid();
  buildKeys();
  layPool();
  upX = 0;
  upY = -1;
  ledgeSteps = 0;
  setPalette(palette);
}

void dropIn(int palette) {
  ledgeSteps = 0;
  switch (PALETTES[palette % PALETTE_COUNT].entrance) {
    case BLAST: layFireball(); break;
    case POUR: layLake(); break;
    case BALLOON: layBalloon(); break;
  }
  setPalette(palette);
}

}  // namespace dots
