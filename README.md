# 🤖 2-Axis Shoulder-Mounted Gimbal (Head-Tracking)

Standalone PlatformIO firmware for an **ESP32-C3 SuperMini** controlling a 2-axis shoulder-mounted gimbal weapon/camera pod with **dual MPU6050 IMU head tracking**.

The gimbal mimics head orientation relative to the body in real time while compensating for body motion, drift, and mechanical clearance constraints.

---

## 📐 Kinematics & Sequential Folding / Deploying

* **Neutral Deployed Position `(0°, 0°)`**:
  * **Yaw Servo**: `90°`
  * **Pitch Servo**: `90°`
  * Head tracking is active.

* **Folded (Stowed) Position**:
  * **Yaw Servo**: `0°`
  * **Pitch Servo**: `0°`
  * Automatic holding torque release after stowing prevents motor hum and battery drain.

### Motion Sequences:
1. **Deploy Sequence** (`CMD_DEPLOY` or Serial `'d'`):
   * **Step 1**: Pitch servo unfolds first (`0°` $\rightarrow$ `90°`).
   * **Step 2**: Yaw servo unfolds second (`0°` $\rightarrow$ `90°`).
   * **Auto-Zero**: Instantly re-zeroes relative yaw reference and enters real-time head-tracking mode.
2. **Fold Sequence** (`CMD_DISARM` or Serial `'f'`):
   * **Step 1**: Yaw servo folds back first to stowed alignment (`current` $\rightarrow$ `0°`).
   * **Step 2**: Pitch servo rolls back second into stowed position (`current` $\rightarrow$ `0°`).

---

## 🎯 Dual-IMU Head Tracking & Drift Calibration

| Sensor | I2C Address | Physical Mounting Orientation | Role |
| :--- | :--- | :--- | :--- |
| **Neck IMU** | `0x69` (AD0 = 3.3V) | Facing front with `+Y`, rolled `-90°` around `+Y` | Measures absolute head motion |
| **Shoulder IMU** | `0x68` (AD0 = GND) | Facing front with `+Y`, rolled `+90°` around `+Y` | Measures body reference pose |

* **Axis Mapping for $Y+$ Forward Mounting**:
  * **Pitch (nodding)**: Rotates around the lateral Z axis (`gz`).
  * **Yaw (turning)**: Rotates around the vertical X axis (`gx`).
* **Relative Pitch**: Computed from accelerometer gravity vectors + complementary gyro integration:
  $$\text{Relative Pitch} = \text{Neck Pitch} - \text{Shoulder Pitch}$$
  Since gravity references true earth-vertical, pitch has zero drift.
* **Relative Yaw**: Integrated from vertical-axis gyros:
  $$\text{Relative Yaw} = (\text{Neck Yaw} - \text{Shoulder Yaw}) - \text{Yaw Zero Offset}$$
* **Yaw Drift Elimination (ESP-NOW Calibration)**:
  * When you press the calibrate button on your master controller (`CMD_CALIBRATE`), the firmware instantly re-zeroes the yaw offset with your head facing forward.
* **Ground Calibration (USB Serial `'g'`)**:
  * Connect to a PC via USB.
  * Keep the sensors stationary on a desk and type `'g'` in the Serial Monitor.
  * Measures gyro drift biases and accelerometer level pitch offsets, then **saves them permanently to Flash NVS** (Preferences).

---

## 📋 Hardware Setup & Pinout (ESP32-C3 SuperMini)

| Peripheral | ESP32-C3 Pin | Details |
| :--- | :--- | :--- |
| **Fire ESC (Electromagnet)** | `GPIO 0` | 14-Bit Native LEDC Timer (Channel 2, 50Hz, Idle: 544µs, Fire: 2200µs) |
| **Yaw Servo PWM** | `GPIO 1` | 14-Bit Native LEDC Timer (Channel 1, 50Hz) |
| **Pitch Servo PWM** | `GPIO 3` | 14-Bit Native LEDC Timer (Channel 0, 50Hz) |
| **I2C SDA** | `GPIO 8` | Shared I2C Data line (400kHz Fast Mode) |
| **I2C SCL** | `GPIO 9` | Shared I2C Clock line |
| **Power (+5V / GND)** | `5V` & `GND` | Common high-current power rail for servos & ESP32 |

> [!NOTE]
> Active buzzer has been removed entirely as requested.

---

## 📡 Wireless Protocol (ESP-NOW)

* **Mode**: ESP-NOW Slave (Station mode, Wi-Fi Channel 1)
* **Master**: Webshooter / Hand Controller (Unicast or Broadcast)

### Commands:
* `CMD_DEPLOY` (`2`): Executes Pitch-first, Yaw-second deploy sequence and engages live tracking.
* `CMD_DISARM` (`3`): Executes Yaw-first, Pitch-second fold sequence into `(0°, 0°)`.
* `CMD_CALIBRATE` (`1`): Re-zeroes relative yaw offset while facing forward.

---

## 💻 USB Serial Command Interface

Open the Serial Monitor at **115200 baud**:
* `'d'`: Trigger Deploy sequence.
* `'f'`: Trigger Fold sequence.
* `'c'`: Quick Yaw re-zero (same as ESP-NOW calibrate).
* `'g'`: Run Ground Calibration (measures biases and saves to NVS).
* `'t'`: Toggle live real-time telemetry stream (prints angles at 10Hz).
* `'h'`: Display help menu.

---

## 🔨 Building & Flashing

```bash
# Build firmware
pio run

# Flash to ESP32-C3 SuperMini over USB
pio run --target upload

# Open Serial Monitor
pio device monitor
```
