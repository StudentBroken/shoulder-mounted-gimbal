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

// Web Server & Dynamic Settings
WebServer server(WEB_SERVER_PORT);
unsigned long lastWebClientActiveMs = 0;

// Battery Monitoring (GPIO 2, 100k-100k Divider)
float gimbalVbatFilteredMv = 0.0f;
unsigned long lastBatterySampleMs = 0;

// Remote Webshooter Telemetry Cache
WebshooterTelemetry remoteWebshooter = {0};
unsigned long lastRemoteTelemetryMs = 0;

// Dynamic LED & Kinematics Settings (Configurable via Web UI & Persisted to Flash)
uint8_t cfgStandbyR = 255, cfgStandbyG = 255, cfgStandbyB = 255;
uint8_t cfgDeployedR = 0, cfgDeployedG = 60, cfgDeployedB = 255; // Tactical Blue when deployed

uint8_t cfgMinBrightness = RGB_MIN_BRIGHTNESS;
uint8_t cfgMaxBrightness = RGB_MAX_BRIGHTNESS;
uint16_t cfgBreathPeriodMs = RGB_BREATH_PERIOD_MS;

float cfgTrackingAlpha = TRACKING_SMOOTH_ALPHA;
float cfgMaxSpeedDps = TRACKING_MAX_SPEED_DPS;
uint16_t cfgDeployDurationMs = DEPLOY_DURATION_MS;
uint16_t cfgFoldDurationMs = FOLD_DURATION_MS;

void setupWebServer();
void updateBatteryAdc();

// Loop Timers
unsigned long lastLoopTimeUs = 0;
unsigned long lastTelemetryTimeMs = 0;
bool telemetryStreamActive = false;

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
void triggerRgbFlash(uint8_t r, uint8_t g, uint8_t b, uint16_t duration_ms);

void startDeploySequence();
void startFoldSequence();
void updateGimbalStateMachine();
void updateTracking(float dt);
void updateRcControl(float dt);

void handleCmdFire(bool force = false);
void handleCmdStopFire();

void onEspNowRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len);
void processSerialCommands();
float smootherStep(float t);

// Breathing Synchronization with Webshooter
int32_t breathPhaseOffset = 0;
bool hasSyncedPhase = false;

// ==============================================================================
// ESP-NOW Receive Callback (Slave)
// ==============================================================================
void onEspNowRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
  if (len == sizeof(WebshooterTelemetry) && incomingData[0] == CMD_TELEMETRY) {
    memcpy(&remoteWebshooter, incomingData, sizeof(WebshooterTelemetry));
    lastRemoteTelemetryMs = millis();

    // Synchronize Apple breathing rhythm with Webshooter
    int32_t currentGimbalPhase = (int32_t)(millis() % cfgBreathPeriodMs);
    int32_t targetPhase = (int32_t)remoteWebshooter.breath_phase_ms;
    int32_t targetOffset = targetPhase - currentGimbalPhase;
    while (targetOffset > (int32_t)cfgBreathPeriodMs / 2) targetOffset -= cfgBreathPeriodMs;
    while (targetOffset < -(int32_t)cfgBreathPeriodMs / 2) targetOffset += cfgBreathPeriodMs;
    if (!hasSyncedPhase) {
      breathPhaseOffset = targetOffset;
      hasSyncedPhase = true;
    } else {
      breathPhaseOffset += (int32_t)((targetOffset - breathPhaseOffset) * 0.25f);
    }
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

void updateRgbLed() {
  unsigned long now = millis();

  // Rate-limit ALL WS2812 updates to 50Hz (every 20ms)
  // This guarantees the WS2812 data line stays LOW for ~20ms between frames,
  // providing the necessary >50us reset latch pulse for all modes including firing and flash!
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

  // 2. Firing mode: Solid bright intense red immediately on both barrels!
  if (isFiring) {
    ws2812WriteBoth(255, 0, 0);
    return;
  }

  // 3. Apple Sleep Breathing Waveform calculation (Locked in phase with Webshooter):
  //    f(x) = (exp(sin(phase)) - 1/e) * 255 / (e - 1/e)
  //    phase = 2*PI * (syncedTime % period) / period - PI/2
  int32_t rawSynced = (int32_t)(now % cfgBreathPeriodMs) + breathPhaseOffset;
  while (rawSynced < 0) rawSynced += cfgBreathPeriodMs;
  uint32_t syncedTime = (uint32_t)(rawSynced % cfgBreathPeriodMs);

  float phase = (2.0f * (float)PI * (float)syncedTime / (float)cfgBreathPeriodMs) - (0.5f * (float)PI);
  float rawNorm = (expf(sinf(phase)) - 0.36787944f) / 2.3504024f; // Normalized 0.0 to 1.0
  rawNorm = constrain(rawNorm, 0.0f, 1.0f);

  uint8_t index = (uint8_t)(rawNorm * 255.0f);
  uint8_t correctedBrightness = pgm_read_byte(&cie1931_table[index]);

  // Scale between minimum floor (never drops to 0%, always glowing) and maximum brightness
  uint8_t level = cfgMinBrightness + (uint8_t)(((uint32_t)correctedBrightness * (cfgMaxBrightness - cfgMinBrightness)) / 255);


  // 4. Color states for Dual Barrel LEDs (Left & Right):
  //    - Deployed (Tracking / Deploying): Dynamic Deployed Color breathing
  //    - Default (Folded / Standby): Dynamic Standby Color breathing
  if (currentState == GIMBAL_TRACKING || currentState == GIMBAL_DEPLOYING) {
    uint8_t r = (uint8_t)(((uint32_t)cfgDeployedR * level) / 255);
    uint8_t g = (uint8_t)(((uint32_t)cfgDeployedG * level) / 255);
    uint8_t b = (uint8_t)(((uint32_t)cfgDeployedB * level) / 255);
    ws2812WriteBoth(r, g, b);
  } else {
    uint8_t r = (uint8_t)(((uint32_t)cfgStandbyR * level) / 255);
    uint8_t g = (uint8_t)(((uint32_t)cfgStandbyG * level) / 255);
    uint8_t b = (uint8_t)(((uint32_t)cfgStandbyB * level) / 255);
    ws2812WriteBoth(r, g, b);
  }
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
  Serial.println("  SHOULDER MOUNTED GIMBAL - 2-AXIS HEAD TRACKER");
  Serial.println("  Fire ESC: GPIO 0 | Yaw: GPIO 1 | Pitch: GPIO 3");
  Serial.println("  Dual MPU6050 (Y+ Fwd, ±90° Roll): Neck 0x69 | Shldr 0x68");
  Serial.println("============================================================");

  // 2. Initialize I2C Bus and both MPU6050 sensors
  initI2CAndIMUs();

  // 3. Load Ground Calibration Offsets from Flash NVS
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

  // 4. Initialize Wi-Fi SoftAP & ESP-NOW Coexistence (Fixed Channel 1)
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  bool apStarted = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_CHANNEL, 0, 4);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  if (apStarted) {
    Serial.printf("[WIFI AP] SoftAP STARTED! SSID: '%s' | Pass: '%s' | IP: %s | Ch: %d\n",
                  WIFI_AP_SSID, WIFI_AP_PASS, WiFi.softAPIP().toString().c_str(), WIFI_AP_CHANNEL);
  } else {
    Serial.println("[WIFI AP] Error: softAP failed to start!");
  }

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.printf("[ESP-NOW] Slave Listening on Wi-Fi Channel %d.\n", WIFI_AP_CHANNEL);
  } else {
    Serial.println("[ESP-NOW] Error: ESP-NOW init failed!");
  }

  // 5. Start Web Server
  setupWebServer();
  Serial.printf("[WEB SERVER] Tactical Dashboard online at: http://%s\n", WiFi.softAPIP().toString().c_str());

  lastLoopTimeUs = micros();
  Serial.println("\n[GIMBAL READY] Gimbal is folded. Press 'h' for serial command menu.");
}

// ==============================================================================
// Main Loop
// ==============================================================================
void loop() {
  // 1a. Handle Web Server Client Requests
  server.handleClient();

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
  updateGimbalStateMachine();

  // 4b. Wi-Fi Disconnect / Heartbeat Failsafe: Auto-default back to Head Tracking
  if (rcModeActive) {
    bool wifiDisconnected = (WiFi.softAPgetStationNum() == 0);
    bool heartbeatTimeout = (lastWebClientActiveMs > 0 && (millis() - lastWebClientActiveMs > 3000));
    if (wifiDisconnected || heartbeatTimeout) {
      rcModeActive = false;
      lastWebClientActiveMs = 0;
      filteredTargetYaw = currentYaw;
      filteredTargetPitch = currentPitch;
      if (!hasBeenZeroed) {
        zeroYawOffset();
      }
      Serial.printf("\n[FAILSAFE] Wi-Fi %s -> Defaulting to HEAD TRACKING mode.\n",
                    wifiDisconnected ? "disconnected (0 clients)" : "heartbeat timeout (>3s)");
    }
  }

  // 5. Update Apple-style WS2812 RGB LED Engine (GPIO 21)
  updateRgbLed();

  // 5. Electromagnet Safety Watchdog (Hard 8.0s maximum limit to protect coil)
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
}

void updateGimbalStateMachine() {
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
      updateRcControl(0.02f);
    } else {
      updateTracking(0.02f);
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
// Active Head Tracking Calculation
// ==============================================================================
void updateTracking(float dt) {
  float rawTargetYaw, rawTargetPitch;
  calculateIntendedAngles(rawTargetYaw, rawTargetPitch);

  // Exponential smoothing filter for fluid, cinematic mimicry without jitter
  filteredTargetYaw += cfgTrackingAlpha * (rawTargetYaw - filteredTargetYaw);
  filteredTargetPitch += cfgTrackingAlpha * (rawTargetPitch - filteredTargetPitch);

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
}

void handleCmdStopFire() {
  isFiring = false;
  writeFirePulseUs(FIRE_PWM_IDLE_US);
}

// ==============================================================================
// Yaw & Pitch Re-zeroing (Eliminates Drift & Posture Offset, Unlocks Arming)
// ==============================================================================
void zeroYawOffset() {
  // Capture current relative yaw and pitch between neck and shoulder while head is facing forward
  relativeYawZeroOffset = imuNeck.yaw - imuShoulder.yaw;
  relativePitchZeroOffset = imuNeck.pitch - imuShoulder.pitch;
  filteredTargetYaw = SERVO_YAW_DEPLOYED_DEG;
  filteredTargetPitch = SERVO_PITCH_DEPLOYED_DEG;
  hasBeenZeroed = true;
  triggerRgbFlash(0, 255, 255, 250); // Cyan flash on zero confirm
  Serial.printf("\n[CALIBRATE] Relative Angles Zeroed: Yaw offset = %+.2f°, Pitch offset = %+.2f°.\n",
                relativeYawZeroOffset, relativePitchZeroOffset);
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

  imuShoulder.gx_bias = shldr_gx_sum / SAMPLES;
  imuShoulder.gy_bias = shldr_gy_sum / SAMPLES;
  imuShoulder.gz_bias = shldr_gz_sum / SAMPLES;
  imuShoulder.pitch_offset = shldr_pitch_sum / SAMPLES;
  imuShoulder.pitch = 0.0f;
  imuShoulder.yaw = 0.0f;

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
  prefs.putFloat("n_gx", imuNeck.gx_bias);
  prefs.putFloat("n_gy", imuNeck.gy_bias);
  prefs.putFloat("n_gz", imuNeck.gz_bias);
  prefs.putFloat("n_poff", imuNeck.pitch_offset);

  prefs.putFloat("s_gx", imuShoulder.gx_bias);
  prefs.putFloat("s_gy", imuShoulder.gy_bias);
  prefs.putFloat("s_gz", imuShoulder.gz_bias);
  prefs.putFloat("s_poff", imuShoulder.pitch_offset);
  prefs.putBool("cal_done", true);
  prefs.end();
}

void loadCalibrationFromNVS() {
  prefs.begin(NVS_NAMESPACE, true);
  bool calDone = prefs.getBool("cal_done", false);

  if (calDone) {
    imuNeck.gx_bias = prefs.getFloat("n_gx", 0.0f);
    imuNeck.gy_bias = prefs.getFloat("n_gy", 0.0f);
    imuNeck.gz_bias = prefs.getFloat("n_gz", 0.0f);
    imuNeck.pitch_offset = prefs.getFloat("n_poff", 0.0f);

    imuShoulder.gx_bias = prefs.getFloat("s_gx", 0.0f);
    imuShoulder.gy_bias = prefs.getFloat("s_gy", 0.0f);
    imuShoulder.gz_bias = prefs.getFloat("s_gz", 0.0f);
    imuShoulder.pitch_offset = prefs.getFloat("s_poff", 0.0f);

    Serial.println("[NVS] Loaded Ground Calibration offsets from Flash.");
  } else {
    Serial.println("[NVS] Notice: No saved ground calibration found. Send 'g' over USB Serial to calibrate on ground.");
  }
  prefs.end();
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

  // Shoulder: +Y forward, rolled +90° around Y
  bool shoulderOk = imuShoulder.begin(IMU_ADDR_SHOULDER, ORIENT_Y_FWD_ROLL_POS90, &Wire);
  if (shoulderOk) {
    Serial.printf("[IMU 0x%02X] Shoulder IMU (+Y Fwd, +90° Roll) initialized.\n", IMU_ADDR_SHOULDER);
  } else {
    Serial.printf("[IMU 0x%02X] ERROR: Shoulder IMU not responding! Check SDA:%d SCL:%d\n",
                  IMU_ADDR_SHOULDER, PIN_I2C_SDA, PIN_I2C_SCL);
  }

  // Neck (Head): +Y forward, rolled -90° around Y
  bool neckOk = imuNeck.begin(IMU_ADDR_NECK, ORIENT_Y_FWD_ROLL_NEG90, &Wire);
  if (neckOk) {
    Serial.printf("[IMU 0x%02X] Neck IMU (+Y Fwd, -90° Roll) initialized.\n", IMU_ADDR_NECK);
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
      Serial.printf("[CMD] Telemetry stream %s.\n", telemetryStreamActive ? "ENABLED" : "DISABLED");
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

    case 'w':
    case 'W':
      Serial.println("\n------------------------------------------------------------");
      Serial.println("  WI-FI & WEBSERVER STATUS");
      Serial.println("------------------------------------------------------------");
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

    // Settings object
    json += "\"settings\":{";
    json += "\"min_b\":" + String(cfgMinBrightness) + ",";
    json += "\"max_b\":" + String(cfgMaxBrightness) + ",";
    json += "\"period\":" + String(cfgBreathPeriodMs) + ",";
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
      if (server.hasArg("min_b")) cfgMinBrightness = server.arg("min_b").toInt();
      if (server.hasArg("max_b")) cfgMaxBrightness = server.arg("max_b").toInt();
      if (server.hasArg("period")) cfgBreathPeriodMs = server.arg("period").toInt();

      prefs.begin(NVS_NAMESPACE, false);
      prefs.putUChar("sr", cfgStandbyR);
      prefs.putUChar("sg", cfgStandbyG);
      prefs.putUChar("sb", cfgStandbyB);
      prefs.putUChar("dr", cfgDeployedR);
      prefs.putUChar("dg", cfgDeployedG);
      prefs.putUChar("db", cfgDeployedB);
      prefs.putUChar("min_b", cfgMinBrightness);
      prefs.putUChar("max_b", cfgMaxBrightness);
      prefs.putUShort("period", cfgBreathPeriodMs);
      prefs.end();

      server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"LED settings saved\"}");
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
