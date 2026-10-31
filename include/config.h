#pragma once
#include <Arduino.h>

// ==============================================================================
// Shoulder-Mounted Gimbal Controller - Configuration
// Board: ESP32-C3 SuperMini
// ==============================================================================

// ------------------------------------------------------------------------------
// Hardware Pinout (ESP32-C3 SuperMini)
// ------------------------------------------------------------------------------
#define PIN_FIRE_ESC        0   // GPIO 0: 25W Electromagnet Brushed ESC (LEDC Channel 2)
#define PIN_SERVO_YAW       1   // GPIO 1: Gimbal Yaw Servo (LEDC Channel 1)
#define PIN_SERVO_PITCH     3   // GPIO 3: Gimbal Pitch Servo (LEDC Channel 0)
#define PIN_I2C_SDA         8   // GPIO 8: I2C Data line (Shared bus for both IMUs)
#define PIN_I2C_SCL         9   // GPIO 9: I2C Clock line
#define PIN_RGB_LED         21  // GPIO 21: Dual WS2812 Addressable RGB LEDs (Left & Right barrel)
#define NUM_BARREL_LEDS     2   // 2 Chained Pixels: Pixel 0 = Left Barrel, Pixel 1 = Right Barrel

#define PIN_BATTERY_ADC     4   // GPIO 4: Battery Voltage monitoring (ADC1_CHANNEL_4, 100k-100k divider)
#define BATTERY_DIVIDER_RATIO 2.0f // 100k:100k divider ratio (V_bat = V_pin * 2.0)
#define BATTERY_SAMPLE_INTERVAL_MS 200 // ADC polling rate

#define RGB_MIN_BRIGHTNESS   12  // Floor brightness (approx 5%): never goes to 0%, always glowing
#define RGB_MAX_BRIGHTNESS   220 // Maximum brightness clamp (0-255)
#define RGB_BREATH_PERIOD_MS 3600 // 3.6s Apple-style breathing cycle (16.6 breaths/min)

// ------------------------------------------------------------------------------
// Wireless App Mode (BLE Web Bluetooth vs Direct Wi-Fi SoftAP)
//   - COMM_MODE_BLE  (0): Default! Advertises BLE GATT for leds.html (Wi-Fi SoftAP OFF, ESP-NOW ON)
//   - COMM_MODE_WIFI (1): Direct Wi-Fi SoftAP ("Gimbal-Tactical-Pod", http://192.168.4.1) + BLE + ESP-NOW
// ------------------------------------------------------------------------------
#define DEFAULT_COMM_MODE   0           // 0 = COMM_MODE_BLE (default), 1 = COMM_MODE_WIFI
#define WIFI_AP_SSID        "Gimbal-Tactical-Pod"
#define WIFI_AP_PASS        "12345678"  // WPA2 Password for reliable beaconing & phone discovery
#define WIFI_AP_CHANNEL     1           // Fixed to Channel 1 (Ensures ESP-NOW coexistence)
#define WEB_SERVER_PORT     80

// ------------------------------------------------------------------------------
// 25W Electromagnet Firing System (GPIO 0 Single-Direction Brushed ESC)
// ------------------------------------------------------------------------------
// Operates on 50Hz standard RC pulses:
//   Idle:   544µs (0% throttle -> ESC armed and safe, 0V output)
//   Active: 2200µs (100% throttle -> Full power to electromagnet)
#define FIRE_LEDC_CHANNEL        2      // Dedicated LEDC Timer Channel 2
#define FIRE_PWM_IDLE_US         544    // 0% Throttle / Idle
#define FIRE_PWM_ACTIVE_US       2450   // 100% Throttle / Energize (Full Saturation Power)
#define FIRE_MAX_DURATION_MS     8000   // 8.0s maximum safety cutoff to protect electromagnet coil


// ------------------------------------------------------------------------------
// Dual 6-DOF IMU Configuration (Auto-Detects Bosch GY-BMI160 & MPU6050)
// ------------------------------------------------------------------------------
#define IMU_ADDR_SHOULDER   0x68 // IMU mounted on shoulder (SAO/AD0 tied to GND)
#define IMU_ADDR_NECK       0x69 // IMU mounted on neck/head (SAO/AD0 tied to 3.3V)

#define I2C_CLOCK_SPEED_HZ  100000 // 100kHz Standard-mode I2C (robust against harness cable capacitance)
#define IMU_SAMPLE_RATE_HZ  50     // 50Hz sensor fusion and servo update rate (20ms loop)
#define COMPLEMENTARY_ALPHA 0.985f // Complementary filter weight (0.985 gyro / 0.015 gravity-gated accel)

// Anti-Drift & Anti-Jitter Sensor Fusion Tuning:
#define GYRO_DEADZONE_DPS         0.30f  // Smooth yaw rate deadzone (deg/s) to stop stationary micro-creep
#define GYRO_STILL_THRESHOLD_DPS  1.25f  // Angular rate threshold (deg/s) for stationary auto-bias learning
#define GYRO_BIAS_LEARN_RATE      0.025f // Continuous stationary gyro bias adaptation speed (eliminates thermal drift)
#define SERVO_DEADBAND_DEG        0.22f  // Sub-degree PWM hysteresis to stop servo hunting/buzzing at rest

// Invert tracking directions if your servo mounting or IMU orientation is flipped:
#define YAW_DIRECTION_SIGN    -1.0f  // +1.0f = normal, -1.0f = inverted (Normal for 3D gravity-aligned yaw)
#define PITCH_DIRECTION_SIGN  -1.0f  // +1.0f = normal, -1.0f = inverted (Normal for pitch tilt)

// ------------------------------------------------------------------------------
// Servo Geometry & Travel Limits
// ------------------------------------------------------------------------------
// Fully Folded (Stowed) Position:
#define SERVO_YAW_FOLDED_DEG      0.0f   // Yaw folds flush at 0°
#define SERVO_PITCH_FOLDED_DEG    0.0f   // Pitch folds flat at 0°

// Fully Deployed Neutral Position (Relative head facing 0°, 0°):
#define SERVO_YAW_DEPLOYED_DEG    90.0f  // Neutral Yaw center = 90°
#define SERVO_PITCH_DEPLOYED_DEG  110.0f // Neutral Pitch center = 110° (mechanical level center)

// Absolute Mechanical Travel Limits (Prevents binding & motor stall)
#define SERVO_YAW_MIN_DEG         0.0f
#define SERVO_YAW_MAX_DEG         180.0f
#define SERVO_PITCH_MIN_DEG       0.0f
#define SERVO_PITCH_MAX_DEG       175.0f

// Standard 50Hz RC Servo Pulse Widths:
#define SERVO_MIN_PULSE_US        544   // Pulse width for 0 degrees (µs)
#define SERVO_MAX_PULSE_US        2400  // Pulse width for 180 degrees (µs)
#define SERVO_LEDC_FREQ           50    // 50Hz (20ms RC period)
#define SERVO_LEDC_RESOLUTION     14    // 14-bit timer (0 to 16383 counts)
#define SERVO_PITCH_CHANNEL       0     // LEDC Channel 0 (GPIO 3)
#define SERVO_YAW_CHANNEL         1     // LEDC Channel 1 (GPIO 1)

// Auto-Release PWM: releases holding torque only after folding to prevent hum & drain
#define SERVO_AUTO_RELEASE_MS     800   // ms after folding before cutting PWM (0 = keep torque)

// ------------------------------------------------------------------------------
// Simultaneous Deploy & Fold Timings (in Milliseconds)
// ------------------------------------------------------------------------------
// Simultaneous Deploy: Pitch and Yaw unfold together, slowly chasing live head target
#define DEPLOY_DURATION_MS        850   // Simultaneous deploy duration (slow, smooth chase)
#define DEPLOY_PITCH_TIME_MS      DEPLOY_DURATION_MS
#define DEPLOY_YAW_TIME_MS        DEPLOY_DURATION_MS

// Simultaneous Fold: Pitch (-> 0°) and Yaw (-> 180°) retract together smoothly
#define FOLD_DURATION_MS          650   // Simultaneous fold duration
#define FOLD_YAW_TIME_MS          FOLD_DURATION_MS
#define FOLD_PITCH_TIME_MS        FOLD_DURATION_MS

// Motion Profile: 1 = Iron Man S-curve overshoot, 2 = SmootherStep, 3 = Cubic
#define DEPLOY_MOTION_PROFILE     2     // Smootherstep (buttery fluid 5th-order polynomial)

// ------------------------------------------------------------------------------
// Active Head Tracking Smoothing
// ------------------------------------------------------------------------------
// Exponential moving average filter factor (0.05 to 0.40):
// Higher = more responsive / snaps faster; Lower = smoother, cinematic dampening
#define TRACKING_SMOOTH_ALPHA     0.22f

// Maximum servo speed in tracking mode (deg/sec limit to prevent violent jerks)
#define TRACKING_MAX_SPEED_DPS    280.0f

// ------------------------------------------------------------------------------
// Serial Telemetry & Ground Calibration
// ------------------------------------------------------------------------------
#define SERIAL_BAUD_RATE          115200
#define NVS_NAMESPACE             "gimbal"
