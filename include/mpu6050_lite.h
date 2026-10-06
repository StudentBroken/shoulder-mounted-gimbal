#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

// ==============================================================================
// Lightweight, High-Speed MPU6050 Driver with Flexible 3D Mounting Orientation
// ==============================================================================

#define MPU6050_REG_SMPLRT_DIV   0x19
#define MPU6050_REG_CONFIG       0x1A
#define MPU6050_REG_GYRO_CONFIG  0x1B
#define MPU6050_REG_ACCEL_CONFIG 0x1C
#define MPU6050_REG_ACCEL_XOUT_H 0x3B
#define MPU6050_REG_PWR_MGMT_1   0x6B
#define MPU6050_REG_WHO_AM_I     0x75

enum ImuOrientation {
  ORIENT_DEFAULT,           // Standard flat: X forward, Y left, Z up
  ORIENT_Y_FWD_ROLL_POS90,  // +Y forward, rolled +90° around +Y (Shoulder mount)
  ORIENT_Y_FWD_ROLL_NEG90   // +Y forward, rolled -90° around +Y (Head mount)
};

class Mpu6050Lite {
public:
  uint8_t address = 0x68;
  TwoWire* wire = &Wire;
  bool isConnected = false;
  ImuOrientation orientation = ORIENT_DEFAULT;

  // Raw and converted values (in sensor frame)
  float ax = 0, ay = 0, az = 0; // Accelerometer in g
  float gx = 0, gy = 0, gz = 0; // Gyroscope in deg/s

  // Gyro biases for all 3 axes (calibrated on ground)
  float gx_bias = 0.0f;
  float gy_bias = 0.0f;
  float gz_bias = 0.0f;
  float pitch_offset = 0.0f; // Accelerometer resting pitch offset

  // Filtered Euler Angles
  float pitch = 0.0f;
  float yaw = 0.0f;

  bool begin(uint8_t i2cAddr, ImuOrientation orient = ORIENT_DEFAULT, TwoWire* wireInstance = &Wire) {
    address = i2cAddr;
    orientation = orient;
    wire = wireInstance;

    // Check WHO_AM_I register
    wire->beginTransmission(address);
    wire->write(MPU6050_REG_WHO_AM_I);
    uint8_t err = wire->endTransmission(true);
    if (err != 0) {
      Serial.printf("[IMU 0x%02X] I2C error %d (NACK/not found) on WHO_AM_I transmission.\n", address, err);
      isConnected = false;
      return false;
    }

    uint8_t bytes = wire->requestFrom((uint8_t)address, (uint8_t)1, (uint8_t)true);
    if (bytes == 0 || !wire->available()) {
      Serial.printf("[IMU 0x%02X] ERROR: No response to requestFrom (check wiring/power)!\n", address);
      isConnected = false;
      return false;
    }

    uint8_t whoami = wire->read();
    Serial.printf("[IMU 0x%02X] Detected WHO_AM_I = 0x%02X\n", address, whoami);
    if (whoami != 0x68 && whoami != 0x70 && whoami != 0x72 && whoami != 0x98) {
      Serial.printf("[IMU 0x%02X] Warning: Unexpected WHO_AM_I (expected 0x68)\n", address);
    }

    // 1. Wake up device, use PLL with X gyro clock reference (0x01)
    writeRegister(MPU6050_REG_PWR_MGMT_1, 0x01);
    delay(10);

    // 2. DLPF Configuration: 42Hz DLPF bandwidth (smooth, low jitter)
    writeRegister(MPU6050_REG_CONFIG, 0x03);

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

    wire->beginTransmission(address);
    wire->write(MPU6050_REG_ACCEL_XOUT_H);
    if (wire->endTransmission(true) != 0) {
      return false;
    }

    // Read 14 consecutive registers:
    // Accel X (2), Accel Y (2), Accel Z (2), Temp (2), Gyro X (2), Gyro Y (2), Gyro Z (2)
    uint8_t bytesReceived = wire->requestFrom((uint8_t)address, (uint8_t)14, (uint8_t)true);
    if (bytesReceived < 14) return false;

    int16_t raw_ax = (wire->read() << 8) | wire->read();
    int16_t raw_ay = (wire->read() << 8) | wire->read();
    int16_t raw_az = (wire->read() << 8) | wire->read();
    wire->read(); wire->read(); // Skip temperature
    int16_t raw_gx = (wire->read() << 8) | wire->read();
    int16_t raw_gy = (wire->read() << 8) | wire->read();
    int16_t raw_gz = (wire->read() << 8) | wire->read();

    // Scale factors: ±4g = 8192 LSB/g, ±500 dps = 65.5 LSB/(deg/s)
    ax = (float)raw_ax / 8192.0f;
    ay = (float)raw_ay / 8192.0f;
    az = (float)raw_az / 8192.0f;

    gx = (float)raw_gx / 65.5f;
    gy = (float)raw_gy / 65.5f;
    gz = (float)raw_gz / 65.5f;

    return true;
  }

  // Calculate pitch from accelerometer based on physical mounting orientation
  float calculateRawAccelPitch() {
    switch (orientation) {
    case ORIENT_Y_FWD_ROLL_POS90:
      // +Y is Forward. Rolled +90° around Y -> vertical axis is -X, lateral axis is +Z.
      return atan2f(ay, sqrtf(ax * ax + az * az)) * (180.0f / (float)M_PI);

    case ORIENT_Y_FWD_ROLL_NEG90:
      // +Y is Forward. Rolled -90° around Y -> vertical axis is +X, lateral axis is -Z.
      return atan2f(ay, sqrtf(ax * ax + az * az)) * (180.0f / (float)M_PI);

    case ORIENT_DEFAULT:
    default:
      return atan2f(-ax, sqrtf(ay * ay + az * az)) * (180.0f / (float)M_PI);
    }
  }

  // Update attitude filter (dt in seconds)
  void updateOrientation(float dt, float alpha = 0.96f) {
    if (!readRawData()) return;

    float gyroPitchRate = 0.0f;
    float gyroYawRate = 0.0f;

    switch (orientation) {
    case ORIENT_Y_FWD_ROLL_POS90:
      // +Y forward, rolled +90° around Y (Shoulder mount):
      // Pitch rotation (nodding forward/backward) rotates around lateral Z axis
      gyroPitchRate = (gz - gz_bias);
      // Yaw rotation (turning head/body left/right) rotates around vertical X axis
      gyroYawRate = (gx - gx_bias);
      break;

    case ORIENT_Y_FWD_ROLL_NEG90:
      // +Y forward, rolled -90° around Y (Head mount):
      // Lateral Z axis is opposite to +90° roll
      gyroPitchRate = -(gz - gz_bias);
      // Vertical X axis is opposite to +90° roll
      gyroYawRate = -(gx - gx_bias);
      break;

    case ORIENT_DEFAULT:
    default:
      gyroPitchRate = (gy - gy_bias);
      gyroYawRate = (gz - gz_bias);
      break;
    }

    // Accelerometer pitch with zero offset
    float accPitch = calculateRawAccelPitch() - pitch_offset;

    // Complementary filter for pitch
    pitch = alpha * (pitch + gyroPitchRate * dt) + (1.0f - alpha) * accPitch;

    // Yaw integration
    yaw += gyroYawRate * dt;
  }

  void zeroYaw() {
    yaw = 0.0f;
  }

  void writeRegister(uint8_t reg, uint8_t val) {
    wire->beginTransmission(address);
    wire->write(reg);
    wire->write(val);
    wire->endTransmission();
  }
};
