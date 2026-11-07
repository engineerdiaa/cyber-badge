#include "imu.h"
#include <math.h>
#include <string.h>

namespace imu {

constexpr float RAD_PER_DEGREE = 0.017453293f;

static float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void cross(const float a[3], const float b[3], float out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

void Watch::feed(const float a[3], const float g[3], int ms) {
  for (int i = 0; i < 3; i++) {
    sumA[i] += a[i];
    sumG[i] += g[i];
    if (!filled || a[i] < lowA[i]) lowA[i] = a[i];
    if (!filled || a[i] > highA[i]) highA[i] = a[i];
    if (!filled || g[i] < lowG[i]) lowG[i] = g[i];
    if (!filled || g[i] > highG[i]) highG[i] = g[i];
  }

  // Gravity turns the other way from the board, which the gyro follows through a flip of
  // any speed. Shaking averages out, so settling on the accelerometer takes out the gyro's
  // errors and leaves the shaking behind. Two half steps: one would grow the vector.
  if (!started) {
    memcpy(down, a, sizeof(down));
    started = true;
  } else {
    float seconds = ms * 0.001f, turn[3], slope[3], middle[3];
    for (int i = 0; i < 3; i++) turn[i] = (g[i] - restG[i]) * RAD_PER_DEGREE * seconds;
    cross(down, turn, slope);
    for (int i = 0; i < 3; i++) middle[i] = down[i] + slope[i] / 2;
    cross(middle, turn, slope);
    for (int i = 0; i < 3; i++) down[i] += slope[i] + (a[i] - down[i]) * seconds / GRAVITY_SECONDS;
  }

  // A shake swings the badge back and forth along its screen. Picking it up or putting it
  // down is one push and one stop, mostly through the screen while it lies flat, and a knock
  // is gone within a reading.
  float swing[3], screen[3];
  for (int i = 0; i < 3; i++) swing[i] = a[i] - down[i];
  toScreen(swing, screen);
  sinceSwing = sinceSwing < SWING_MS ? sinceSwing + ms : SWING_MS;
  if (sinceSwing >= SWING_MS) swings = 0;
  if (screen[0] * screen[0] + screen[1] * screen[1] > SWING_G * SWING_G) {
    if (swings == 0 || screen[0] * lastSwing[0] + screen[1] * lastSwing[1] < 0) {
      swings++;
      sinceSwing = 0;
    }
    lastSwing[0] = screen[0];
    lastSwing[1] = screen[1];
  }

  // Lying flat, gravity barely pulls along the screen, so after the badge is put down the
  // balls would float about for seconds. Once it lies still they get what the accelerometer
  // reads standing upright, the bottom edge down, and sink straight back.
  onTable = lying() ? fminf(onTable + (float)ms / TABLE_IN_MS, 1.0f) : fmaxf(onTable - (float)ms / TABLE_OUT_MS, 0.0f);
  for (int i = 0; i < 3; i++) shown[i] = down[i] + (-MOUNT.y[i] - down[i]) * onTable;

  if (++filled < WINDOW) return;

  float spanG = 0, spanDps = 0, driftG = 0, meanA[3];
  for (int i = 0; i < 3; i++) {
    meanA[i] = sumA[i] / WINDOW;
    spanG = fmaxf(spanG, highA[i] - lowA[i]);
    spanDps = fmaxf(spanDps, highG[i] - lowG[i]);
    driftG = fmaxf(driftG, fabsf(meanA[i] - lastMeanA[i]));
  }
  // Really holding still, so whatever the gyro says now is its zero. A slow turn looks calm
  // within one window, and taking that for the zero leans the whole idea of down.
  if (spanG <= CALM_G && spanDps <= CALM_DPS && driftG <= STEADY_G) {
    for (int i = 0; i < 3; i++) {
      float meanG = sumG[i] / WINDOW;
      restG[i] = zeroKnown ? restG[i] + (meanG - restG[i]) / 8 : meanG;  // it wanders with temperature
    }
    zeroKnown = true;
  }
  // A table holds it far stiller than a hand: in Diaa's recording 0.4 dps at most, against at
  // least 4 even holding it still.
  float flat[3];
  toScreen(down, flat);
  if (spanDps <= TABLE_DPS && spanG <= TABLE_G && flat[0] * flat[0] + flat[1] * flat[1] < TABLE_FLAT_G * TABLE_FLAT_G) {
    if (still < TABLE_WINDOWS) still++;
  } else {
    still = 0;
  }
  memcpy(lastMeanA, meanA, sizeof(lastMeanA));
  memset(sumA, 0, sizeof(sumA));
  memset(sumG, 0, sizeof(sumG));
  filled = 0;
}

void toScreen(const float sensor[3], float screen[3]) {
  float out[3] = {dot(MOUNT.x, sensor), dot(MOUNT.y, sensor), dot(MOUNT.z, sensor)};
  memcpy(screen, out, sizeof(out));
}

}  // namespace imu

// ---------- the sensor itself ----------

#ifdef ARDUINO
#include <Arduino.h>
#include <Wire.h>

namespace imu {

constexpr uint8_t ADDRESSES[] = {0x6A, 0x6B};  // SA0 low, SA0 high
constexpr uint8_t WHO_AM_I = 0x0F, CTRL1_XL = 0x10, CTRL2_G = 0x11, CTRL3_C = 0x12, OUTX_L_G = 0x22;
constexpr uint8_t ID_DS3 = 0x69, ID_DS3TR_C = 0x6A;
constexpr uint8_t RESET = 0x01;
constexpr uint8_t BLOCK_UPDATE_AUTO_INCREMENT = 0x44;  // samples never tear, bursts walk the registers
constexpr uint8_t ACCEL_208HZ_8G = 0x5C;
constexpr uint8_t GYRO_208HZ_2000DPS = 0x5C;
constexpr float G_PER_LSB = 0.000244f;
constexpr float DPS_PER_LSB = 0.07f;
constexpr int GIVE_UP_AFTER = 40;  // failed reads in a row

static uint8_t address;
static Watch watch;
static volatile bool answering, wasShaken;
static volatile int sampleMs = SAMPLE_MS;

// Shared with the task.
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static float sumGravity[3], sumShake[3], sumTurn[3];
static int summed;

static bool writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool readRegisters(uint8_t reg, uint8_t *out, uint8_t count) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(address, (size_t)count) != count) return false;
  for (int i = 0; i < count; i++) out[i] = Wire.read();
  return true;
}

static bool readSample(float a[3], float g[3]) {
  uint8_t raw[12];  // gyro X Y Z, then accelerometer X Y Z, low byte first
  if (!readRegisters(OUTX_L_G, raw, sizeof(raw))) return false;
  for (int i = 0; i < 3; i++) {
    g[i] = (int16_t)(raw[i * 2] | raw[i * 2 + 1] << 8) * DPS_PER_LSB;
    a[i] = (int16_t)(raw[i * 2 + 6] | raw[i * 2 + 7] << 8) * G_PER_LSB;
  }
  return true;
}

// Its own task, so a wait on the bus (the OLED holds it for 7 ms at a time) costs nobody
// else anything.
static void sensorTask(void *) {
  int failures = 0;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    int ms = sampleMs;
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(ms));
    float a[3], g[3];
    if (!readSample(a, g)) {
      if (++failures >= GIVE_UP_AFTER) answering = false;
      continue;
    }
    failures = 0;
    answering = true;

    watch.feed(a, g, ms);
    if (watch.shaken()) wasShaken = true;

    const float *down = watch.gravity(), *felt = watch.ballGravity();
    portENTER_CRITICAL(&lock);
    for (int i = 0; i < 3; i++) {
      sumGravity[i] += felt[i];
      sumShake[i] += a[i] - down[i];
      sumTurn[i] += g[i];
    }
    summed++;
    portEXIT_CRITICAL(&lock);
  }
}

bool begin() {
  for (uint8_t candidate : ADDRESSES) {
    address = candidate;
    uint8_t id = 0;
    if (readRegisters(WHO_AM_I, &id, 1) && (id == ID_DS3 || id == ID_DS3TR_C)) {
      writeRegister(CTRL3_C, RESET);
      delay(20);
      if (!writeRegister(CTRL3_C, BLOCK_UPDATE_AUTO_INCREMENT) || !writeRegister(CTRL1_XL, ACCEL_208HZ_8G) ||
          !writeRegister(CTRL2_G, GYRO_208HZ_2000DPS)) {
        return false;
      }
      delay(100);  // the gyro's start-up time
      answering = true;
      xTaskCreate(sensorTask, "imu", 4096, nullptr, 3, nullptr);
      return true;
    }
  }
  return false;
}

void setFast(bool fast) {
  sampleMs = fast ? SAMPLE_MS : SLOW_SAMPLE_MS;
  wasShaken = false;
}

bool working() { return answering; }

bool shaken() { return wasShaken; }

void restart() {
  portENTER_CRITICAL(&lock);
  memset(sumGravity, 0, sizeof(sumGravity));
  memset(sumShake, 0, sizeof(sumShake));
  memset(sumTurn, 0, sizeof(sumTurn));
  summed = 0;
  portEXIT_CRITICAL(&lock);
}

bool takeMotion(dots::Motion &m) {
  float gravity[3], shake[3], turn[3];
  portENTER_CRITICAL(&lock);
  int count = summed;
  memcpy(gravity, sumGravity, sizeof(gravity));
  memcpy(shake, sumShake, sizeof(shake));
  memcpy(turn, sumTurn, sizeof(turn));
  memset(sumGravity, 0, sizeof(sumGravity));
  memset(sumShake, 0, sizeof(sumShake));
  memset(sumTurn, 0, sizeof(sumTurn));
  summed = 0;
  portEXIT_CRITICAL(&lock);
  if (!count) return false;

  const float *rest = watch.gyroRest();
  for (int i = 0; i < 3; i++) {
    gravity[i] /= count;
    shake[i] /= count;
    turn[i] = (turn[i] / count - rest[i]) * RAD_PER_DEGREE;
  }
  float screenGravity[3], screenShake[3], screenTurn[3];
  toScreen(gravity, screenGravity);
  toScreen(shake, screenShake);
  toScreen(turn, screenTurn);
  m = {screenGravity[0], screenGravity[1], screenShake[0], screenShake[1], screenTurn[0], screenTurn[1], screenTurn[2],
       count * sampleMs * 0.001f};
  return true;
}

}  // namespace imu
#endif  // ARDUINO
