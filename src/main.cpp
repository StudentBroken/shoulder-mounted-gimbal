#include "config.h"
#include "esp_now_protocol.h"
#include "mpu6050_lite.h"
#include "web_ui.h"
#include <esp32-hal-rgb-led.h>
#include <esp32-hal-rmt.h>
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>
#include <math.h>

static esp_adc_cal_characteristics_t adc1_chars;
static bool adc1_cal_initialized = false;

uint32_t getBatteryPinMv() {
  int raw = adc1_get_raw(ADC1_CHANNEL_4);
  if (raw < 0) raw = 0;
  if (adc1_cal_initialized) {
    return esp_adc_cal_raw_to_voltage(raw, &adc1_chars);
  }
  return analogReadMilliVolts(PIN_BATTERY_ADC);
}

// ==============================================================================
// Hardware Objects & Global State
// ==============================================================================
Preferences prefs;
Mpu6050Lite imuShoulder; // Address 0x68 (Body reference: Y+ fwd, +90° roll)
Mpu6050Lite imuNeck;     // Address 0x69 (Head tracker:   Y+ fwd, -90° roll)

// Gimbal Motion States
enum GimbalState {
  GIMBAL_FOLDED,     // Stowed at (0°, 0°)
  GIMBAL_DEPLOYING,  // Simultaneous unfolding (Pitch + Yaw slowly chasing live head position)
  GIMBAL_TRACKING,   // Fully deployed: Real-time head tracking
  GIMBAL_FOLDING     // Simultaneous retracting (Pitch -> 0°, Yaw -> 0°)
};

GimbalState currentState = GIMBAL_FOLDED;

// Current and Target Servo Angles (degrees)
float currentYaw = SERVO_YAW_FOLDED_DEG;
float currentPitch = SERVO_PITCH_FOLDED_DEG;
float lastCommittedYaw = -999.0f;
float lastCommittedPitch = -999.0f;
float startSeqYaw = SERVO_YAW_FOLDED_DEG;
float startSeqPitch = SERVO_PITCH_FOLDED_DEG;
float targetSeqYaw = SERVO_YAW_FOLDED_DEG;
float targetSeqPitch = SERVO_PITCH_FOLDED_DEG;

// Sequence Timing
unsigned long seqStartTime = 0;
unsigned long seqDurationMs = 0;
unsigned long foldCompleteTime = 0;
bool pwmActive = false;

// 25W Electromagnet Firing State
bool isFiring = false;
unsigned long fireStartTime = 0;

// Manual RC Control Mode
bool rcModeActive = false;
float rcTargetYaw = SERVO_YAW_DEPLOYED_DEG;   // 90.0° center
float rcTargetPitch = SERVO_PITCH_DEPLOYED_DEG; // 110.0° center

// Head Tracking and Zero Drift Compensation
bool hasBeenZeroed = false;           // Safety interlock: Gimbal will NOT arm/deploy unless zeroed after boot
float relativeYawZeroOffset = 0.0f;   // Calibrated when head faces straight forward
float relativePitchZeroOffset = 0.0f; // Calibrated when head faces straight forward (horizon)
float lastRelativeYaw = 0.0f;
float lastRelativePitch = 0.0f;
float filteredTargetYaw = SERVO_YAW_DEPLOYED_DEG;
float filteredTargetPitch = SERVO_PITCH_DEPLOYED_DEG;

// Web Server, BLE GATT & Wireless Mode Settings
WebServer server(WEB_SERVER_PORT);
bool webServerStarted = false;
uint8_t cfgCommMode = DEFAULT_COMM_MODE; // 0 = COMM_MODE_BLE (Default), 1 = COMM_MODE_WIFI
int8_t pendingCommModeSwitch = -1;
unsigned long pendingCommSwitchAtMs = 0;
unsigned long lastWebClientActiveMs = 0;
unsigned long lastBleClientActiveMs = 0;
unsigned long lastBleNotifyMs = 0;

BLEServer *pBleServer = nullptr;
BLECharacteristic *pStatusChar = nullptr;
BLECharacteristic *pCmdChar = nullptr;
bool bleClientConnected = false;
String pendingBleCommand = "";

// Battery Monitoring (GPIO 2, 100k-100k Divider)
float gimbalVbatFilteredMv = 0.0f;
unsigned long lastBatterySampleMs = 0;

// Remote Webshooter Telemetry Cache
WebshooterTelemetry remoteWebshooter = {0};
unsigned long lastRemoteTelemetryMs = 0;

// Remote Suit LED Mesh Nodes Cache (1=Chest, 2=Back, 3=Helmet, 4=Aux)
SuitNodeTelemetry remoteLedNodes[5] = {0};
unsigned long lastLedNodeSeenMs[5] = {0};

// Dynamic LED & Kinematics Settings (Configurable via Web UI & Persisted to Flash)
uint8_t cfgStandbyR = 255, cfgStandbyG = 255, cfgStandbyB = 255;
uint8_t cfgDeployedR = 255, cfgDeployedG = 0, cfgDeployedB = 0; // Pure Red when deployed/armed
uint8_t cfgLedPattern = PATTERN_BREATH_APPLE;
uint8_t cfgLastActivePattern = PATTERN_BREATH_APPLE;
uint8_t cfgArmedPattern = PATTERN_ARMED_SOFT_FLASH; // Default: Soft Armed Red Flash when either WS or Gimbal is armed
uint8_t cfgWaveMode = WAVE_LOW_TO_HIGH;             // Default: Low (WS) -> Mid (Vest) -> High (Gimbal)
uint8_t cfgWaveOffsetPct = DEFAULT_WAVE_OFFSET_PCT; // Default: 30% phase offset per vertical tier
uint8_t cfgAppStateOverride = 0xFF;                 // 0xFF = Auto (follow suit), 0=Standby, 1=Armed, 2=Firing
bool cfgStealthMode = false; // Suit-wide Stealth Mode (synced with Webshooter button hold & Web UI)

uint8_t cfgMinBrightness = RGB_MIN_BRIGHTNESS;
uint8_t cfgMaxBrightness = RGB_MAX_BRIGHTNESS;
uint16_t cfgBreathPeriodMs = RGB_BREATH_PERIOD_MS;
uint16_t cfgArmedPeriodMs = DEFAULT_ARMED_PERIOD_MS;

float cfgTrackingAlpha = TRACKING_SMOOTH_ALPHA;
float cfgMaxSpeedDps = TRACKING_MAX_SPEED_DPS;
uint16_t cfgDeployDurationMs = DEPLOY_DURATION_MS;
uint16_t cfgFoldDurationMs = FOLD_DURATION_MS;

void setupWebServer();
void initBleGattServer();
void applyWirelessCommMode(uint8_t mode, bool saveToNvs = true);
String buildBleStatusJson();
void handleBleCommandString(const String &cmdStr);
void updateBatteryAdc();
void broadcastLedSync();

// Loop Timers
unsigned long lastLoopTimeUs = 0;
unsigned long lastTelemetryTimeMs = 0;
unsigned long lastLedSyncBroadcastMs = 0;
uint32_t ledSyncSequence = 0;
bool telemetryStreamActive = false;
bool rawDiagStreamActive = false; // Live raw IMU 6-axis gravity orientation diagnostic (toggle via 'm')

// ==============================================================================
// Function Prototypes
// ==============================================================================
void initServosAndFireEsc();
void writeYawServo(float angle);
void writePitchServo(float angle);
void releaseServos();
void writeFirePulseUs(uint32_t pulse_us);

void initI2CAndIMUs();
void loadCalibrationFromNVS();
void saveCalibrationToNVS();
void runGroundCalibration();
void zeroYawOffset();

void updateRgbLed();
void ws2812WriteBoth(uint8_t r, uint8_t g, uint8_t b);
void triggerRgbFlash(uint8_t r, uint8_t g, uint8_t b, uint16_t duration_ms);

void startDeploySequence();
void startFoldSequence();
void updateGimbalStateMachine(float dt);
void updateTracking(float dt);
void updateRcControl(float dt);

void handleCmdFire(bool force = false);
void handleCmdStopFire();

void onEspNowRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len);
void processSerialCommands();
float smootherStep(float t);

// ==============================================================================
// ESP-NOW Receive Callback (Gimbal Hub: Receives Webshooter & Suit LED Nodes)
// ==============================================================================
void onEspNowRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
  if (len == sizeof(WebshooterTelemetry) && incomingData[0] == CMD_TELEMETRY) {
    memcpy(&remoteWebshooter, incomingData, sizeof(WebshooterTelemetry));
    lastRemoteTelemetryMs = millis();
    return;
  }

  if (len == sizeof(SuitNodeTelemetry) && incomingData[0] == CMD_LED_NODE_TELEM) {
    SuitNodeTelemetry nodeMsg;
    memcpy(&nodeMsg, incomingData, sizeof(SuitNodeTelemetry));
    if (nodeMsg.node_id >= 1 && nodeMsg.node_id <= 4) {
      remoteLedNodes[nodeMsg.node_id] = nodeMsg;
      lastLedNodeSeenMs[nodeMsg.node_id] = millis();
    }
    return;
  }

  // Allow LED Sync updates forwarded from a BLE-connected Suit-LED-Vest node
  if (len == sizeof(SuitLedSyncMessage) && incomingData[0] == CMD_LED_SYNC) {
    SuitLedSyncMessage syncMsg;
    memcpy(&syncMsg, incomingData, sizeof(SuitLedSyncMessage));
    cfgStandbyR = syncMsg.standby_r;
    cfgStandbyG = syncMsg.standby_g;
    cfgStandbyB = syncMsg.standby_b;
    cfgDeployedR = syncMsg.deployed_r;
    cfgDeployedG = syncMsg.deployed_g;
    cfgDeployedB = syncMsg.deployed_b;
    cfgLedPattern = syncMsg.pattern;
    cfgStealthMode = (syncMsg.stealth_mode != 0) || (cfgLedPattern == PATTERN_STEALTH_OFF);
    cfgArmedPattern = syncMsg.armed_pattern;
    cfgWaveMode = syncMsg.wave_mode;
    cfgWaveOffsetPct = constrain(syncMsg.wave_offset_pct, 0, 50);
    cfgMinBrightness = syncMsg.min_brightness;
    cfgMaxBrightness = syncMsg.max_brightness;
    cfgBreathPeriodMs = (syncMsg.breath_period_ms >= 300) ? syncMsg.breath_period_ms : 3600;
    cfgArmedPeriodMs = (syncMsg.armed_period_ms >= 250) ? syncMsg.armed_period_ms : DEFAULT_ARMED_PERIOD_MS;
    return;
  }

  if (len == sizeof(GimbalMessage)) {
    GimbalMessage msg;
    memcpy(&msg, incomingData, sizeof(msg));

    switch (msg.command) {
    case CMD_DEPLOY:
      Serial.println("\n[ESP-NOW] CMD_DEPLOY received -> Initiating deploy sequence...");
      startDeploySequence();
      break;

    case CMD_DISARM:
      Serial.println("\n[ESP-NOW] CMD_DISARM received -> Initiating fold sequence...");
      handleCmdStopFire();
      startFoldSequence();
      break;

    case CMD_CALIBRATE:
      // Can be zeroed anytime while disarmed (folded or folding)
      if (currentState == GIMBAL_FOLDED || currentState == GIMBAL_FOLDING) {
        Serial.println("\n[ESP-NOW] CMD_CALIBRATE received -> Re-zeroing relative Yaw (disarmed)...");
        zeroYawOffset();
      } else {
        Serial.println("\n[ESP-NOW] CMD_CALIBRATE ignored: Gimbal is armed/active. Disarm first to re-zero.");
      }
      break;

    case CMD_FIRE:
      Serial.println("\n[ESP-NOW] CMD_FIRE received -> Energizing electromagnet...");
      handleCmdFire();
      break;

    case CMD_STOP_FIRE:
      Serial.println("\n[ESP-NOW] CMD_STOP_FIRE received -> Electromagnet idle.");
      handleCmdStopFire();
      break;

    case CMD_STEALTH_ON:
      Serial.println("\n[ESP-NOW] CMD_STEALTH_ON received from Webshooter -> Suit Stealth Mode ON (All LEDs OFF)!");
      cfgStealthMode = true;
      if (cfgLedPattern != PATTERN_STEALTH_OFF) {
        cfgLastActivePattern = cfgLedPattern;
      }
      cfgLedPattern = PATTERN_STEALTH_OFF;
      prefs.begin(NVS_NAMESPACE, false);
      prefs.putBool("stealth", true);
      prefs.putUChar("pattern", cfgLedPattern);
      prefs.end();
      ws2812WriteBoth(0, 0, 0);
      broadcastLedSync();
      break;

    case CMD_STEALTH_OFF:
      Serial.println("\n[ESP-NOW] CMD_STEALTH_OFF received from Webshooter -> Suit Stealth Mode OFF (All LEDs ON)!");
      cfgStealthMode = false;
      if (cfgLedPattern == PATTERN_STEALTH_OFF) {
        cfgLedPattern = (cfgLastActivePattern != PATTERN_STEALTH_OFF) ? cfgLastActivePattern : PATTERN_BREATH_APPLE;
      }
      prefs.begin(NVS_NAMESPACE, false);
      prefs.putBool("stealth", false);
      prefs.putUChar("pattern", cfgLedPattern);
      prefs.end();
      broadcastLedSync();
      break;

    default:
      break;
    }
  }
}

// ==============================================================================
// Apple-Style WS2812 RGB LED Engine (GPIO 21)
// Non-linear perceptual eye smoothness via CIE 1931 lightness curve + exp(sin(t))
// ==============================================================================
const uint8_t cie1931_table[256] PROGMEM = {
    0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,
    2,   2,   2,   2,   2,   2,   2,   3,   3,   3,   3,   3,   3,   3,   3,   4,
    4,   4,   4,   4,   4,   5,   5,   5,   5,   5,   6,   6,   6,   6,   6,   7,
    7,   7,   7,   8,   8,   8,   8,   9,   9,   9,  10,  10,  10,  10,  11,  11,
   11,  12,  12,  12,  13,  13,  13,  14,  14,  15,  15,  15,  16,  16,  17,  17,
   17,  18,  18,  19,  19,  20,  20,  21,  21,  22,  22,  23,  23,  24,  24,  25,
   25,  26,  26,  27,  28,  28,  29,  29,  30,  31,  31,  32,  32,  33,  34,  34,
   35,  36,  37,  37,  38,  39,  39,  40,  41,  42,  43,  43,  44,  45,  46,  47,
   47,  48,  49,  50,  51,  52,  53,  54,  54,  55,  56,  57,  58,  59,  60,  61,
   62,  63,  64,  65,  66,  67,  68,  70,  71,  72,  73,  74,  75,  76,  77,  79,
   80,  81,  82,  83,  85,  86,  87,  88,  90,  91,  92,  94,  95,  96,  98,  99,
  100, 102, 103, 105, 106, 108, 109, 110, 112, 113, 115, 116, 118, 120, 121, 123,
  124, 126, 128, 129, 131, 132, 134, 136, 138, 139, 141, 143, 145, 146, 148, 150,
  152, 154, 155, 157, 159, 161, 163, 165, 167, 169, 171, 173, 175, 177, 179, 181,
  183, 185, 187, 189, 191, 193, 196, 198, 200, 202, 204, 207, 209, 211, 214, 216,
  218, 220, 223, 225, 228, 230, 232, 235, 237, 240, 242, 245, 247, 250, 252, 255
};

unsigned long rgbFlashEndTime = 0;
uint8_t flashR = 0, flashG = 0, flashB = 0;

void triggerRgbFlash(uint8_t r, uint8_t g, uint8_t b, uint16_t duration_ms) {
  flashR = r;
  flashG = g;
  flashB = b;
  rgbFlashEndTime = millis() + duration_ms;
  broadcastLedSync();
}

uint8_t getEffectiveSuitLedState(unsigned long now) {
  if (now < rgbFlashEndTime) {
    return SUIT_LED_FLASH;
  }
  if (cfgAppStateOverride <= SUIT_LED_FIRING) {
    return cfgAppStateOverride;
  }
  bool wsOnline = (lastRemoteTelemetryMs > 0) && (now - lastRemoteTelemetryMs < 2500);
  bool wsFiring = wsOnline && (remoteWebshooter.gimbal_firing != 0);
  bool wsArmed  = wsOnline && (remoteWebshooter.webshooter_pos == 0 || remoteWebshooter.gimbal_armed != 0);
  bool gimArmed = (currentState == GIMBAL_TRACKING || currentState == GIMBAL_DEPLOYING);

  if (isFiring || wsFiring) {
    return SUIT_LED_FIRING;
  }
  // Suit enters ARMED state when EITHER Webshooter OR Shoulder Mounted Gimbal is armed!
  if (gimArmed || wsArmed) {
    return SUIT_LED_DEPLOYED;
  }
  return SUIT_LED_STANDBY;
}

void broadcastLedSync() {
  unsigned long now = millis();
  uint16_t period = (cfgBreathPeriodMs >= 300) ? cfgBreathPeriodMs : 3600;
  uint16_t armedPeriod = (cfgArmedPeriodMs >= 250) ? cfgArmedPeriodMs : DEFAULT_ARMED_PERIOD_MS;

  SuitLedSyncMessage syncMsg = {};
  syncMsg.command = CMD_LED_SYNC;
  syncMsg.suit_state = getEffectiveSuitLedState(now);
  syncMsg.pattern = cfgStealthMode ? PATTERN_STEALTH_OFF : cfgLedPattern;
  syncMsg.armed_pattern = cfgStealthMode ? PATTERN_STEALTH_OFF : cfgArmedPattern;
  syncMsg.wave_mode = cfgWaveMode;
  syncMsg.wave_offset_pct = cfgWaveOffsetPct;
  syncMsg.stealth_mode = (cfgStealthMode || cfgLedPattern == PATTERN_STEALTH_OFF) ? 1 : 0;
  syncMsg.gimbal_actual_state = (uint8_t)currentState;
  syncMsg.gimbal_zeroed = hasBeenZeroed ? 1 : 0;
  syncMsg.standby_r = cfgStandbyR;
  syncMsg.standby_g = cfgStandbyG;
  syncMsg.standby_b = cfgStandbyB;
  syncMsg.deployed_r = cfgDeployedR;
  syncMsg.deployed_g = cfgDeployedG;
  syncMsg.deployed_b = cfgDeployedB;
  syncMsg.flash_r = flashR;
  syncMsg.flash_g = flashG;
  syncMsg.flash_b = flashB;
  syncMsg.min_brightness = cfgMinBrightness;
  syncMsg.max_brightness = cfgMaxBrightness;
  syncMsg.breath_period_ms = period;
  syncMsg.armed_period_ms = armedPeriod;
  syncMsg.breath_phase_ms = (uint16_t)(now % period);
  syncMsg.armed_phase_ms = (uint16_t)(now % armedPeriod);
  syncMsg.sequence = ++ledSyncSequence;

  uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcastAddr, (const uint8_t *)&syncMsg, sizeof(syncMsg));
}

void ws2812WritePixels(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1) {
  rmt_data_t led_data[48];
  static rmt_obj_t* rmt_send = NULL;
  static bool initialized = false;

  if (!initialized) {
    rmt_send = rmtInit(PIN_RGB_LED, RMT_TX_MODE, RMT_MEM_64);
    if (rmt_send == NULL) return;
    rmtSetTick(rmt_send, 100);
    initialized = true;
  }

  uint8_t colors[6] = {r0, b0, g0, r1, b1, g1}; // Hardware RBG diode format: Byte 0=R, Byte 1=B, Byte 2=G
  int idx = 0;
  for (int p = 0; p < 6; p++) {
    uint8_t val = colors[p];
    for (int bit = 7; bit >= 0; bit--) {
      if (val & (1 << bit)) {
        led_data[idx].level0 = 1;
        led_data[idx].duration0 = 8; // 0.8us
        led_data[idx].level1 = 0;
        led_data[idx].duration1 = 4; // 0.4us
      } else {
        led_data[idx].level0 = 1;
        led_data[idx].duration0 = 4; // 0.4us
        led_data[idx].level1 = 0;
        led_data[idx].duration1 = 8; // 0.8us
      }
      idx++;
    }
  }
  rmtWriteBlocking(rmt_send, led_data, 48);
}

void ws2812WriteBoth(uint8_t r, uint8_t g, uint8_t b) {
  ws2812WritePixels(r, g, b, r, g, b);
}

uint8_t computePatternBrightness(uint8_t pattern, uint32_t phaseMs, uint16_t periodMs, uint8_t minB, uint8_t maxB) {
  if (pattern == PATTERN_STEALTH_OFF) {
    return 0;
  }
  if (pattern == PATTERN_SOLID) {
    return maxB;
  }

  float norm = evaluateSuitPatternNorm(pattern, phaseMs, periodMs);
  norm = constrain(norm, 0.0f, 1.0f);
  uint8_t index = (uint8_t)(norm * 255.0f);
  uint8_t corrected = pgm_read_byte(&cie1931_table[index]);
  return minB + (uint8_t)(((uint32_t)corrected * (maxB - minB)) / 255);
}

void updateRgbLed() {
  unsigned long now = millis();

  // Rate-limit ALL WS2812 updates to 50Hz (every 20ms)
  static unsigned long lastLedFrameMs = 0;
  if (now - lastLedFrameMs < 20) {
    return;
  }
  lastLedFrameMs = now;

  // 1. Temporary high-priority visual flash (e.g. calibration confirm or error strobe)
  if (now < rgbFlashEndTime) {
    ws2812WriteBoth(flashR, flashG, flashB);
    return;
  }

  uint8_t effState = getEffectiveSuitLedState(now);

  // 2. Firing mode: Solid bright intense red immediately on both barrels!
  if (effState == SUIT_LED_FIRING) {
    ws2812WriteBoth(255, 0, 0);
    return;
  }

  // 2b. Suit-wide Stealth Mode (All LEDs OFF when toggled on Webshooter or Web UI)
  if (cfgStealthMode) {
    ws2812WriteBoth(0, 0, 0);
    return;
  }

  // 3. Armed State (Either Webshooter OR Shoulder Gimbal is Armed)
  if (effState == SUIT_LED_DEPLOYED) {
    if (cfgArmedPattern == PATTERN_STEALTH_OFF) {
      ws2812WriteBoth(0, 0, 0);
      return;
    }
    uint16_t armedPeriod = (cfgArmedPeriodMs >= 250) ? cfgArmedPeriodMs : DEFAULT_ARMED_PERIOD_MS;
    uint32_t baseArmedPhase = (uint32_t)(now % armedPeriod);
    // Shoulder Gimbal is TIER_GIMBAL (High = 2)
    uint32_t gimArmedPhase = applySuitWaveOffset(baseArmedPhase, armedPeriod, cfgWaveMode, cfgWaveOffsetPct, TIER_GIMBAL);
    uint8_t level = computePatternBrightness(cfgArmedPattern, gimArmedPhase, armedPeriod, cfgMinBrightness, cfgMaxBrightness);

    uint8_t baseR = cfgDeployedR, baseG = cfgDeployedG, baseB = cfgDeployedB;
    if (cfgArmedPattern == PATTERN_RAINBOW_FLOW) {
      float hue = (360.0f * (float)gimArmedPhase) / (float)armedPeriod;
      suitHsvToRgb(hue, 1.0f, 1.0f, baseR, baseG, baseB);
    }

    uint8_t r = (uint8_t)(((uint32_t)baseR * level) / 255);
    uint8_t g = (uint8_t)(((uint32_t)baseG * level) / 255);
    uint8_t b = (uint8_t)(((uint32_t)baseB * level) / 255);
    ws2812WriteBoth(r, g, b);
    return;
  }

  // 4. Standby / Disarmed State (with Low -> Mid -> High Traveling Wave support)
  if (cfgLedPattern == PATTERN_STEALTH_OFF) {
    ws2812WriteBoth(0, 0, 0);
    return;
  }

  uint16_t period = (cfgBreathPeriodMs >= 300) ? cfgBreathPeriodMs : 3600;
  uint32_t basePhase = (uint32_t)(now % period);
  // Shoulder Gimbal is TIER_GIMBAL (High = 2): pulses after Webshooter (Low = 0) and Vest (Mid = 1)
  uint32_t gimPhase = applySuitWaveOffset(basePhase, period, cfgWaveMode, cfgWaveOffsetPct, TIER_GIMBAL);
  uint8_t level = computePatternBrightness(cfgLedPattern, gimPhase, period, cfgMinBrightness, cfgMaxBrightness);

  uint8_t baseR = cfgStandbyR, baseG = cfgStandbyG, baseB = cfgStandbyB;
  if (cfgLedPattern == PATTERN_RAINBOW_FLOW) {
    float hue = (360.0f * (float)gimPhase) / (float)period;
    suitHsvToRgb(hue, 1.0f, 1.0f, baseR, baseG, baseB);
  }

  uint8_t r = (uint8_t)(((uint32_t)baseR * level) / 255);
  uint8_t g = (uint8_t)(((uint32_t)baseG * level) / 255);
  uint8_t b = (uint8_t)(((uint32_t)baseB * level) / 255);
  ws2812WriteBoth(r, g, b);
}

// ==============================================================================
// Setup
// ==============================================================================
void setup() {
  // 1. Initialize Servos (LEDC PWM) and Fire ESC immediately at millisecond 0
  initServosAndFireEsc();

  // Initialize WS2812 RGB LED (starts soft white right at boot)
  ws2812WriteBoth(30, 30, 30);

  // Cold boot: hold folded position (0°, 0°)
  writeYawServo(SERVO_YAW_FOLDED_DEG);
  writePitchServo(SERVO_PITCH_FOLDED_DEG);
  currentYaw = SERVO_YAW_FOLDED_DEG;
  currentPitch = SERVO_PITCH_FOLDED_DEG;
  currentState = GIMBAL_FOLDED;
  foldCompleteTime = millis();

  Serial.begin(SERIAL_BAUD_RATE);
  delay(100);

  Serial.println("\n============================================================");
  Serial.println("  SHOULDER MOUNTED GIMBAL - 2-AXIS HEAD TRACKER & MESH HUB");
  Serial.println("  Fire ESC: GPIO 0 | Yaw: GPIO 1 | Pitch: GPIO 3");
  Serial.println("  Dual 6-DOF IMUs (BMI160/MPU6050): Neck 0x69 | Shldr 0x68");
  Serial.println("============================================================");

  // 2. Initialize I2C Bus and both MPU6050 sensors
  initI2CAndIMUs();

  // 3. Load Ground Calibration Offsets & LED/Motion Settings from Flash NVS
  loadCalibrationFromNVS();

  // 3b. Initialize Battery Voltage ADC (GPIO 2, 100k-100k divider)
  // Ensure internal pull-up (active by default on strapping pin GPIO 2) is strictly disabled
  gpio_set_pull_mode((gpio_num_t)PIN_BATTERY_ADC, GPIO_FLOATING);
  gpio_pullup_dis((gpio_num_t)PIN_BATTERY_ADC);
  gpio_pulldown_dis((gpio_num_t)PIN_BATTERY_ADC);
  gpio_set_pull_mode(GPIO_NUM_4, GPIO_FLOATING);
  gpio_pullup_dis(GPIO_NUM_4);
  gpio_pulldown_dis(GPIO_NUM_4);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_2, ADC_ATTEN_DB_11);
  adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_DB_11);
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc1_chars);
  if (val_type != ESP_ADC_CAL_VAL_NOT_SUPPORTED) {
    adc1_cal_initialized = true;
    Serial.printf("[ADC] Calibration Initialized (eFuse type: %d)\n", (int)val_type);
  } else {
    Serial.println("[ADC] Warning: eFuse calibration not supported, using default curve.");
  }

  // 4. Initialize ESP-NOW, BLE GATT Server & Wireless Mode (Default: BLE Mode)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(WIFI_AP_CHANNEL, WIFI_SECOND_CHAN_NONE);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.printf("[ESP-NOW] Suit LED Mesh Master & Slave Initialized on Channel %d.\n", WIFI_AP_CHANNEL);
  } else {
    Serial.println("[ESP-NOW] Error: ESP-NOW init failed!");
  }

  // 5. Start BLE GATT Server ("Suit-Gimbal-Hub") & Apply Active Wireless Mode (BLE Default or Wi-Fi AP)
  initBleGattServer();
  applyWirelessCommMode(cfgCommMode, false);

  lastLoopTimeUs = micros();
  Serial.println("\n[GIMBAL READY] Gimbal is folded. Press 'h' for serial command menu ('b' toggles BLE / Wi-Fi AP).");
}

// ==============================================================================
// Main Loop
// ==============================================================================
void loop() {
  // 1a. Handle Web Server Client Requests (when in Wi-Fi AP mode) & BLE Commands
  if (cfgCommMode == COMM_MODE_WIFI && webServerStarted) {
    server.handleClient();
  }
  if (pendingBleCommand.length() > 0) {
    String cmd = pendingBleCommand;
    pendingBleCommand = "";
    handleBleCommandString(cmd);
  }
  if (pendingCommModeSwitch >= 0 && millis() >= pendingCommSwitchAtMs) {
    uint8_t targetMode = (uint8_t)pendingCommModeSwitch;
    pendingCommModeSwitch = -1;
    applyWirelessCommMode(targetMode, true);
  }

  // 1b. Sample Battery ADC (GPIO 2)
  updateBatteryAdc();

  // 1c. Calculate loop delta time (dt)
  unsigned long nowUs = micros();
  float dt = (float)(nowUs - lastLoopTimeUs) / 1000000.0f;
  if (dt <= 0.0f || dt > 0.1f) dt = 0.02f; // Clamp anomalous step
  lastLoopTimeUs = nowUs;

  // 2. Update attitudes for both IMUs (50Hz - 200Hz)
  if (imuNeck.isConnected) {
    imuNeck.updateOrientation(dt, COMPLEMENTARY_ALPHA);
  }
  if (imuShoulder.isConnected) {
    imuShoulder.updateOrientation(dt, COMPLEMENTARY_ALPHA);
  }

  // 3. Handle Serial CLI commands (e.g. ground calibration, zero yaw, deploy, fold, fire)
  processSerialCommands();

  // 4. Update Gimbal Motion State Machine & Sequence
  updateGimbalStateMachine(dt);

  // 4b. Wi-Fi / BLE Disconnect Failsafe: Auto-default back to Head Tracking
  if (rcModeActive && !bleClientConnected) {
    bool wifiDisconnected = (cfgCommMode != COMM_MODE_WIFI) || (WiFi.softAPgetStationNum() == 0);
    bool heartbeatTimeout = (lastWebClientActiveMs > 0 && (millis() - lastWebClientActiveMs > 3000));
    if (wifiDisconnected || heartbeatTimeout) {
      rcModeActive = false;
      lastWebClientActiveMs = 0;
      filteredTargetYaw = currentYaw;
      filteredTargetPitch = currentPitch;
      if (!hasBeenZeroed) {
        zeroYawOffset();
      }
      Serial.printf("\n[FAILSAFE] Wireless %s -> Defaulting to HEAD TRACKING mode.\n",
                    wifiDisconnected ? "disconnected" : "heartbeat timeout (>3s)");
    }
  }

  // 4c. Periodic BLE Status Notification (4Hz when connected to leds.html)
  if (bleClientConnected && pStatusChar != nullptr && (millis() - lastBleNotifyMs >= 250)) {
    lastBleNotifyMs = millis();
    String json = buildBleStatusJson();
    pStatusChar->setValue(json.c_str());
    pStatusChar->notify();
  }

  // 5. Update Apple-style WS2812 RGB LED Engine (GPIO 21)
  updateRgbLed();

  // 5b. Broadcast Suit LED Sync packet to all ESP32s in the mesh every 200ms (5Hz)
  if (millis() - lastLedSyncBroadcastMs >= LED_SYNC_BROADCAST_MS) {
    lastLedSyncBroadcastMs = millis();
    broadcastLedSync();
  }

  // 5c. Electromagnet Safety Watchdog (Hard 8.0s maximum limit to protect coil)
  if (isFiring) {
    if (millis() - fireStartTime >= FIRE_MAX_DURATION_MS) {
      handleCmdStopFire();
      Serial.println("\n[FIRE WATCHDOG] Safety continuous firing limit reached! Shut off to protect coil.");
    }
  }

  // 6. Periodic Telemetry Stream (if enabled via 't')
  if (telemetryStreamActive && (millis() - lastTelemetryTimeMs >= 100)) {
    lastTelemetryTimeMs = millis();
    float relYaw = (imuNeck.yaw - imuShoulder.yaw) - relativeYawZeroOffset;
    float relPitch = (imuNeck.pitch - imuShoulder.pitch) - relativePitchZeroOffset;

    const char* stateStr = (currentState == GIMBAL_FOLDED)    ? "FOLDED" :
                           (currentState == GIMBAL_DEPLOYING) ? "DEPLOYING" :
                           (currentState == GIMBAL_TRACKING)  ? "TRACKING" : "FOLDING";

    Serial.printf("[TELEM] State:%s Zeroed:%s Neck:%s Shldr:%s | RelYaw:%+5.1f° RelPitch:%+5.1f° | ServoYaw:%5.1f° ServoPitch:%5.1f° | Neck(P:%+5.1f,Y:%+5.1f) Shldr(P:%+5.1f,Y:%+5.1f)\n",
                  stateStr,
                  hasBeenZeroed ? "YES" : "NO",
                  imuNeck.isConnected ? "OK" : "ERR",
                  imuShoulder.isConnected ? "OK" : "ERR",
                  relYaw, relPitch, currentYaw, currentPitch,
                  imuNeck.pitch, imuNeck.yaw, imuShoulder.pitch, imuShoulder.yaw);
  }

  // 6b. Live Raw 6-Axis Gravity & Orientation Diagnostic (if enabled via 'm')
  if (rawDiagStreamActive && (millis() - lastTelemetryTimeMs >= 120)) {
    lastTelemetryTimeMs = millis();
    auto getDownAxis = [](float ax, float ay, float az) -> const char* {
      float mag = sqrtf(ax*ax + ay*ay + az*az);
      if (mag < 0.6f) return "FREEFALL/ACCEL";
      float absX = fabsf(ax), absY = fabsf(ay), absZ = fabsf(az);
      if (absZ >= absX && absZ >= absY) return (az > 0) ? "+Z DOWN (Chip facing UP)" : "-Z DOWN (Chip facing DOWN)";
      if (absY >= absX && absY >= absZ) return (ay > 0) ? "+Y DOWN (Pin header DOWN)" : "-Y DOWN (Pin header UP)";
      return (ax > 0) ? "+X DOWN (Right edge DOWN)" : "-X DOWN (Left edge DOWN)";
    };

    Serial.printf("[GRAV DIAG]\n"
                  "  NECK  (0x69): Ax:%+5.2fg Ay:%+5.2fg Az:%+5.2fg | Gx:%+5.1f Gy:%+5.1f Gz:%+5.1f dps | Ground: %s | RawPitch:%+5.1f°\n"
                  "  SHLDR (0x68): Ax:%+5.2fg Ay:%+5.2fg Az:%+5.2fg | Gx:%+5.1f Gy:%+5.1f Gz:%+5.1f dps | Ground: %s | RawPitch:%+5.1f°\n",
                  imuNeck.ax, imuNeck.ay, imuNeck.az, imuNeck.gx, imuNeck.gy, imuNeck.gz,
                  getDownAxis(imuNeck.ax, imuNeck.ay, imuNeck.az), imuNeck.calculateRawAccelPitch(),
                  imuShoulder.ax, imuShoulder.ay, imuShoulder.az, imuShoulder.gx, imuShoulder.gy, imuShoulder.gz,
                  getDownAxis(imuShoulder.ax, imuShoulder.ay, imuShoulder.az), imuShoulder.calculateRawAccelPitch());
  }

  // Rate-limit loop to ~50Hz (20ms) for servo stability
  delay(10);
}

// ==============================================================================
// State Machine & Sequence Engine
// ==============================================================================
// State Machine & Sequence Engine
// ==============================================================================
void calculateIntendedAngles(float &targetYaw, float &targetPitch) {
  float rawRelativeYaw = imuNeck.yaw - imuShoulder.yaw;
  float relativeYaw = rawRelativeYaw - relativeYawZeroOffset;
  float rawRelativePitch = imuNeck.pitch - imuShoulder.pitch;
  float relativePitch = rawRelativePitch - relativePitchZeroOffset;

  targetYaw = SERVO_YAW_DEPLOYED_DEG + (relativeYaw * YAW_DIRECTION_SIGN);
  targetPitch = SERVO_PITCH_DEPLOYED_DEG + (relativePitch * PITCH_DIRECTION_SIGN);

  targetYaw = constrain(targetYaw, SERVO_YAW_MIN_DEG, SERVO_YAW_MAX_DEG);
  targetPitch = constrain(targetPitch, SERVO_PITCH_MIN_DEG, SERVO_PITCH_MAX_DEG);
}

void startDeploySequence() {
  if (currentState == GIMBAL_TRACKING || currentState == GIMBAL_DEPLOYING) {
    return;
  }

  // Safety Interlock: Must be zeroed at least once after power-on (unless in direct RC mode)
  if (!hasBeenZeroed && !rcModeActive) {
    triggerRgbFlash(255, 120, 0, 350); // Amber warning flash
    Serial.println("\n[SAFETY INTERLOCK] Deploy BLOCKED! The gimbal cannot arm until it is zeroed.");
    Serial.println("  -> Look straight forward and press 'c' (or hold Webshooter button) to zero yaw first.\n");
    return;
  }

  Serial.println("[DEPLOY] Simultaneous unfolding: Pitch and Yaw slowly chasing live head position...");
  currentState = GIMBAL_DEPLOYING;
  startSeqPitch = currentPitch;
  startSeqYaw = currentYaw;
  seqStartTime = millis();
  seqDurationMs = cfgDeployDurationMs;
  pwmActive = true;
  broadcastLedSync();
}

void startFoldSequence() {
  if (currentState == GIMBAL_FOLDED || currentState == GIMBAL_FOLDING) {
    return;
  }

  Serial.println("[FOLD] Simultaneous retracting: Pitch (-> 0°) and Yaw (-> 0°)...");
  currentState = GIMBAL_FOLDING;
  startSeqPitch = currentPitch;
  startSeqYaw = currentYaw;
  targetSeqPitch = SERVO_PITCH_FOLDED_DEG;
  targetSeqYaw = SERVO_YAW_FOLDED_DEG;
  seqStartTime = millis();
  seqDurationMs = cfgFoldDurationMs;
  pwmActive = true;
  broadcastLedSync();
}

void updateGimbalStateMachine(float dt) {
  unsigned long now = millis();

  switch (currentState) {
  // ----------------------------------------------------------------------------
  // GIMBAL_FOLDED: Servos at stowed position (0°, 0°), optional torque release
  // ----------------------------------------------------------------------------
  case GIMBAL_FOLDED:
    if (pwmActive && SERVO_AUTO_RELEASE_MS > 0) {
      if (now - foldCompleteTime >= SERVO_AUTO_RELEASE_MS) {
        releaseServos();
      }
    }
    break;

  // ----------------------------------------------------------------------------
  // GIMBAL_DEPLOYING: Simultaneous unfolding (Pitch + Yaw slowly chasing live head position)
  // ----------------------------------------------------------------------------
  case GIMBAL_DEPLOYING: {
    float intendedYaw, intendedPitch;
    if (rcModeActive) {
      intendedYaw = rcTargetYaw;
      intendedPitch = rcTargetPitch;
    } else {
      calculateIntendedAngles(intendedYaw, intendedPitch);
    }

    float t = (float)(now - seqStartTime) / (float)seqDurationMs;
    if (t >= 1.0f) {
      currentPitch = intendedPitch;
      currentYaw = intendedYaw;
      writePitchServo(currentPitch);
      writeYawServo(currentYaw);
      lastCommittedYaw = currentYaw;
      lastCommittedPitch = currentPitch;

      // Deployment complete: seamlessly hand over to active tracking with zero jump!
      filteredTargetYaw = currentYaw;
      filteredTargetPitch = currentPitch;
      currentState = GIMBAL_TRACKING;
      Serial.printf("[DEPLOY COMPLETE] Gimbal deployed and locked onto target (Yaw: %.1f°, Pitch: %.1f°)! %s\n",
                    currentYaw, currentPitch, rcModeActive ? "RC Direct Mode ON." : "Active tracking ON.");
    } else {
      float easedT = smootherStep(t);
      currentPitch = startSeqPitch + easedT * (intendedPitch - startSeqPitch);
      currentYaw = startSeqYaw + easedT * (intendedYaw - startSeqYaw);
      writePitchServo(currentPitch);
      writeYawServo(currentYaw);
    }
    break;
  }

  // ----------------------------------------------------------------------------
  // GIMBAL_TRACKING: Active 2-axis Head Tracking or RC Manual Mode
  // ----------------------------------------------------------------------------
  case GIMBAL_TRACKING: {
    if (rcModeActive) {
      updateRcControl(dt);
    } else {
      updateTracking(dt);
    }
    break;
  }

  // ----------------------------------------------------------------------------
  // GIMBAL_FOLDING: Simultaneous retracting (Pitch -> 0°, Yaw -> 0°)
  // ----------------------------------------------------------------------------
  case GIMBAL_FOLDING: {
    float t = (float)(now - seqStartTime) / (float)seqDurationMs;
    if (t >= 1.0f) {
      currentPitch = targetSeqPitch;
      currentYaw = targetSeqYaw;
      writePitchServo(currentPitch);
      writeYawServo(currentYaw);
      currentState = GIMBAL_FOLDED;
      foldCompleteTime = now;
      Serial.println("[FOLD COMPLETE] Gimbal fully stowed at (0°, 0°).");
    } else {
      float easedT = smootherStep(t);
      currentPitch = startSeqPitch + easedT * (targetSeqPitch - startSeqPitch);
      currentYaw = startSeqYaw + easedT * (targetSeqYaw - startSeqYaw);
      writePitchServo(currentPitch);
      writeYawServo(currentYaw);
    }
    break;
  }
  }
}

// ==============================================================================
// Active Head Tracking Calculation (Velocity-Adaptive EMA + Anti-Jitter Deadband)
// ==============================================================================
void updateTracking(float dt) {
  float rawTargetYaw, rawTargetPitch;
  calculateIntendedAngles(rawTargetYaw, rawTargetPitch);

  // Adaptive velocity-aware smoothing (1-Euro filter principle):
  //   - Small errors (< 2.0°): heavy damping (0.35x alpha) suppresses posture/sensor micro-tremors
  //   - Larger head turns (>= 2.0°): full cfgTrackingAlpha for snappy, zero-lag tracking
  float errYaw = fabsf(rawTargetYaw - filteredTargetYaw);
  float errPitch = fabsf(rawTargetPitch - filteredTargetPitch);

  float yawScale = constrain(0.35f + 0.65f * (errYaw / 2.5f), 0.35f, 1.0f);
  float pitchScale = constrain(0.35f + 0.65f * (errPitch / 2.5f), 0.35f, 1.0f);

  filteredTargetYaw += (cfgTrackingAlpha * yawScale) * (rawTargetYaw - filteredTargetYaw);
  filteredTargetPitch += (cfgTrackingAlpha * pitchScale) * (rawTargetPitch - filteredTargetPitch);

  // Slew rate limiting to protect servo gears from sudden violent snaps
  float maxStep = cfgMaxSpeedDps * dt;
  float yawDiff = filteredTargetYaw - currentYaw;
  float pitchDiff = filteredTargetPitch - currentPitch;

  yawDiff = constrain(yawDiff, -maxStep, maxStep);
  pitchDiff = constrain(pitchDiff, -maxStep, maxStep);

  currentYaw += yawDiff;
  currentPitch += pitchDiff;

  // Sub-degree PWM hysteresis deadband: prevents servos from buzzing/chattering when head is still
  if (fabsf(currentYaw - lastCommittedYaw) >= SERVO_DEADBAND_DEG) {
    writeYawServo(currentYaw);
    lastCommittedYaw = currentYaw;
  }
  if (fabsf(currentPitch - lastCommittedPitch) >= SERVO_DEADBAND_DEG) {
    writePitchServo(currentPitch);
    lastCommittedPitch = currentPitch;
  }
}

// ==============================================================================
// Manual RC Virtual Joystick Controller (Manual Mode Override)
// ==============================================================================
void updateRcControl(float dt) {
  rcTargetYaw = constrain(rcTargetYaw, SERVO_YAW_MIN_DEG, SERVO_YAW_MAX_DEG);
  rcTargetPitch = constrain(rcTargetPitch, SERVO_PITCH_MIN_DEG, SERVO_PITCH_MAX_DEG);

  // Exponential smoothing filter towards RC stick target
  filteredTargetYaw += cfgTrackingAlpha * (rcTargetYaw - filteredTargetYaw);
  filteredTargetPitch += cfgTrackingAlpha * (rcTargetPitch - filteredTargetPitch);

  // Slew rate limiting to protect servo gears from sudden violent snaps
  float maxStep = cfgMaxSpeedDps * dt;
  float yawDiff = filteredTargetYaw - currentYaw;
  float pitchDiff = filteredTargetPitch - currentPitch;

  yawDiff = constrain(yawDiff, -maxStep, maxStep);
  pitchDiff = constrain(pitchDiff, -maxStep, maxStep);

  currentYaw += yawDiff;
  currentPitch += pitchDiff;

  writeYawServo(currentYaw);
  writePitchServo(currentPitch);
}

// ==============================================================================
// Firing System Handlers (25W Electromagnet Brushed ESC)
// ==============================================================================
void handleCmdFire(bool force) {
  // Critical Safety: Only permit firing if gimbal is fully deployed/tracking,
  // or forced by manual test command (Serial/Web UI)
  if (!force && currentState != GIMBAL_TRACKING && currentState != GIMBAL_DEPLOYING) {
    Serial.println("[FIRE SAFETY] Fire command BLOCKED: Gimbal is not deployed/armed!");
    return;
  }
  isFiring = true;
  fireStartTime = millis();
  writeFirePulseUs(FIRE_PWM_ACTIVE_US);
  broadcastLedSync();
}

void handleCmdStopFire() {
  isFiring = false;
  writeFirePulseUs(FIRE_PWM_IDLE_US);
  broadcastLedSync();
}

// ==============================================================================
// Yaw & Pitch Re-zeroing + 3D Gravity-Aligned Neck-Tape Skew Compensation
// ==============================================================================
void zeroYawOffset() {
  // Sample 15 fast readings (~30ms) while wearer is looking straight forward dead-center
  // so the low-pass filtered gravity vector captures the exact current neck-tape & shoulder tilt
  for (int i = 0; i < 15; i++) {
    imuNeck.readRawData();
    imuShoulder.readRawData();
    delay(2);
  }

  // Calibrate 3D orthonormal Yaw (vertical gravity) & Pitch (horizontal orthogonal) axes
  // for both IMUs so turning the head flat left/right produces 0% Pitch bleed even if
  // the neck mount tape is skewed/angled sideways!
  imuNeck.calibrateMountAxes();
  imuShoulder.calibrateMountAxes();

  imuNeck.yaw = 0.0f;
  imuNeck.pitch = 0.0f;
  imuShoulder.yaw = 0.0f;
  imuShoulder.pitch = 0.0f;

  relativeYawZeroOffset = 0.0f;
  relativePitchZeroOffset = 0.0f;
  filteredTargetYaw = SERVO_YAW_DEPLOYED_DEG;
  filteredTargetPitch = SERVO_PITCH_DEPLOYED_DEG;
  hasBeenZeroed = true;

  triggerRgbFlash(0, 255, 255, 250); // Cyan flash on zero confirm (also broadcasts state sync)
  Serial.printf("\n[CALIBRATE] 3D Gravity-Aligned Zero Complete!\n"
                "  Neck (0x69)     Skew Compensated: %.1f° | YawAxis:(%+.2f,%+.2f,%+.2f) PitchAxis:(%+.2f,%+.2f,%+.2f)\n"
                "  Shoulder (0x68) Skew Compensated: %.1f° | YawAxis:(%+.2f,%+.2f,%+.2f) PitchAxis:(%+.2f,%+.2f,%+.2f)\n",
                imuNeck.mountTiltDeg,
                imuNeck.yaw_axis_x, imuNeck.yaw_axis_y, imuNeck.yaw_axis_z,
                imuNeck.pitch_axis_x, imuNeck.pitch_axis_y, imuNeck.pitch_axis_z,
                imuShoulder.mountTiltDeg,
                imuShoulder.yaw_axis_x, imuShoulder.yaw_axis_y, imuShoulder.yaw_axis_z,
                imuShoulder.pitch_axis_x, imuShoulder.pitch_axis_y, imuShoulder.pitch_axis_z);
  Serial.println("  [✓] Arming UNLOCKED! You can now press 'd' (or single tap Webshooter) to deploy.\n");
}

// ==============================================================================
// Ground Calibration (USB / Ground Setup)
// ==============================================================================
void runGroundCalibration() {
  Serial.println("\n============================================================");
  Serial.println("  STARTING GROUND CALIBRATION (Keep Gimbal Stationary!)");
  Serial.println("============================================================");
  Serial.println("Measuring gyro biases & resting accelerometer pitch offsets...");

  const int SAMPLES = 400;
  float neck_gx_sum = 0, neck_gy_sum = 0, neck_gz_sum = 0, neck_pitch_sum = 0;
  float shldr_gx_sum = 0, shldr_gy_sum = 0, shldr_gz_sum = 0, shldr_pitch_sum = 0;

  for (int i = 0; i < SAMPLES; i++) {
    if (imuNeck.readRawData()) {
      neck_gx_sum += imuNeck.gx;
      neck_gy_sum += imuNeck.gy;
      neck_gz_sum += imuNeck.gz;
      neck_pitch_sum += imuNeck.calculateRawAccelPitch();
    }
    if (imuShoulder.readRawData()) {
      shldr_gx_sum += imuShoulder.gx;
      shldr_gy_sum += imuShoulder.gy;
      shldr_gz_sum += imuShoulder.gz;
      shldr_pitch_sum += imuShoulder.calculateRawAccelPitch();
    }
    delay(5);
  }

  imuNeck.gx_bias = neck_gx_sum / SAMPLES;
  imuNeck.gy_bias = neck_gy_sum / SAMPLES;
  imuNeck.gz_bias = neck_gz_sum / SAMPLES;
  imuNeck.pitch_offset = neck_pitch_sum / SAMPLES;
  imuNeck.pitch = 0.0f;
  imuNeck.yaw = 0.0f;
  imuNeck.calibrateMountAxes();

  imuShoulder.gx_bias = shldr_gx_sum / SAMPLES;
  imuShoulder.gy_bias = shldr_gy_sum / SAMPLES;
  imuShoulder.gz_bias = shldr_gz_sum / SAMPLES;
  imuShoulder.pitch_offset = shldr_pitch_sum / SAMPLES;
  imuShoulder.pitch = 0.0f;
  imuShoulder.yaw = 0.0f;
  imuShoulder.calibrateMountAxes();

  relativeYawZeroOffset = 0.0f;
  relativePitchZeroOffset = 0.0f;

  saveCalibrationToNVS();

  Serial.println("\n[GROUND CALIBRATION COMPLETE]");
  Serial.printf("  Neck (0x69): GyroBias(X:%.2f, Y:%.2f, Z:%.2f) PitchOffset:%.2f°\n",
                imuNeck.gx_bias, imuNeck.gy_bias, imuNeck.gz_bias, imuNeck.pitch_offset);
  Serial.printf("  Shoulder (0x68): GyroBias(X:%.2f, Y:%.2f, Z:%.2f) PitchOffset:%.2f°\n",
                imuShoulder.gx_bias, imuShoulder.gy_bias, imuShoulder.gz_bias, imuShoulder.pitch_offset);
  Serial.println("  Offsets saved to Flash NVS.\n");
}

void saveCalibrationToNVS() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUChar("n_chip", (uint8_t)imuNeck.chipType);
  prefs.putFloat("n_gx", imuNeck.gx_bias);
  prefs.putFloat("n_gy", imuNeck.gy_bias);
  prefs.putFloat("n_gz", imuNeck.gz_bias);
  prefs.putFloat("n_poff", imuNeck.pitch_offset);

  prefs.putUChar("s_chip", (uint8_t)imuShoulder.chipType);
  prefs.putFloat("s_gx", imuShoulder.gx_bias);
  prefs.putFloat("s_gy", imuShoulder.gy_bias);
  prefs.putFloat("s_gz", imuShoulder.gz_bias);
  prefs.putFloat("s_poff", imuShoulder.pitch_offset);
  prefs.putBool("cal_done", true);
  prefs.end();
}

void autoCalibrateImuBiasOnBoot(bool calNeck, bool calShoulder) {
  if (!calNeck && !calShoulder) return;
  Serial.println("[AUTO-BIAS] Capturing fresh startup gyro zero-rate biases (~400ms)...");
  const int BOOT_SAMPLES = 80;
  float n_gx = 0, n_gy = 0, n_gz = 0, n_p = 0;
  float s_gx = 0, s_gy = 0, s_gz = 0, s_p = 0;
  int n_cnt = 0, s_cnt = 0;

  for (int i = 0; i < BOOT_SAMPLES; i++) {
    if (calNeck && imuNeck.isConnected && imuNeck.readRawData()) {
      n_gx += imuNeck.gx;
      n_gy += imuNeck.gy;
      n_gz += imuNeck.gz;
      n_p  += imuNeck.calculateRawAccelPitch();
      n_cnt++;
    }
    if (calShoulder && imuShoulder.isConnected && imuShoulder.readRawData()) {
      s_gx += imuShoulder.gx;
      s_gy += imuShoulder.gy;
      s_gz += imuShoulder.gz;
      s_p  += imuShoulder.calculateRawAccelPitch();
      s_cnt++;
    }
    delay(5);
  }

  if (calNeck && n_cnt > 0) {
    imuNeck.gx_bias = n_gx / n_cnt;
    imuNeck.gy_bias = n_gy / n_cnt;
    imuNeck.gz_bias = n_gz / n_cnt;
    imuNeck.pitch_offset = n_p / n_cnt;
    Serial.printf("[AUTO-BIAS] Neck (%s 0x69) fresh bias: GX=%.2f, GY=%.2f, GZ=%.2f dps\n",
                  imuNeck.getChipName(), imuNeck.gx_bias, imuNeck.gy_bias, imuNeck.gz_bias);
  }
  if (calShoulder && s_cnt > 0) {
    imuShoulder.gx_bias = s_gx / s_cnt;
    imuShoulder.gy_bias = s_gy / s_cnt;
    imuShoulder.gz_bias = s_gz / s_cnt;
    imuShoulder.pitch_offset = s_p / s_cnt;
    Serial.printf("[AUTO-BIAS] Shoulder (%s 0x68) fresh bias: GX=%.2f, GY=%.2f, GZ=%.2f dps\n",
                  imuShoulder.getChipName(), imuShoulder.gx_bias, imuShoulder.gy_bias, imuShoulder.gz_bias);
  }
  saveCalibrationToNVS();
}

void loadCalibrationFromNVS() {
  prefs.begin(NVS_NAMESPACE, true);
  bool calDone = prefs.getBool("cal_done", false);
  uint8_t savedNeckChip = prefs.getUChar("n_chip", 0);
  uint8_t savedShoulderChip = prefs.getUChar("s_chip", 0);

  bool neckChipMatches = calDone && (savedNeckChip == (uint8_t)imuNeck.chipType);
  bool shoulderChipMatches = calDone && (savedShoulderChip == (uint8_t)imuShoulder.chipType);

  if (neckChipMatches) {
    imuNeck.gx_bias = prefs.getFloat("n_gx", 0.0f);
    imuNeck.gy_bias = prefs.getFloat("n_gy", 0.0f);
    imuNeck.gz_bias = prefs.getFloat("n_gz", 0.0f);
    imuNeck.pitch_offset = prefs.getFloat("n_poff", 0.0f);
  }
  if (shoulderChipMatches) {
    imuShoulder.gx_bias = prefs.getFloat("s_gx", 0.0f);
    imuShoulder.gy_bias = prefs.getFloat("s_gy", 0.0f);
    imuShoulder.gz_bias = prefs.getFloat("s_gz", 0.0f);
    imuShoulder.pitch_offset = prefs.getFloat("s_poff", 0.0f);
  }

  // Load saved Suit LED Mesh & Kinematics settings
  cfgStandbyR = prefs.getUChar("sr", cfgStandbyR);
  cfgStandbyG = prefs.getUChar("sg", cfgStandbyG);
  cfgStandbyB = prefs.getUChar("sb", cfgStandbyB);
  cfgDeployedR = prefs.getUChar("dr", cfgDeployedR);
  cfgDeployedG = prefs.getUChar("dg", cfgDeployedG);
  cfgDeployedB = prefs.getUChar("db", cfgDeployedB);

  // Sanitize legacy #FF2A55 pink swatch (255,42,85) or old default blue (0,60,255) to Pure Red (255,0,0)
  if (cfgStandbyR == 255 && cfgStandbyG == 42 && cfgStandbyB == 85) {
    cfgStandbyR = 255; cfgStandbyG = 0; cfgStandbyB = 0;
    prefs.putUChar("sr", 255); prefs.putUChar("sg", 0); prefs.putUChar("sb", 0);
  }
  if ((cfgDeployedR == 255 && cfgDeployedG == 42 && cfgDeployedB == 85) ||
      (cfgDeployedR == 0 && cfgDeployedG == 60 && cfgDeployedB == 255)) {
    cfgDeployedR = 255; cfgDeployedG = 0; cfgDeployedB = 0;
    prefs.putUChar("dr", 255); prefs.putUChar("dg", 0); prefs.putUChar("db", 0);
  }
  cfgLedPattern = prefs.getUChar("pattern", cfgLedPattern);
  cfgCommMode = prefs.getUChar("comm_v2", DEFAULT_COMM_MODE);
  cfgArmedPattern = prefs.getUChar("arm_pat", PATTERN_ARMED_SOFT_FLASH);
  cfgWaveMode = prefs.getUChar("wave_m", WAVE_LOW_TO_HIGH);
  cfgWaveOffsetPct = prefs.getUChar("wave_o", DEFAULT_WAVE_OFFSET_PCT);
  cfgStealthMode = prefs.getBool("stealth", false) || (cfgLedPattern == PATTERN_STEALTH_OFF);
  if (cfgLedPattern != PATTERN_STEALTH_OFF) {
    cfgLastActivePattern = cfgLedPattern;
  }
  cfgMinBrightness = prefs.getUChar("min_b", cfgMinBrightness);
  cfgMaxBrightness = prefs.getUChar("max_b", cfgMaxBrightness);
  cfgBreathPeriodMs = prefs.getUShort("period", cfgBreathPeriodMs);
  cfgArmedPeriodMs = prefs.getUShort("arm_per", DEFAULT_ARMED_PERIOD_MS);

  cfgTrackingAlpha = prefs.getFloat("alpha", cfgTrackingAlpha);
  cfgMaxSpeedDps = prefs.getFloat("speed", cfgMaxSpeedDps);
  cfgDeployDurationMs = prefs.getUShort("dep_ms", cfgDeployDurationMs);
  cfgFoldDurationMs = prefs.getUShort("fold_ms", cfgFoldDurationMs);

  prefs.end();

  // If either IMU was swapped (e.g. MPU6050 -> Bosch BMI160) or never calibrated,
  // automatically capture fresh gyro zero-rate biases on boot!
  bool needNeckCal = imuNeck.isConnected && !neckChipMatches;
  bool needShoulderCal = imuShoulder.isConnected && !shoulderChipMatches;
  if (needNeckCal || needShoulderCal) {
    Serial.println("[NVS] New IMU sensor hardware detected (or first boot) -> Running automatic bias calibration...");
    autoCalibrateImuBiasOnBoot(needNeckCal, needShoulderCal);
  } else {
    Serial.println("[NVS] Loaded Ground Calibration offsets from Flash.");
    for (int i = 0; i < 10; i++) {
      imuNeck.readRawData();
      imuShoulder.readRawData();
      delay(2);
    }
  }
  imuNeck.calibrateMountAxes();
  imuShoulder.calibrateMountAxes();
}

// ==============================================================================
// I2C Bus Diagnostics & Initialization
// ==============================================================================
void scanI2CBus() {
  Serial.println("\n------------------------------------------------------------");
  Serial.println("  SCANNING I2C BUS (SDA: GPIO 8, SCL: GPIO 9)");
  Serial.println("------------------------------------------------------------");
  int count = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      const char* name = (addr == IMU_ADDR_SHOULDER) ? "Shoulder IMU (0x68)" :
                         (addr == IMU_ADDR_NECK)     ? "Neck IMU (0x69)" : "Unknown Device";
      Serial.printf("  [✓] Found device at address 0x%02X (%s)\n", addr, name);
      count++;
    } else if (err == 4) {
      Serial.printf("  [!] Bus error / timeout at address 0x%02X\n", addr);
    }
  }

  if (count == 0) {
    Serial.println("  [✗] NO I2C DEVICES FOUND on bus!");
    Serial.println("      Please verify:");
    Serial.println("      1. VCC (3.3V or 5V) and GND are connected to both MPU6050s.");
    Serial.println("      2. SDA wire connects to GPIO 8, SCL wire connects to GPIO 9.");
    Serial.println("      3. Onboard LEDs on MPU6050 breakout boards are illuminated.");
  } else {
    Serial.printf("  Scan finished: %d I2C device(s) found.\n", count);
  }
  Serial.println("------------------------------------------------------------\n");
}

void initI2CAndIMUs() {
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_SPEED_HZ);
  Wire.setTimeOut(30);
  delay(100);

  // Scan bus on boot to provide immediate diagnostic output
  scanI2CBus();

  // Shoulder IMU (0x68): Vertical board with +X DOWN, facing Forward
  bool shoulderOk = imuShoulder.begin(IMU_ADDR_SHOULDER, ORIENT_VERT_AX_DOWN_FWD, &Wire);
  if (shoulderOk) {
    Serial.printf("[IMU 0x%02X] Shoulder IMU (Vertical +X Down, Facing Fwd) initialized.\n", IMU_ADDR_SHOULDER);
  } else {
    Serial.printf("[IMU 0x%02X] ERROR: Shoulder IMU not responding! Check SDA:%d SCL:%d\n",
                  IMU_ADDR_SHOULDER, PIN_I2C_SDA, PIN_I2C_SCL);
  }

  // Neck IMU (0x69): Vertical board with +X DOWN, facing Backward (180° yaw flipped)
  bool neckOk = imuNeck.begin(IMU_ADDR_NECK, ORIENT_VERT_AX_DOWN_BACK, &Wire);
  if (neckOk) {
    Serial.printf("[IMU 0x%02X] Neck IMU (Vertical +X Down, Facing Back 180°) initialized.\n", IMU_ADDR_NECK);
  } else {
    Serial.printf("[IMU 0x%02X] ERROR: Neck IMU not responding! Check SDA:%d SCL:%d\n",
                  IMU_ADDR_NECK, PIN_I2C_SDA, PIN_I2C_SCL);
  }
}

// ==============================================================================
// Native Hardware PWM (LEDC) Servo & ESC Drivers
// ==============================================================================
void initServosAndFireEsc() {
  // 1. 25W Electromagnet Brushed ESC (GPIO 0, LEDC Ch 2)
  // Release pad hold and cleanly reset pin from any boot/safety holds
  gpio_hold_dis((gpio_num_t)PIN_FIRE_ESC);
  gpio_reset_pin((gpio_num_t)PIN_FIRE_ESC);

  // Pre-load LEDC channel with 0% throttle safe idle pulse (544us) BEFORE attaching to pin
  ledcSetup(FIRE_LEDC_CHANNEL, SERVO_LEDC_FREQ, SERVO_LEDC_RESOLUTION);
  writeFirePulseUs(FIRE_PWM_IDLE_US);
  ledcAttachPin(PIN_FIRE_ESC, FIRE_LEDC_CHANNEL);
  writeFirePulseUs(FIRE_PWM_IDLE_US);

  // 2. Pitch Servo (GPIO 3, LEDC Ch 0)
  ledcSetup(SERVO_PITCH_CHANNEL, SERVO_LEDC_FREQ, SERVO_LEDC_RESOLUTION);
  ledcAttachPin(PIN_SERVO_PITCH, SERVO_PITCH_CHANNEL);

  // 3. Yaw Servo (GPIO 1, LEDC Ch 1)
  ledcSetup(SERVO_YAW_CHANNEL, SERVO_LEDC_FREQ, SERVO_LEDC_RESOLUTION);
  ledcAttachPin(PIN_SERVO_YAW, SERVO_YAW_CHANNEL);
}


void writeYawServo(float angle) {
  angle = constrain(angle, 0.0f, 180.0f);
  float pulse_us = (float)SERVO_MIN_PULSE_US + (angle / 180.0f) * (float)(SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US);
  pulse_us = constrain(pulse_us, 500.0f, 2500.0f);

  uint32_t duty = (uint32_t)(((uint64_t)pulse_us * 16384ULL) / 20000ULL);
  ledcWrite(SERVO_YAW_CHANNEL, duty);
  pwmActive = true;
}

void writePitchServo(float angle) {
  angle = constrain(angle, 0.0f, 180.0f);
  float pulse_us = (float)SERVO_MIN_PULSE_US + (angle / 180.0f) * (float)(SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US);
  pulse_us = constrain(pulse_us, 500.0f, 2500.0f);

  uint32_t duty = (uint32_t)(((uint64_t)pulse_us * 16384ULL) / 20000ULL);
  ledcWrite(SERVO_PITCH_CHANNEL, duty);
  pwmActive = true;
}

void releaseServos() {
  ledcWrite(SERVO_YAW_CHANNEL, 0);
  ledcWrite(SERVO_PITCH_CHANNEL, 0);
  pwmActive = false;
}

void writeFirePulseUs(uint32_t pulse_us) {
  pulse_us = constrain(pulse_us, 400, 2500);
  uint32_t duty = (uint32_t)(((uint64_t)pulse_us * 16384ULL) / 20000ULL);
  ledcWrite(FIRE_LEDC_CHANNEL, duty);
}

// ==============================================================================
// Serial CLI Menu (Ground Calibration, Testing & Telemetry)
// ==============================================================================
void processSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') continue;

    switch (c) {
    case 'd':
    case 'D':
      Serial.println("[CMD] Deploy sequence triggered via Serial.");
      startDeploySequence();
      break;

    case 'f':
    case 'F':
      Serial.println("[CMD] Fold sequence triggered via Serial.");
      handleCmdStopFire();
      startFoldSequence();
      break;

    case 'c':
    case 'C':
    case 'z':
    case 'Z':
      if (currentState == GIMBAL_FOLDED || currentState == GIMBAL_FOLDING) {
        Serial.println("[CMD] Yaw zeroing triggered via Serial (disarmed).");
        zeroYawOffset();
      } else {
        Serial.println("[CMD REJECT] Gimbal is currently armed! Fold/disarm first ('f') to re-zero.");
      }
      break;

    case 'g':
    case 'G':
      runGroundCalibration();
      break;

    case 'i':
    case 'I':
      scanI2CBus();
      break;

    case ' ':
    case 'x':
    case 'X':
      if (!isFiring) {
        Serial.println("[CMD] Fire pulse START (2.0s safety watchdog active).");
        handleCmdFire(true);
      } else {
        Serial.println("[CMD] Fire pulse STOP.");
        handleCmdStopFire();
      }
      break;

    case 't':
    case 'T':
      telemetryStreamActive = !telemetryStreamActive;
      if (telemetryStreamActive) rawDiagStreamActive = false;
      Serial.printf("[CMD] Telemetry stream %s.\n", telemetryStreamActive ? "ENABLED" : "DISABLED");
      break;

    case 'm':
    case 'M':
      rawDiagStreamActive = !rawDiagStreamActive;
      if (rawDiagStreamActive) telemetryStreamActive = false;
      Serial.printf("[CMD] Raw 6-Axis Gravity Diagnostic %s.\n", rawDiagStreamActive ? "ENABLED" : "DISABLED");
      break;

    case 'r':
    case 'R':
      rcModeActive = !rcModeActive;
      if (rcModeActive) {
        rcTargetYaw = currentYaw;
        rcTargetPitch = currentPitch;
        Serial.printf("[CMD] RC Mode ENABLED. Manual joystick/API active (Yaw: %.1f°, Pitch: %.1f°).\n", rcTargetYaw, rcTargetPitch);
      } else {
        filteredTargetYaw = currentYaw;
        filteredTargetPitch = currentPitch;
        Serial.println("[CMD] RC Mode DISABLED. Head-tracking IMU active.");
      }
      break;

    case 'b':
    case 'B':
      applyWirelessCommMode(cfgCommMode == COMM_MODE_BLE ? COMM_MODE_WIFI : COMM_MODE_BLE, true);
      break;

    case 'w':
    case 'W':
      Serial.println("\n------------------------------------------------------------");
      Serial.println("  WIRELESS (BLE / WI-FI) & WEBSERVER STATUS");
      Serial.println("------------------------------------------------------------");
      Serial.printf("  Active Mode: %s (Press 'b' to toggle)\n",
                    cfgCommMode == COMM_MODE_BLE ? "BLE ONLY (Default - leds.html)" : "WI-FI SoftAP + BLE");
      Serial.printf("  BLE Status:  %s (Name: Suit-Gimbal-Hub)\n",
                    bleClientConnected ? "CONNECTED" : "ADVERTISING");
      Serial.printf("  SSID:        %s\n", WIFI_AP_SSID);
      Serial.printf("  Password:    %s\n", WIFI_AP_PASS);
      Serial.printf("  IP Address:  http://%s\n", WiFi.softAPIP().toString().c_str());
      Serial.printf("  Channel:     %d\n", WIFI_AP_CHANNEL);
      Serial.printf("  Connected Clients: %d\n", WiFi.softAPgetStationNum());
      Serial.printf("  Raw ADC (GPIO 4):  %d (0-4095) | IDF Raw: %d\n", analogRead(PIN_BATTERY_ADC), adc1_get_raw(ADC1_CHANNEL_4));
      Serial.printf("  Pin Voltage:       %d mV (IDF Cal: %d mV)\n", analogReadMilliVolts(PIN_BATTERY_ADC), getBatteryPinMv());
      Serial.printf("  Gimbal Vbat:       %.2fV (%d mV)\n", gimbalVbatFilteredMv / 1000.0f, (int)gimbalVbatFilteredMv);
      Serial.printf("  Webshooter:        %s (Vbat: %.2fV)\n",
                    (millis() - lastRemoteTelemetryMs < 2500 && lastRemoteTelemetryMs > 0) ? "CONNECTED" : "OFFLINE",
                    remoteWebshooter.vbat_mv / 1000.0f);
      Serial.println("------------------------------------------------------------\n");
      break;

    case 'h':
    case '?':
      Serial.println("\n------------------------------------------------------------");
      Serial.println("  GIMBAL SERIAL COMMAND MENU");
      Serial.println("------------------------------------------------------------");
      Serial.println("  Workflow:");
      Serial.println("    1. 'g'     : Ground Calibration (once on desk, saves to Flash)");
      Serial.println("    2. 'z'/'c' : Zero relative Yaw (Re-zero ANY time while disarmed! Face forward)");
      Serial.println("    3. 'd'     : Deploy / Arm (Simultaneous unfolding chasing head position)");
      Serial.println("    4. 'f'     : Fold / Disarm (Simultaneous retracting to 0°, 0°)");
      Serial.println("  Utilities:");
      Serial.println("    't'    : Toggle real-time telemetry stream");
      Serial.println("    'm'    : Toggle Raw 6-Axis Gravity Diagnostic (Shows which face points to ground)");
      Serial.println("    'i'    : Scan I2C bus (Check wiring for 0x68 and 0x69)");
      Serial.println("    SPACE  : Toggle 25W Electromagnet Fire pulse");
      Serial.println("    'h'    : Print this help menu");
      Serial.println("------------------------------------------------------------\n");
      break;

    default:
      break;
    }
  }
}

// ==============================================================================
// Smooth Kinematic Easing (SmootherStep 5th-order polynomial)
// ==============================================================================
float smootherStep(float t) {
  t = constrain(t, 0.0f, 1.0f);
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// ==============================================================================
// Battery Voltage ADC Sampling (GPIO 2 - 100k:100k Divider)
// ==============================================================================
void updateBatteryAdc() {
  unsigned long now = millis();
  if (now - lastBatterySampleMs >= BATTERY_SAMPLE_INTERVAL_MS) {
    lastBatterySampleMs = now;
    uint32_t pinMv = getBatteryPinMv();
    uint32_t rawMv = (uint32_t)(pinMv * BATTERY_DIVIDER_RATIO);
    if (gimbalVbatFilteredMv < 100.0f) {
      gimbalVbatFilteredMv = rawMv;
    } else {
      gimbalVbatFilteredMv = gimbalVbatFilteredMv * 0.90f + (float)rawMv * 0.10f;
    }
  }
}

// ==============================================================================
// BLE GATT Server & Wireless Mode Switcher (BLE Default <-> Wi-Fi SoftAP)
// ==============================================================================
String buildBleStatusJson() {
  unsigned long now = millis();
  bool wsConnected = (now - lastRemoteTelemetryMs < 2500) && (lastRemoteTelemetryMs > 0);
  bool chestOnline = (lastLedNodeSeenMs[NODE_CHEST] > 0) && (now - lastLedNodeSeenMs[NODE_CHEST] < 3500);
  bool backOnline  = (lastLedNodeSeenMs[NODE_BACK] > 0) && (now - lastLedNodeSeenMs[NODE_BACK] < 3500);
  uint8_t effSuitState = getEffectiveSuitLedState(now);

  const char* stateStr = (currentState == GIMBAL_FOLDED)    ? "FOLDED" :
                         (currentState == GIMBAL_DEPLOYING) ? "DEPLOYING" :
                         (currentState == GIMBAL_TRACKING)  ? "TRACKING" : "FOLDING";

  String json = "{";
  json += "\"node\":\"GIMBAL\",";
  json += "\"comm\":" + String(cfgCommMode) + ",";
  json += "\"g\":{\"st\":\"" + String(stateStr) + "\",\"v\":" + String((int)gimbalVbatFilteredMv) +
          ",\"y\":" + String(currentYaw, 1) + ",\"p\":" + String(currentPitch, 1) +
          ",\"nk\":" + String(imuNeck.isConnected ? 1 : 0) +
          ",\"sh\":" + String(imuShoulder.isConnected ? 1 : 0) +
          ",\"z\":" + String(hasBeenZeroed ? 1 : 0) +
          ",\"rc\":" + String(rcModeActive ? 1 : 0) +
          ",\"f\":" + String(isFiring ? 1 : 0) + "},";
  json += "\"ws\":{\"c\":" + String(wsConnected ? 1 : 0) +
          ",\"v\":" + String(remoteWebshooter.vbat_mv) +
          ",\"pos\":" + String(remoteWebshooter.webshooter_pos) +
          ",\"arm\":" + String(remoteWebshooter.gimbal_armed ? 1 : 0) +
          ",\"f\":" + String(remoteWebshooter.gimbal_firing ? 1 : 0) + "},";
  json += "\"m\":{\"c\":" + String(chestOnline ? 1 : 0) +
          ",\"b\":" + String(backOnline ? 1 : 0) +
          ",\"g\":1},";
  json += "\"s\":{\"sr\":" + String(cfgStandbyR) + ",\"sg\":" + String(cfgStandbyG) + ",\"sb\":" + String(cfgStandbyB) +
          ",\"dr\":" + String(cfgDeployedR) + ",\"dg\":" + String(cfgDeployedG) + ",\"db\":" + String(cfgDeployedB) +
          ",\"pat\":" + String(cfgLedPattern) +
          ",\"apat\":" + String(cfgArmedPattern) +
          ",\"wm\":" + String(cfgWaveMode) +
          ",\"wo\":" + String(cfgWaveOffsetPct) +
          ",\"min\":" + String(cfgMinBrightness) +
          ",\"max\":" + String(cfgMaxBrightness) +
          ",\"per\":" + String(cfgBreathPeriodMs) +
          ",\"aper\":" + String(cfgArmedPeriodMs) +
          ",\"ov\":" + String(cfgAppStateOverride) +
          ",\"st\":" + String(effSuitState) +
          ",\"al\":" + String(cfgTrackingAlpha, 2) +
          ",\"sp\":" + String((int)cfgMaxSpeedDps) +
          ",\"dms\":" + String(cfgDeployDurationMs) +
          ",\"fms\":" + String(cfgFoldDurationMs) + "}";
  json += "}";
  return json;
}

void handleBleCommandString(const String &cmdStr) {
  lastBleClientActiveMs = millis();

  if (cmdStr.startsWith("L:")) {
    // Format: L:sr,sg,sb,dr,dg,db,pat,apat,wm,wo,min,max,per,aper,ov
    int vals[15] = {0};
    int idx = 0;
    int start = 2;
    while (idx < 15 && start <= (int)cmdStr.length()) {
      int comma = cmdStr.indexOf(',', start);
      if (comma == -1) comma = cmdStr.length();
      vals[idx++] = cmdStr.substring(start, comma).toInt();
      start = comma + 1;
    }
    if (idx >= 14) {
      cfgStandbyR = constrain(vals[0], 0, 255);
      cfgStandbyG = constrain(vals[1], 0, 255);
      cfgStandbyB = constrain(vals[2], 0, 255);
      cfgDeployedR = constrain(vals[3], 0, 255);
      cfgDeployedG = constrain(vals[4], 0, 255);
      cfgDeployedB = constrain(vals[5], 0, 255);
      cfgLedPattern = constrain(vals[6], 0, 8);
      cfgStealthMode = (cfgLedPattern == PATTERN_STEALTH_OFF);
      if (!cfgStealthMode) cfgLastActivePattern = cfgLedPattern;
      cfgArmedPattern = constrain(vals[7], 0, 8);
      cfgWaveMode = constrain(vals[8], 0, 2);
      cfgWaveOffsetPct = constrain(vals[9], 0, 50);
      cfgMinBrightness = constrain(vals[10], 0, 255);
      cfgMaxBrightness = constrain(vals[11], 1, 255);
      cfgBreathPeriodMs = constrain(vals[12], 300, 8000);
      cfgArmedPeriodMs = constrain(vals[13], 250, 5000);
      if (idx >= 15) {
        cfgAppStateOverride = (vals[14] >= 0 && vals[14] <= 2) ? (uint8_t)vals[14] : 0xFF;
      }

      prefs.begin(NVS_NAMESPACE, false);
      prefs.putUChar("sr", cfgStandbyR);
      prefs.putUChar("sg", cfgStandbyG);
      prefs.putUChar("sb", cfgStandbyB);
      prefs.putUChar("dr", cfgDeployedR);
      prefs.putUChar("dg", cfgDeployedG);
      prefs.putUChar("db", cfgDeployedB);
      prefs.putUChar("pattern", cfgLedPattern);
      prefs.putUChar("arm_pat", cfgArmedPattern);
      prefs.putUChar("wave_m", cfgWaveMode);
      prefs.putUChar("wave_o", cfgWaveOffsetPct);
      prefs.putBool("stealth", cfgStealthMode);
      prefs.putUChar("min_b", cfgMinBrightness);
      prefs.putUChar("max_b", cfgMaxBrightness);
      prefs.putUShort("period", cfgBreathPeriodMs);
      prefs.putUShort("arm_per", cfgArmedPeriodMs);
      prefs.end();

      broadcastLedSync();
      Serial.printf("[BLE CMD] Suit LEDs Synced: Standby(%d,%d,%d) Pat:%d | Armed(%d,%d,%d) ArmPat:%d | Wave:%d (%d%%)\n",
                    cfgStandbyR, cfgStandbyG, cfgStandbyB, cfgLedPattern,
                    cfgDeployedR, cfgDeployedG, cfgDeployedB, cfgArmedPattern,
                    cfgWaveMode, cfgWaveOffsetPct);
    }
  } else if (cmdStr.startsWith("mot:")) {
    // Format: mot:alpha,speed,dep_ms,fold_ms
    int c1 = cmdStr.indexOf(',', 4);
    int c2 = cmdStr.indexOf(',', c1 + 1);
    int c3 = cmdStr.indexOf(',', c2 + 1);
    if (c1 > 0 && c2 > 0 && c3 > 0) {
      cfgTrackingAlpha = constrain(cmdStr.substring(4, c1).toFloat(), 0.03f, 0.60f);
      cfgMaxSpeedDps = constrain(cmdStr.substring(c1 + 1, c2).toFloat(), 50.0f, 600.0f);
      cfgDeployDurationMs = constrain(cmdStr.substring(c2 + 1, c3).toInt(), 300, 3000);
      cfgFoldDurationMs = constrain(cmdStr.substring(c3 + 1).toInt(), 300, 3000);
      prefs.begin(NVS_NAMESPACE, false);
      prefs.putFloat("alpha", cfgTrackingAlpha);
      prefs.putFloat("speed", cfgMaxSpeedDps);
      prefs.putUShort("dep_ms", cfgDeployDurationMs);
      prefs.putUShort("fold_ms", cfgFoldDurationMs);
      prefs.end();
    }
  } else if (cmdStr.startsWith("rc:")) {
    // Format: rc:yaw,pitch
    int comma = cmdStr.indexOf(',', 3);
    if (comma > 0) {
      rcModeActive = true;
      rcTargetYaw = constrain(cmdStr.substring(3, comma).toFloat(), SERVO_YAW_MIN_DEG, SERVO_YAW_MAX_DEG);
      rcTargetPitch = constrain(cmdStr.substring(comma + 1).toFloat(), SERVO_PITCH_MIN_DEG, SERVO_PITCH_MAX_DEG);
      if (currentState == GIMBAL_FOLDED) {
        startDeploySequence();
      }
    }
  } else if (cmdStr == "cmd:deploy") {
    startDeploySequence();
  } else if (cmdStr == "cmd:fold") {
    handleCmdStopFire();
    startFoldSequence();
  } else if (cmdStr == "cmd:zero") {
    if (currentState == GIMBAL_FOLDED || currentState == GIMBAL_FOLDING) {
      zeroYawOffset();
    }
  } else if (cmdStr == "cmd:fire_start") {
    handleCmdFire(true);
  } else if (cmdStr == "cmd:fire_stop") {
    handleCmdStopFire();
  } else if (cmdStr == "cmd:calibrate_ground") {
    runGroundCalibration();
  } else if (cmdStr == "cmd:rc_mode_1") {
    rcModeActive = true;
    rcTargetYaw = currentYaw;
    rcTargetPitch = currentPitch;
  } else if (cmdStr == "cmd:rc_mode_0") {
    rcModeActive = false;
    filteredTargetYaw = currentYaw;
    filteredTargetPitch = currentPitch;
  } else if (cmdStr == "cmd:rc_center") {
    rcModeActive = true;
    rcTargetYaw = SERVO_YAW_DEPLOYED_DEG;
    rcTargetPitch = SERVO_PITCH_DEPLOYED_DEG;
  } else if (cmdStr == "cmd:comm_ble") {
    pendingCommModeSwitch = COMM_MODE_BLE;
    pendingCommSwitchAtMs = millis() + 250;
  } else if (cmdStr == "cmd:comm_wifi") {
    pendingCommModeSwitch = COMM_MODE_WIFI;
    pendingCommSwitchAtMs = millis() + 250;
  }
}

class GimbalBleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    bleClientConnected = true;
    Serial.println("[BLE] Web Bluetooth Client Connected to Suit-Gimbal-Hub!");
  }
  void onDisconnect(BLEServer* pServer) override {
    bleClientConnected = false;
    if (rcModeActive) {
      rcModeActive = false;
      filteredTargetYaw = currentYaw;
      filteredTargetPitch = currentPitch;
    }
    Serial.println("[BLE] Web Bluetooth Client Disconnected -> Restarting Advertising...");
    BLEDevice::startAdvertising();
  }
};

class GimbalBleCmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    std::string rxValue = pCharacteristic->getValue();
    if (!rxValue.empty()) {
      pendingBleCommand = String(rxValue.c_str());
    }
  }
};

void initBleGattServer() {
  BLEDevice::init("Suit-Gimbal-Hub");
  BLEDevice::setMTU(517);

  pBleServer = BLEDevice::createServer();
  pBleServer->setCallbacks(new GimbalBleServerCallbacks());

  BLEService *pService = pBleServer->createService(SUIT_BLE_SERVICE_UUID);

  pStatusChar = pService->createCharacteristic(
      SUIT_BLE_CHAR_STATUS_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pStatusChar->addDescriptor(new BLE2902());

  pCmdChar = pService->createCharacteristic(
      SUIT_BLE_CHAR_CMD_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pCmdChar->setCallbacks(new GimbalBleCmdCallbacks());

  String initJson = buildBleStatusJson();
  pStatusChar->setValue(initJson.c_str());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SUIT_BLE_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  Serial.printf("[BLE GATT] Advertising as 'Suit-Gimbal-Hub' (Service: %s)\n", SUIT_BLE_SERVICE_UUID);
}

void applyWirelessCommMode(uint8_t mode, bool saveToNvs) {
  cfgCommMode = (mode == COMM_MODE_WIFI) ? COMM_MODE_WIFI : COMM_MODE_BLE;
  if (saveToNvs) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUChar("comm_v2", cfgCommMode);
    prefs.end();
  }

  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);

  if (cfgCommMode == COMM_MODE_WIFI) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    bool apStarted = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_CHANNEL, 0, 4);
    esp_wifi_set_channel(WIFI_AP_CHANNEL, WIFI_SECOND_CHAN_NONE);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    if (!webServerStarted) {
      setupWebServer();
      webServerStarted = true;
    }
    Serial.printf("\n[WIRELESS MODE] WI-FI AP + BLE ACTIVE! SSID: '%s' | Pass: '%s' | http://%s\n",
                  WIFI_AP_SSID, WIFI_AP_PASS, WiFi.softAPIP().toString().c_str());
    (void)apStarted;
  } else {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(WIFI_AP_CHANNEL, WIFI_SECOND_CHAN_NONE);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    Serial.println("\n[WIRELESS MODE] BLE ONLY MODE ACTIVE (Default - Wi-Fi SoftAP OFF | BLE GATT + ESP-NOW Ch 1 ON)");
  }

  // Re-bind broadcast peer to active interface
  uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_del_peer(broadcastAddr);

  esp_now_peer_info_t peerInfo = {};
  memset(&peerInfo, 0, sizeof(peerInfo));
  memset(peerInfo.peer_addr, 0xFF, 6);
  peerInfo.channel = (cfgCommMode == COMM_MODE_WIFI) ? 0 : WIFI_AP_CHANNEL;
  peerInfo.encrypt = false;
  peerInfo.ifidx = (cfgCommMode == COMM_MODE_WIFI) ? WIFI_IF_AP : WIFI_IF_STA;
  esp_now_add_peer(&peerInfo);

  if (pBleServer != nullptr && !bleClientConnected) {
    BLEDevice::startAdvertising();
  }
}

// ==============================================================================
// Embedded Tactical Web Server & REST API Handlers
// ==============================================================================
void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/status", HTTP_GET, []() {
    lastWebClientActiveMs = millis();
    bool wsConnected = (millis() - lastRemoteTelemetryMs < 2500) && (lastRemoteTelemetryMs > 0);
    uint32_t wsAge = (lastRemoteTelemetryMs > 0) ? (millis() - lastRemoteTelemetryMs) : 99999;

    const char* stateStr = (currentState == GIMBAL_FOLDED)    ? "FOLDED" :
                           (currentState == GIMBAL_DEPLOYING) ? "DEPLOYING" :
                           (currentState == GIMBAL_TRACKING)  ? "TRACKING" : "FOLDING";

    String json = "{";
    // Gimbal object
    json += "\"gimbal\":{";
    json += "\"state\":\"" + String(stateStr) + "\",";
    json += "\"vbat_mv\":" + String((int)gimbalVbatFilteredMv) + ",";
    json += "\"yaw\":" + String(currentYaw, 1) + ",";
    json += "\"pitch\":" + String(currentPitch, 1) + ",";
    json += "\"neck_ok\":" + String(imuNeck.isConnected ? "true" : "false") + ",";
    json += "\"shoulder_ok\":" + String(imuShoulder.isConnected ? "true" : "false") + ",";
    json += "\"zeroed\":" + String(hasBeenZeroed ? "true" : "false") + ",";
    json += "\"rc_mode\":" + String(rcModeActive ? "true" : "false") + ",";
    json += "\"rc_yaw\":" + String(rcTargetYaw, 1) + ",";
    json += "\"rc_pitch\":" + String(rcTargetPitch, 1) + ",";
    json += "\"firing\":" + String(isFiring ? "true" : "false");
    json += "},";

    // Webshooter object
    json += "\"webshooter\":{";
    json += "\"connected\":" + String(wsConnected ? "true" : "false") + ",";
    json += "\"vbat_mv\":" + String(remoteWebshooter.vbat_mv) + ",";
    json += "\"pos\":" + String(remoteWebshooter.webshooter_pos) + ",";
    json += "\"armed\":" + String(remoteWebshooter.gimbal_armed ? "true" : "false") + ",";
    json += "\"firing\":" + String(remoteWebshooter.gimbal_firing ? "true" : "false") + ",";
    json += "\"last_seen_ms\":" + String(wsAge);
    json += "},";

    // Suit LED Mesh Nodes (Chest = 1, Back = 2, Helmet = 3, Aux = 4)
    bool chestOnline = (lastLedNodeSeenMs[NODE_CHEST] > 0) && (millis() - lastLedNodeSeenMs[NODE_CHEST] < 3500);
    bool backOnline  = (lastLedNodeSeenMs[NODE_BACK] > 0) && (millis() - lastLedNodeSeenMs[NODE_BACK] < 3500);
    json += "\"mesh_nodes\":{";
    json += "\"chest\":{\"online\":" + String(chestOnline ? "true" : "false") + ",\"vbat_mv\":" + String(remoteLedNodes[NODE_CHEST].vbat_mv) + ",\"leds\":" + String(remoteLedNodes[NODE_CHEST].num_leds) + "},";
    json += "\"back\":{\"online\":" + String(backOnline ? "true" : "false") + ",\"vbat_mv\":" + String(remoteLedNodes[NODE_BACK].vbat_mv) + ",\"leds\":" + String(remoteLedNodes[NODE_BACK].num_leds) + "}";
    json += "},";

    // Settings object
    uint8_t effSuitState = getEffectiveSuitLedState(millis());
    json += "\"settings\":{";
    json += "\"sr\":" + String(cfgStandbyR) + ",";
    json += "\"sg\":" + String(cfgStandbyG) + ",";
    json += "\"sb\":" + String(cfgStandbyB) + ",";
    json += "\"dr\":" + String(cfgDeployedR) + ",";
    json += "\"dg\":" + String(cfgDeployedG) + ",";
    json += "\"db\":" + String(cfgDeployedB) + ",";
    json += "\"pattern\":" + String(cfgLedPattern) + ",";
    json += "\"arm_pat\":" + String(cfgArmedPattern) + ",";
    json += "\"wave_mode\":" + String(cfgWaveMode) + ",";
    json += "\"wave_off\":" + String(cfgWaveOffsetPct) + ",";
    json += "\"min_b\":" + String(cfgMinBrightness) + ",";
    json += "\"max_b\":" + String(cfgMaxBrightness) + ",";
    json += "\"period\":" + String(cfgBreathPeriodMs) + ",";
    json += "\"arm_per\":" + String(cfgArmedPeriodMs) + ",";
    json += "\"override_state\":" + String(cfgAppStateOverride) + ",";
    json += "\"suit_state\":" + String(effSuitState) + ",";
    json += "\"comm_mode\":" + String(cfgCommMode) + ",";
    json += "\"ble_connected\":" + String(bleClientConnected ? "true" : "false") + ",";
    json += "\"alpha\":" + String(cfgTrackingAlpha, 2) + ",";
    json += "\"speed\":" + String(cfgMaxSpeedDps, 1);
    json += "}";
    json += "}";

    server.send(200, "application/json", json);
  });

  server.on("/api/cmd", HTTP_POST, []() {
    lastWebClientActiveMs = millis();
    if (!server.hasArg("action")) {
      server.send(400, "application/json", "{\"error\":\"Missing action\"}");
      return;
    }
    String act = server.arg("action");
    if (act == "deploy") {
      startDeploySequence();
      server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Deploy sequence initiated\"}");
    } else if (act == "fold") {
      handleCmdStopFire();
      startFoldSequence();
      server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Fold sequence initiated\"}");
    } else if (act == "zero") {
      if (currentState == GIMBAL_FOLDED || currentState == GIMBAL_FOLDING) {
        zeroYawOffset();
        server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Sensors zeroed to horizon!\"}");
      } else {
        server.send(400, "application/json", "{\"error\":\"Gimbal must be folded to zero\"}");
      }
    } else if (act == "fire_start") {
      handleCmdFire(true);
      server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Firing active!\"}");
    } else if (act == "fire_stop") {
      handleCmdStopFire();
      server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Fire stopped\"}");
    } else if (act == "calibrate_ground") {
      runGroundCalibration();
      server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Ground calibration complete\"}");
    } else if (act == "rc_toggle") {
      rcModeActive = !rcModeActive;
      if (rcModeActive) {
        rcTargetYaw = currentYaw;
        rcTargetPitch = currentPitch;
      } else {
        filteredTargetYaw = currentYaw;
        filteredTargetPitch = currentPitch;
      }
      server.send(200, "application/json", String("{\"status\":\"ok\",\"rc_mode\":") + (rcModeActive ? "true" : "false") + "}");
    } else if (act == "rc_mode") {
      if (server.hasArg("enabled")) {
        bool en = (server.arg("enabled") == "1" || server.arg("enabled") == "true");
        rcModeActive = en;
        if (rcModeActive) {
          rcTargetYaw = currentYaw;
          rcTargetPitch = currentPitch;
        } else {
          filteredTargetYaw = currentYaw;
          filteredTargetPitch = currentPitch;
        }
      }
      server.send(200, "application/json", String("{\"status\":\"ok\",\"rc_mode\":") + (rcModeActive ? "true" : "false") + "}");
    } else if (act == "rc_target") {
      lastWebClientActiveMs = millis();
      rcModeActive = true;
      if (server.hasArg("yaw")) rcTargetYaw = server.arg("yaw").toFloat();
      if (server.hasArg("pitch")) rcTargetPitch = server.arg("pitch").toFloat();
      if (server.hasArg("d_yaw")) rcTargetYaw += server.arg("d_yaw").toFloat();
      if (server.hasArg("d_pitch")) rcTargetPitch += server.arg("d_pitch").toFloat();
      rcTargetYaw = constrain(rcTargetYaw, SERVO_YAW_MIN_DEG, SERVO_YAW_MAX_DEG);
      rcTargetPitch = constrain(rcTargetPitch, SERVO_PITCH_MIN_DEG, SERVO_PITCH_MAX_DEG);

      // Auto-deploy if user commands RC joystick while folded
      if (currentState == GIMBAL_FOLDED) {
        startDeploySequence();
      }
      server.send(200, "application/json", "{\"status\":\"ok\"}");
    } else if (act == "rc_center") {
      lastWebClientActiveMs = millis();
      rcModeActive = true;
      rcTargetYaw = SERVO_YAW_DEPLOYED_DEG;
      rcTargetPitch = SERVO_PITCH_DEPLOYED_DEG;
      server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Turret re-centered\"}");
    } else if (act == "comm_mode") {
      int m = server.hasArg("mode") ? server.arg("mode").toInt() : (cfgCommMode == COMM_MODE_BLE ? COMM_MODE_WIFI : COMM_MODE_BLE);
      pendingCommModeSwitch = (m == 1) ? COMM_MODE_WIFI : COMM_MODE_BLE;
      pendingCommSwitchAtMs = millis() + 350;
      String msg = (pendingCommModeSwitch == COMM_MODE_BLE)
                       ? "Switching to BLE Mode (Wi-Fi AP turning OFF)..."
                       : "Wi-Fi AP + BLE Mode Active";
      server.send(200, "application/json",
                  String("{\"status\":\"ok\",\"comm_mode\":") + pendingCommModeSwitch + ",\"message\":\"" + msg + "\"}");
    } else {
      server.send(400, "application/json", "{\"error\":\"Unknown action\"}");
    }
  });

  server.on("/api/settings", HTTP_POST, []() {
    if (!server.hasArg("action")) {
      server.send(400, "application/json", "{\"error\":\"Missing action\"}");
      return;
    }
    String act = server.arg("action");
    if (act == "led") {
      if (server.hasArg("sr")) cfgStandbyR = server.arg("sr").toInt();
      if (server.hasArg("sg")) cfgStandbyG = server.arg("sg").toInt();
      if (server.hasArg("sb")) cfgStandbyB = server.arg("sb").toInt();
      if (server.hasArg("dr")) cfgDeployedR = server.arg("dr").toInt();
      if (server.hasArg("dg")) cfgDeployedG = server.arg("dg").toInt();
      if (server.hasArg("db")) cfgDeployedB = server.arg("db").toInt();
      if (server.hasArg("pattern")) {
        cfgLedPattern = server.arg("pattern").toInt();
        cfgStealthMode = (cfgLedPattern == PATTERN_STEALTH_OFF);
        if (!cfgStealthMode) {
          cfgLastActivePattern = cfgLedPattern;
        }
      }
      if (server.hasArg("arm_pat")) cfgArmedPattern = server.arg("arm_pat").toInt();
      if (server.hasArg("wave_mode")) cfgWaveMode = constrain(server.arg("wave_mode").toInt(), 0, 2);
      if (server.hasArg("wave_off")) cfgWaveOffsetPct = constrain(server.arg("wave_off").toInt(), 0, 50);
      if (server.hasArg("min_b")) cfgMinBrightness = server.arg("min_b").toInt();
      if (server.hasArg("max_b")) cfgMaxBrightness = server.arg("max_b").toInt();
      if (server.hasArg("period")) cfgBreathPeriodMs = constrain(server.arg("period").toInt(), 300, 8000);
      if (server.hasArg("arm_per")) cfgArmedPeriodMs = constrain(server.arg("arm_per").toInt(), 250, 5000);
      if (server.hasArg("override_state")) {
        int ov = server.arg("override_state").toInt();
        cfgAppStateOverride = (ov >= 0 && ov <= 2) ? (uint8_t)ov : 0xFF;
      }

      prefs.begin(NVS_NAMESPACE, false);
      prefs.putUChar("sr", cfgStandbyR);
      prefs.putUChar("sg", cfgStandbyG);
      prefs.putUChar("sb", cfgStandbyB);
      prefs.putUChar("dr", cfgDeployedR);
      prefs.putUChar("dg", cfgDeployedG);
      prefs.putUChar("db", cfgDeployedB);
      prefs.putUChar("pattern", cfgLedPattern);
      prefs.putUChar("arm_pat", cfgArmedPattern);
      prefs.putUChar("wave_m", cfgWaveMode);
      prefs.putUChar("wave_o", cfgWaveOffsetPct);
      prefs.putBool("stealth", cfgStealthMode);
      prefs.putUChar("min_b", cfgMinBrightness);
      prefs.putUChar("max_b", cfgMaxBrightness);
      prefs.putUShort("period", cfgBreathPeriodMs);
      prefs.putUShort("arm_per", cfgArmedPeriodMs);
      prefs.end();

      // Immediately broadcast new LED config & breathing phase to all ESP32s in the suit mesh!
      broadcastLedSync();

      server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Suit LED Mesh Synced & Saved\"}");
    } else if (act == "motion") {
      if (server.hasArg("alpha")) cfgTrackingAlpha = server.arg("alpha").toFloat();
      if (server.hasArg("speed")) cfgMaxSpeedDps = server.arg("speed").toFloat();
      if (server.hasArg("deploy_ms")) cfgDeployDurationMs = server.arg("deploy_ms").toInt();
      if (server.hasArg("fold_ms")) cfgFoldDurationMs = server.arg("fold_ms").toInt();

      prefs.begin(NVS_NAMESPACE, false);
      prefs.putFloat("alpha", cfgTrackingAlpha);
      prefs.putFloat("speed", cfgMaxSpeedDps);
      prefs.putUShort("dep_ms", cfgDeployDurationMs);
      prefs.putUShort("fold_ms", cfgFoldDurationMs);
      prefs.end();

      server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Motion settings saved\"}");
    } else {
      server.send(400, "application/json", "{\"error\":\"Unknown action\"}");
    }
  });

  server.begin();
}
