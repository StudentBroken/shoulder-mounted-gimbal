#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "config.h"

// ==============================================================================
// Unified Dual-Sensor 6-DOF IMU Driver (Bosch GY-BMI160 & InvenSense MPU6050)
// Features:
//   - Automatic hardware detection (BMI160 0xD1 vs MPU6050 0x68/clones)
//   - Hardware OSR4 (BMI160) / DLPF (MPU6050) vibration filtering
//   - Continuous Stationary Gyro Auto-Bias Learning (eliminates thermal yaw drift)
//   - Gravity-norm gated accelerometer fusion (prevents servo vibration jitter)
// ==============================================================================

// --- MPU6050 Registers ---
#define MPU6050_REG_SMPLRT_DIV   0x19
#define MPU6050_REG_CONFIG       0x1A
#define MPU6050_REG_GYRO_CONFIG  0x1B
#define MPU6050_REG_ACCEL_CONFIG 0x1C
#define MPU6050_REG_ACCEL_XOUT_H 0x3B
#define MPU6050_REG_PWR_MGMT_1   0x6B
#define MPU6050_REG_WHO_AM_I     0x75

// --- Bosch BMI160 Registers ---
#define BMI160_REG_CHIP_ID       0x00
#define BMI160_REG_PMU_STATUS    0x03
#define BMI160_REG_DATA_GYR_X_L  0x0C // 0x0C..0x11 = Gyro XYZ (LE), 0x12..0x17 = Accel XYZ (LE)
#define BMI160_REG_ACC_CONF      0x40
#define BMI160_REG_ACC_RANGE     0x41
#define BMI160_REG_GYR_CONF      0x42
#define BMI160_REG_GYR_RANGE     0x43
#define BMI160_REG_CMD           0x7E

#define BMI160_CHIP_ID_VAL       0xD1
#define BMI160_CMD_SOFTRESET     0xB6
#define BMI160_CMD_ACC_NORMAL    0x11
#define BMI160_CMD_GYR_NORMAL    0x15

enum ImuOrientation {
  ORIENT_DEFAULT,           // Standard flat: X forward, Y left, Z up
  ORIENT_Y_FWD_ROLL_POS90,  // Legacy: +Y forward, rolled +90° around +Y
  ORIENT_Y_FWD_ROLL_NEG90,  // Legacy: +Y forward, rolled -90° around +Y
  ORIENT_VERT_AX_DOWN_FWD,  // Vertical board: +X points DOWN, +Y faces FORWARD, pitch tilt around Z, yaw around X
  ORIENT_VERT_AX_DOWN_BACK  // Vertical board: +X points DOWN, +Y faces BACKWARD (180° yaw), pitch tilt around Z, yaw around X
};

enum ImuChipType : uint8_t {
  IMU_CHIP_UNKNOWN = 0,
  IMU_CHIP_MPU6050 = 1,
  IMU_CHIP_BMI160  = 2
};

class Mpu6050Lite {
public:
  uint8_t address = 0x68;
  TwoWire* wire = &Wire;
  bool isConnected = false;
  ImuChipType chipType = IMU_CHIP_UNKNOWN;
  ImuOrientation orientation = ORIENT_DEFAULT;

  // Raw and converted values (in sensor frame)
  float ax = 0, ay = 0, az = 0; // Accelerometer in g
  float gx = 0, gy = 0, gz = 0; // Gyroscope in deg/s

  // Low-pass filtered accelerometer components (rejects high-frequency servo chatter)
  float ax_f = 0, ay_f = 0, az_f = 0;
  bool accelFilterPrimed = false;

  // Gyro biases for all 3 axes (calibrated on ground + continuously refined when still)
  float gx_bias = 0.0f;
  float gy_bias = 0.0f;
  float gz_bias = 0.0f;
  float pitch_offset = 0.0f; // Accelerometer resting pitch offset

  // Stationary ZUPT (Zero-Velocity Update) tracker for automatic thermal drift cancellation
  float stationaryTimeSec = 0.0f;

  // Filtered Euler Angles
  float pitch = 0.0f;
  float yaw = 0.0f;

  // 3D Gravity-Aligned Orthonormal Basis (calibrated on zeroing to cancel neck-tape skew/tilt):
  // - yaw_axis: unit vector along true vertical axis (gravity) so flat head turns are 100% Yaw
  // - pitch_axis: unit vector strictly orthogonal to yaw_axis so flat head turns cause 0 Pitch bleed
  float yaw_axis_x = -1.0f, yaw_axis_y = 0.0f, yaw_axis_z = 0.0f;
  float pitch_axis_x = 0.0f, pitch_axis_y = 0.0f, pitch_axis_z = 1.0f;
  float mountTiltDeg = 0.0f; // Diagnostic: measured sideways/forward tilt angle of mount from vertical

  const char* getChipName() const {
    switch (chipType) {
      case IMU_CHIP_BMI160:  return "Bosch BMI160";
      case IMU_CHIP_MPU6050: return "MPU6050";
      default:               return "Unknown";
    }
  }

  bool begin(uint8_t i2cAddr, ImuOrientation orient = ORIENT_DEFAULT, TwoWire* wireInstance = &Wire) {
    address = i2cAddr;
    orientation = orient;
    wire = wireInstance;
    isConnected = false;
    chipType = IMU_CHIP_UNKNOWN;
    accelFilterPrimed = false;
    stationaryTimeSec = 0.0f;
    resetDefaultMountAxes();

    // Verify device ACK on I2C bus
    wire->beginTransmission(address);
    uint8_t err = wire->endTransmission(true);
    if (err != 0) {
      Serial.printf("[IMU 0x%02X] I2C error %d (NACK/not found).\n", address, err);
      return false;
    }

    // 1. Probe Bosch BMI160 CHIP_ID (Register 0x00 == 0xD1)
    uint8_t chipId00 = 0;
    if (readRegister(BMI160_REG_CHIP_ID, chipId00) && chipId00 == BMI160_CHIP_ID_VAL) {
      return initBmi160();
    }

    // 2. Probe MPU6050 WHO_AM_I (Register 0x75 == 0x68, 0x70, 0x72, 0x98)
    uint8_t whoami75 = 0;
    if (readRegister(MPU6050_REG_WHO_AM_I, whoami75) &&
        (whoami75 == 0x68 || whoami75 == 0x70 || whoami75 == 0x71 || whoami75 == 0x72 || whoami75 == 0x98)) {
      return initMpu6050(whoami75);
    }

    // 3. Fallback: Issue BMI160 soft-reset in case BMI160 was mid-transaction on warm reboot
    writeRegister(BMI160_REG_CMD, BMI160_CMD_SOFTRESET);
    delay(20);
    if (readRegister(BMI160_REG_CHIP_ID, chipId00) && chipId00 == BMI160_CHIP_ID_VAL) {
      return initBmi160();
    }

    // 4. Default to MPU6050 initialization if device ACKed but had non-standard WHO_AM_I
    Serial.printf("[IMU 0x%02X] Warning: Unrecognized ID (reg0x00=0x%02X, reg0x75=0x%02X), attempting MPU6050 init...\n",
                  address, chipId00, whoami75);
    return initMpu6050(whoami75);
  }

  bool initBmi160() {
    chipType = IMU_CHIP_BMI160;
    Serial.printf("[IMU 0x%02X] Detected Bosch GY-BMI160 (CHIP_ID = 0xD1)\n", address);

    // 1. Soft-reset to clean state
    writeRegister(BMI160_REG_CMD, BMI160_CMD_SOFTRESET);
    delay(20);
    uint8_t dummy = 0;
    readRegister(BMI160_REG_CHIP_ID, dummy); // Dummy I2C read recommended after reset

    // 2. Wake Accelerometer to Normal Mode (0x11)
    writeRegister(BMI160_REG_CMD, BMI160_CMD_ACC_NORMAL);
    delay(10);

    // 3. Wake Gyroscope to Normal Mode (0x15) - requires ~60-80ms for MEMS spin-up
    writeRegister(BMI160_REG_CMD, BMI160_CMD_GYR_NORMAL);
    delay(80);

    // Verify PMU_STATUS (0x03): bits [5:4]=01 (acc normal), bits [3:2]=01 (gyr normal) -> 0x14
    uint8_t pmu = 0;
    readRegister(BMI160_REG_PMU_STATUS, pmu);
    if ((pmu & 0x3C) != 0x14) {
      // Retry wakeup once if needed
      writeRegister(BMI160_REG_CMD, BMI160_CMD_ACC_NORMAL);
      delay(15);
      writeRegister(BMI160_REG_CMD, BMI160_CMD_GYR_NORMAL);
      delay(85);
      readRegister(BMI160_REG_PMU_STATUS, pmu);
    }

    // 4. Configure Accelerometer:
    //    ACC_CONF (0x40) = 0x09 -> OSR4 (4x oversampling sharp anti-vibration DLPF) + 200Hz ODR
    //    ACC_RANGE (0x41) = 0x05 -> ±4g (8192 LSB/g)
    writeRegister(BMI160_REG_ACC_CONF, 0x09);
    writeRegister(BMI160_REG_ACC_RANGE, 0x05);

    // 5. Configure Gyroscope:
    //    GYR_CONF (0x42) = 0x09 -> OSR4 (4x oversampling ~20Hz hardware DLPF) + 200Hz ODR
    //    GYR_RANGE (0x43) = 0x02 -> ±500 deg/s (65.536 LSB/deg/s)
    writeRegister(BMI160_REG_GYR_CONF, 0x09);
    writeRegister(BMI160_REG_GYR_RANGE, 0x02);

    delay(10);
    Serial.printf("[IMU 0x%02X] BMI160 Active (PMU=0x%02X, ODR=200Hz, Filter=OSR4, ±4g, ±500dps)\n", address, pmu);
    isConnected = true;
    return true;
  }

  bool initMpu6050(uint8_t whoami) {
    chipType = IMU_CHIP_MPU6050;
    Serial.printf("[IMU 0x%02X] Detected MPU6050 (WHO_AM_I = 0x%02X)\n", address, whoami);

    // 1. Wake up device, use PLL with X gyro clock reference (0x01)
    writeRegister(MPU6050_REG_PWR_MGMT_1, 0x01);
    delay(10);

    // 2. DLPF Configuration: 20Hz DLPF bandwidth (0x04 - stronger servo vibration rejection)
    writeRegister(MPU6050_REG_CONFIG, 0x04);

    // 3. Sample Rate Divider: 1kHz / (1 + 4) = 200Hz
    writeRegister(MPU6050_REG_SMPLRT_DIV, 0x04);

    // 4. Gyro Config: ±500 deg/s (65.5 LSB per deg/s)
    writeRegister(MPU6050_REG_GYRO_CONFIG, 0x08);

    // 5. Accel Config: ±4g (8192 LSB per g)
    writeRegister(MPU6050_REG_ACCEL_CONFIG, 0x08);

    isConnected = true;
    return true;
  }

  bool readRawData() {
    if (!isConnected) return false;

    if (chipType == IMU_CHIP_BMI160) {
      wire->beginTransmission(address);
      wire->write(BMI160_REG_DATA_GYR_X_L);
      if (wire->endTransmission(true) != 0) {
        return false;
      }

      // Read 12 consecutive bytes (Little-Endian):
      // Gyro X (2), Gyro Y (2), Gyro Z (2), Accel X (2), Accel Y (2), Accel Z (2)
      uint8_t bytesReceived = wire->requestFrom((uint8_t)address, (uint8_t)12, (uint8_t)true);
      if (bytesReceived < 12) return false;

      int16_t raw_gx = (int16_t)(wire->read() | (wire->read() << 8));
      int16_t raw_gy = (int16_t)(wire->read() | (wire->read() << 8));
      int16_t raw_gz = (int16_t)(wire->read() | (wire->read() << 8));
      int16_t raw_ax = (int16_t)(wire->read() | (wire->read() << 8));
      int16_t raw_ay = (int16_t)(wire->read() | (wire->read() << 8));
      int16_t raw_az = (int16_t)(wire->read() | (wire->read() << 8));

      // Scale factors: ±4g = 8192 LSB/g, ±500 dps = 65.536 LSB/(deg/s)
      ax = (float)raw_ax / 8192.0f;
      ay = (float)raw_ay / 8192.0f;
      az = (float)raw_az / 8192.0f;

      gx = (float)raw_gx / 65.536f;
      gy = (float)raw_gy / 65.536f;
      gz = (float)raw_gz / 65.536f;
    } else {
      wire->beginTransmission(address);
      wire->write(MPU6050_REG_ACCEL_XOUT_H);
      if (wire->endTransmission(false) != 0) {
        return false;
      }

      // Read 14 consecutive registers (Big-Endian):
      // Accel X (2), Accel Y (2), Accel Z (2), Temp (2), Gyro X (2), Gyro Y (2), Gyro Z (2)
      uint8_t bytesReceived = wire->requestFrom((uint8_t)address, (uint8_t)14, (uint8_t)true);
      if (bytesReceived < 14) return false;

      int16_t raw_ax = (int16_t)((wire->read() << 8) | wire->read());
      int16_t raw_ay = (int16_t)((wire->read() << 8) | wire->read());
      int16_t raw_az = (int16_t)((wire->read() << 8) | wire->read());
      wire->read(); wire->read(); // Skip temperature
      int16_t raw_gx = (int16_t)((wire->read() << 8) | wire->read());
      int16_t raw_gy = (int16_t)((wire->read() << 8) | wire->read());
      int16_t raw_gz = (int16_t)((wire->read() << 8) | wire->read());

      // Scale factors: ±4g = 8192 LSB/g, ±500 dps = 65.5 LSB/(deg/s)
      ax = (float)raw_ax / 8192.0f;
      ay = (float)raw_ay / 8192.0f;
      az = (float)raw_az / 8192.0f;

      gx = (float)raw_gx / 65.5f;
      gy = (float)raw_gy / 65.5f;
      gz = (float)raw_gz / 65.5f;
    }

    // Update low-pass filtered accelerometer state (smooths out mechanical servo frame vibration)
    if (!accelFilterPrimed) {
      ax_f = ax;
      ay_f = ay;
      az_f = az;
      accelFilterPrimed = true;
    } else {
      const float accLpfAlpha = 0.25f;
      ax_f += accLpfAlpha * (ax - ax_f);
      ay_f += accLpfAlpha * (ay - ay_f);
      az_f += accLpfAlpha * (az - az_f);
    }

    return true;
  }

  // Calculate pitch from filtered accelerometer based on physical mounting orientation
  float calculateRawAccelPitch() {
    float use_ax = accelFilterPrimed ? ax_f : ax;
    float use_ay = accelFilterPrimed ? ay_f : ay;
    float use_az = accelFilterPrimed ? az_f : az;

    switch (orientation) {
    case ORIENT_VERT_AX_DOWN_FWD:
      // Vertical board facing Forward:
      // Gravity points down +X at rest.
      // Pitch tilt (nodding up/down) rotates around lateral axis. Accelerometer Z swings against X.
      return atan2f(use_az, use_ax) * (180.0f / (float)M_PI);

    case ORIENT_VERT_AX_DOWN_BACK:
      // Vertical board facing Backward (180° yaw flipped):
      // Nodding in the same physical direction rotates around lateral Z in opposite direction
      return atan2f(-use_az, use_ax) * (180.0f / (float)M_PI);

    case ORIENT_Y_FWD_ROLL_POS90:
      return atan2f(use_ay, sqrtf(use_ax * use_ax + use_az * use_az)) * (180.0f / (float)M_PI);

    case ORIENT_Y_FWD_ROLL_NEG90:
      return atan2f(use_ay, sqrtf(use_ax * use_ax + use_az * use_az)) * (180.0f / (float)M_PI);

    case ORIENT_DEFAULT:
    default:
      return atan2f(-use_ax, sqrtf(use_ay * use_ay + use_az * use_az)) * (180.0f / (float)M_PI);
    }
  }

  void resetDefaultMountAxes() {
    float sz = (orientation == ORIENT_VERT_AX_DOWN_BACK || orientation == ORIENT_Y_FWD_ROLL_NEG90) ? -1.0f : 1.0f;
    yaw_axis_x = -1.0f;
    yaw_axis_y = 0.0f;
    yaw_axis_z = 0.0f;
    pitch_axis_x = 0.0f;
    pitch_axis_y = 0.0f;
    pitch_axis_z = sz;
    mountTiltDeg = 0.0f;
  }

  // Calibrate 3D Vertical Yaw Axis & Orthogonal Horizontal Pitch Axis from current gravity vector:
  // Called during zeroYawOffset() while wearer is looking straight forward dead-center.
  // Cancels out any sideways/forward skew of the neck tape or shoulder mount so turning the head
  // flat left/right produces 100% pure Yaw with ZERO Pitch cross-coupling!
  void calibrateMountAxes() {
    float gx_v = accelFilterPrimed ? ax_f : ax;
    float gy_v = accelFilterPrimed ? ay_f : ay;
    float gz_v = accelFilterPrimed ? az_f : az;

    float norm = sqrtf(gx_v * gx_v + gy_v * gy_v + gz_v * gz_v);
    if (norm < 0.25f) {
      resetDefaultMountAxes();
      return;
    }

    // Normalized true vertical gravity unit vector g_hat = (gx_v, gy_v, gz_v) in sensor frame
    gx_v /= norm;
    gy_v /= norm;
    gz_v /= norm;

    // 1. True vertical Yaw axis (aligned with gravity vector):
    yaw_axis_x = -gx_v;
    yaw_axis_y = -gy_v;
    yaw_axis_z = -gz_v;

    // 2. True horizontal Pitch axis (Gram-Schmidt orthogonalized against gravity g_hat):
    //    Guarantees (pitch_axis . yaw_axis == 0) so any flat horizontal head rotation around
    //    the vertical axis projects 0.0 onto Pitch!
    float sz = (orientation == ORIENT_VERT_AX_DOWN_BACK || orientation == ORIENT_Y_FWD_ROLL_NEG90) ? -1.0f : 1.0f;
    float px = -sz * gz_v * gx_v;
    float py = -sz * gz_v * gy_v;
    float pz =  sz * (1.0f - gz_v * gz_v);
    float pnorm = sqrtf(px * px + py * py + pz * pz);

    if (pnorm > 1e-4f) {
      pitch_axis_x = px / pnorm;
      pitch_axis_y = py / pnorm;
      pitch_axis_z = pz / pnorm;
    } else {
      pitch_axis_x = -sz;
      pitch_axis_y = 0.0f;
      pitch_axis_z = 0.0f;
    }

    // Compute total mount skew angle from ideal vertical (+X) for diagnostics
    float clampedGx = constrain(fabsf(gx_v), 0.0f, 1.0f);
    mountTiltDeg = acosf(clampedGx) * (180.0f / (float)M_PI);
    pitch_offset = calculateRawAccelPitch();
  }

  // Update attitude filter (dt in seconds)
  void updateOrientation(float dt, float alpha = COMPLEMENTARY_ALPHA) {
    if (!readRawData()) return;

    // 1. Bias-corrected 3-axis gyro rates
    float dgx = gx - gx_bias;
    float dgy = gy - gy_bias;
    float dgz = gz - gz_bias;

    // 2. Stationary Auto-Bias Learning (Zero-Velocity Update / ZUPT):
    //    When the sensor is held still (gyro rate < 1.25 dps and accel norm ~ 1.0g),
    //    continuously track and cancel thermal gyro drift in real time!
    float gyroMag = sqrtf(dgx * dgx + dgy * dgy + dgz * dgz);
    float accNorm = sqrtf(ax * ax + ay * ay + az * az);
    float accDev = fabsf(accNorm - 1.0f);

    if (gyroMag < GYRO_STILL_THRESHOLD_DPS && accDev < 0.08f) {
      stationaryTimeSec += dt;
      if (stationaryTimeSec >= 0.30f) {
        // Sensor is confirmed stationary: gently adapt gyro bias toward current resting reading
        gx_bias += GYRO_BIAS_LEARN_RATE * (gx - gx_bias);
        gy_bias += GYRO_BIAS_LEARN_RATE * (gy - gy_bias);
        gz_bias += GYRO_BIAS_LEARN_RATE * (gz - gz_bias);
        dgx = gx - gx_bias;
        dgy = gy - gy_bias;
        dgz = gz - gz_bias;
      }
    } else {
      stationaryTimeSec = 0.0f;
    }

    // 3. Project 3D body-frame gyro vector (dgx, dgy, dgz) onto the calibrated
    //    3D orthonormal Yaw (vertical) and Pitch (horizontal) axes:
    //    Eliminates pitch cross-coupling when neck tape is angled/tilted!
    float gyroYawRate   = (dgx * yaw_axis_x)   + (dgy * yaw_axis_y)   + (dgz * yaw_axis_z);
    float gyroPitchRate = (dgx * pitch_axis_x) + (dgy * pitch_axis_y) + (dgz * pitch_axis_z);

    // 4. Smooth Soft Deadzone on Yaw & Pitch Gyro Rates:
    //    Prevents micro-noise from integrating into yaw creep when holding head still,
    //    while ramping smoothly above the deadzone with zero step jump.
    if (fabsf(gyroYawRate) <= GYRO_DEADZONE_DPS) {
      gyroYawRate = 0.0f;
    } else {
      gyroYawRate = (gyroYawRate > 0.0f) ? (gyroYawRate - GYRO_DEADZONE_DPS)
                                         : (gyroYawRate + GYRO_DEADZONE_DPS);
    }

    if (fabsf(gyroPitchRate) <= (GYRO_DEADZONE_DPS * 0.5f)) {
      gyroPitchRate = 0.0f;
    }

    // 5. 100% Pure 3D Gravity-Aligned Gyro Integration for both Pitch and Yaw:
    pitch += gyroPitchRate * dt;
    yaw   += gyroYawRate * dt;
  }

  void zeroYaw() {
    yaw = 0.0f;
  }

  bool readRegister(uint8_t reg, uint8_t &val) {
    wire->beginTransmission(address);
    wire->write(reg);
    if (wire->endTransmission(false) != 0) {
      return false;
    }
    if (wire->requestFrom((uint8_t)address, (uint8_t)1, (uint8_t)true) != 1) {
      return false;
    }
    val = wire->read();
    return true;
  }

  void writeRegister(uint8_t reg, uint8_t val) {
    wire->beginTransmission(address);
    wire->write(reg);
    wire->write(val);
    wire->endTransmission(true);
  }
};
