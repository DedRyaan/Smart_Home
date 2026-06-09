// =============================================================================
// ESP32 Matter Smart-Home Attachment
//   - 3 lights + 1 fan via a 4-channel relay (on/off)
//   - Croma AC via IR (on/off toggling via Matter endpoint)
//   - Matter over Wi-Fi protocol for native smart-home integration (Alexa, Apple, Google)
//   - BLE-based native commissioning (onboarding QR code / pairing code on boot)
//   - FreeRTOS-task-driven switch debouncer for existing 2-way wall switches
// Everything runs on the ESP32 itself; no separate server or subscription.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ir_Midea.h>
#include <Matter.h>
#include <ArduinoOTA.h>

#include "config.h"
#include "driver/rmt_tx.h"

// ---------- Globals ----------
Preferences    prefs;
IRMideaAC      acmidea(IR_LED_PIN);

rmt_channel_handle_t tx_channel = NULL;
rmt_encoder_handle_t copy_encoder = NULL;

bool relayState[RELAY_COUNT] = {false, false, false, false};
bool acPower = false;
int  acTemp  = DEFAULT_AC_TEMP;
int  acMode  = AC_MODE_COOL;
int  acFanSpeed = kMideaACFanAuto;

// AC send signal flag (set in async callback, handled in loop())
volatile bool acDirty = false;

// NVS Persistence
bool     stateDirty = false;
uint32_t lastStateChangeMs = 0;
bool     savedRelay[RELAY_COUNT];
bool     savedAcPower;
int      savedAcTemp;
int      savedAcFanSpeed;

// Matter Endpoints
MatterOnOffLight matterLight1;
MatterOnOffLight matterLight2;
MatterOnOffLight matterLight3;
MatterFan        matterFan;
MatterThermostat matterAc;
MatterFan        matterAcFan;
MatterOnOffLight matterOnboardLed;
MatterTemperatureSensor matterTempSensor;

bool onboardLedState = false;

// ---------- Persistence ----------
void markStateDirty() {
  stateDirty = true;
  lastStateChangeMs = millis();
}

void persistNow() {
  if (memcmp(savedRelay, relayState, sizeof(relayState)) != 0) {
    prefs.putBytes("relays", relayState, sizeof(relayState));
    memcpy(savedRelay, relayState, sizeof(relayState));
    Serial.println("[NVS] Relays state persisted.");
  }
  if (savedAcPower != acPower) {
    prefs.putBool("acPower", acPower);
    savedAcPower = acPower;
    Serial.println("[NVS] AC Power persisted.");
  }
  if (savedAcTemp != acTemp) {
    prefs.putInt("acTemp", acTemp);
    savedAcTemp = acTemp;
    Serial.println("[NVS] AC Temp persisted.");
  }
  if (savedAcFanSpeed != acFanSpeed) {
    prefs.putInt("acFanSpeed", acFanSpeed);
    savedAcFanSpeed = acFanSpeed;
    Serial.println("[NVS] AC Fan Speed persisted.");
  }
}

void loadState() {
  if (prefs.getBytes("relays", relayState, sizeof(relayState)) != sizeof(relayState)) {
    memset(relayState, 0, sizeof(relayState)); // fresh/mismatched NVS -> all off
  }
  acPower = prefs.getBool("acPower", false);
  acTemp  = prefs.getInt("acTemp", DEFAULT_AC_TEMP);
  acFanSpeed = prefs.getInt("acFanSpeed", kMideaACFanAuto);
  
  // Seed shadow so the first persistNow() doesn't rewrite unchanged keys.
  memcpy(savedRelay, relayState, sizeof(relayState));
  savedAcPower = acPower;
  savedAcTemp = acTemp;
  savedAcFanSpeed = acFanSpeed;
  Serial.println("[NVS] State loaded from flash.");
}

// ---------- Outputs ----------
void applyRelay(int i) {
  digitalWrite(RELAY_PINS[i], relayState[i] ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
  Serial.printf("[Relay] Channel %d set to %s\n", i, relayState[i] ? "ON" : "OFF");
}

void initRmt() {
  rmt_tx_channel_config_t tx_chan_config = {};
  tx_chan_config.gpio_num = (gpio_num_t)IR_LED_PIN;
  tx_chan_config.clk_src = RMT_CLK_SRC_DEFAULT;
  tx_chan_config.resolution_hz = 1000000; // 1MHz, 1 tick = 1us
  tx_chan_config.mem_block_symbols = 64;
  tx_chan_config.trans_queue_depth = 4;
  tx_chan_config.flags.invert_out = false;
  tx_chan_config.flags.with_dma = false;

  esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &tx_channel);
  if (err != ESP_OK) {
    Serial.printf("[RMT] Failed to create TX channel: %s\n", esp_err_to_name(err));
    return;
  }

  rmt_carrier_config_t tx_carrier_cfg = {};
  tx_carrier_cfg.frequency_hz = 38000; // 38kHz
  tx_carrier_cfg.duty_cycle = 0.33;    // 33% duty cycle
  tx_carrier_cfg.flags.polarity_active_low = false;

  err = rmt_apply_carrier(tx_channel, &tx_carrier_cfg);
  if (err != ESP_OK) {
    Serial.printf("[RMT] Failed to apply carrier: %s\n", esp_err_to_name(err));
    return;
  }

  err = rmt_enable(tx_channel);
  if (err != ESP_OK) {
    Serial.printf("[RMT] Failed to enable TX channel: %s\n", esp_err_to_name(err));
    return;
  }

  rmt_copy_encoder_config_t copy_config = {};
  err = rmt_new_copy_encoder(&copy_config, &copy_encoder);
  if (err != ESP_OK) {
    Serial.printf("[RMT] Failed to create copy encoder: %s\n", esp_err_to_name(err));
    return;
  }

  Serial.println("[RMT] Hardware RMT IR transmitter initialized successfully on GPIO 18.");
}

void sendMideaRmt(uint64_t data) {
  if (tx_channel == NULL || copy_encoder == NULL) {
    Serial.println("[RMT] Error: RMT transmitter not initialized.");
    return;
  }

  // Turn onboard LED ON to indicate transmission starts
  digitalWrite(ONBOARD_LED_PIN, HIGH);

  rmt_symbol_word_t items[50];
  rmt_transmit_config_t tx_config = {};

  // Midea protocol repeats the message in an inverted format (Phase 2)
  for (int phase = 0; phase < 2; phase++) {
    int idx = 0;

    // Header
    rmt_symbol_word_t header = {};
    header.duration0 = 4480;
    header.level0 = 1;
    header.duration1 = 4480;
    header.level1 = 0;
    items[idx++] = header;

    // Data (48 bits -> 6 bytes, MSB first)
    for (int i = 8; i <= 48; i += 8) {
      uint8_t byteVal = (data >> (48 - i)) & 0xFF;
      for (int bit = 7; bit >= 0; bit--) {
        bool isOne = (byteVal >> bit) & 1;
        rmt_symbol_word_t bit_symbol = {};
        bit_symbol.duration0 = 560;
        bit_symbol.level0 = 1;
        bit_symbol.duration1 = isOne ? 1680 : 560;
        bit_symbol.level1 = 0;
        items[idx++] = bit_symbol;
      }
    }

    // Footer
    rmt_symbol_word_t footer = {};
    footer.duration0 = 560;
    footer.level0 = 1;
    footer.duration1 = 5600;
    footer.level1 = 0;
    items[idx++] = footer;

    // Transmit Phase
    esp_err_t err = rmt_transmit(tx_channel, copy_encoder, items, sizeof(items), &tx_config);
    if (err != ESP_OK) {
      Serial.printf("[RMT] Transmission failed: %s\n", esp_err_to_name(err));
      digitalWrite(ONBOARD_LED_PIN, LOW); // Turn off LED on error
      return;
    }

    // Wait for completion (blocking)
    rmt_tx_wait_all_done(tx_channel, portMAX_DELAY);

    // Invert the data for Phase 2 of the Midea protocol
    data = ~data;
  }

  // Turn onboard LED OFF when transmission completes
  digitalWrite(ONBOARD_LED_PIN, LOW);
}

void sendAc() {
  Serial.printf("[AC] Sending native Midea IR signal (Power=%s, Temp=%d, Fan=%d)\n", 
                acPower ? "ON" : "OFF", acTemp, acFanSpeed);
  acmidea.setUseCelsius(true);
  acmidea.setPower(acPower);
  acmidea.setTemp(acTemp, true);
  acmidea.setMode(acPower ? kMideaACCool : kMideaACAuto);
  acmidea.setFan(acFanSpeed);
  
  // Transmit using RMT hardware instead of software bit-banging
  sendMideaRmt(acmidea.getRaw());
}

// ---------- Matter Callback Handlers ----------
bool setLight1OnOff(bool state) {
  if (relayState[0] != state) {
    relayState[0] = state;
    applyRelay(0);
    markStateDirty();
  }
  return true;
}

bool setLight2OnOff(bool state) {
  if (relayState[1] != state) {
    relayState[1] = state;
    applyRelay(1);
    markStateDirty();
  }
  return true;
}

bool setLight3OnOff(bool state) {
  if (relayState[2] != state) {
    relayState[2] = state;
    applyRelay(2);
    markStateDirty();
  }
  return true;
}

bool setFanOnOff(MatterFan::FanMode_t mode, uint8_t percent) {
  bool state = (mode != MatterFan::FAN_MODE_OFF);
  if (relayState[3] != state) {
    relayState[3] = state;
    applyRelay(3);
    markStateDirty();
  }
  return true;
}

bool setAcMode(MatterThermostat::ThermostatMode_t mode) {
  bool state = (mode != MatterThermostat::THERMOSTAT_MODE_OFF);
  if (acPower != state) {
    acPower = state;
    acDirty = true;
    markStateDirty();
  }
  return true;
}

bool setAcTemperature(double temp) {
  int targetTemp = (int)round(temp);
  targetTemp = constrain(targetTemp, AC_TEMP_MIN, AC_TEMP_MAX);
  if (acTemp != targetTemp) {
    acTemp = targetTemp;
    acDirty = true;
    markStateDirty();
  }
  return true;
}

bool setOnboardLedOnOff(bool state) {
  if (onboardLedState != state) {
    onboardLedState = state;
    digitalWrite(ONBOARD_LED_PIN, state ? HIGH : LOW);
    Serial.printf("[LED] Onboard LED set to %s\n", state ? "ON" : "OFF");
  }
  return true;
}

bool setAcFan(MatterFan::FanMode_t mode, uint8_t percent) {
  int targetFanSpeed = kMideaACFanAuto;
  
  if (mode == MatterFan::FAN_MODE_LOW) {
    targetFanSpeed = kMideaACFanLow;
  } else if (mode == MatterFan::FAN_MODE_MEDIUM) {
    targetFanSpeed = kMideaACFanMed;
  } else if (mode == MatterFan::FAN_MODE_HIGH) {
    targetFanSpeed = kMideaACFanHigh;
  } else if (mode == MatterFan::FAN_MODE_AUTO) {
    targetFanSpeed = kMideaACFanAuto;
  } else if (mode == MatterFan::FAN_MODE_OFF) {
    targetFanSpeed = kMideaACFanAuto;
  } else {
    // Map speed percentage
    if (percent < 33) targetFanSpeed = kMideaACFanLow;
    else if (percent < 66) targetFanSpeed = kMideaACFanMed;
    else targetFanSpeed = kMideaACFanHigh;
  }
  
  if (acFanSpeed != targetFanSpeed) {
    acFanSpeed = targetFanSpeed;
    acDirty = true;
    markStateDirty();
    Serial.printf("[AC Fan] Speed mode set to: %d\n", acFanSpeed);
  }
  return true;
}

// Volatile flags for syncing switch toggles to the main loop context
volatile bool switchDirty[RELAY_COUNT] = {false, false, false, false};

// ---------- FreeRTOS Wall Switch Debouncer Task ----------
void switchDebounceTask(void* parameter) {
  const uint32_t LOCKOUT_MS = 1000; // ignore transitions for 1000ms after toggle
  const int WINDOW_SIZE = 15;       // 150ms history
  const uint32_t MASK = (1 << WINDOW_SIZE) - 1; // 0x7FFF (15 bits)
  
  uint32_t history[RELAY_COUNT];
  bool stableState[RELAY_COUNT];
  uint32_t lastToggleTime[RELAY_COUNT];
  
  // Setup inputs & read initial states
  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(SWITCH_PINS[i], INPUT_PULLUP);
    delay(5); // brief settling time
    bool r = digitalRead(SWITCH_PINS[i]);
    stableState[i] = r;
    // Initialize history: if pin is HIGH, history is all 1s. If LOW, all 0s.
    history[i] = r ? MASK : 0;
    lastToggleTime[i] = 0;
  }
  
  Serial.println("[Task] Robust asymmetric switch debouncer started.");
  
  while (true) {
    uint32_t now = millis();
    for (int i = 0; i < RELAY_COUNT; i++) {
      bool reading = digitalRead(SWITCH_PINS[i]);
      
      // Shift reading into history
      history[i] = ((history[i] << 1) | (reading ? 1 : 0)) & MASK;
      
      // If we are in the lockout period, we do not evaluate transitions
      if (now - lastToggleTime[i] < LOCKOUT_MS) {
        continue;
      }
      
      // Count HIGH samples in the window
      int highCount = __builtin_popcount(history[i]);
      
      bool triggered = false;
      if (stableState[i] == HIGH && highCount <= 1) {
        // Transition to LOW (switch closed to GND)
        stableState[i] = LOW;
        triggered = true;
      } 
      else if (stableState[i] == LOW && highCount >= 4) {
        // Transition to HIGH (switch opened, pulled up)
        stableState[i] = HIGH;
        triggered = true;
      }
      
      if (triggered) {
        lastToggleTime[i] = now;
        Serial.printf("[Switch] Toggle event on channel %d (stableState=%s, highCount=%d)\n", 
                      i, stableState[i] ? "HIGH" : "LOW", highCount);
        
        relayState[i] = !relayState[i];
        applyRelay(i);
        switchDirty[i] = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10)); // Sample every 10ms
  }
}

// ---------- Setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(1000); // Wait for serial console to attach
  Serial.println("\n[System] Booting Smart Home Matter Device...");

  // Initialize NVS and load state
  prefs.begin(NVS_NAMESPACE, false);
  loadState();

  // Initialize hardware relays to safe/saved state before output configuration
  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], relayState[i] ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
    applyRelay(i);
  }

  // Initialize onboard LED pin
  pinMode(ONBOARD_LED_PIN, OUTPUT);
  digitalWrite(ONBOARD_LED_PIN, onboardLedState ? HIGH : LOW);

  // Initialize IR transmitter
  acmidea.begin();
  initRmt();

  // Connect to Wi-Fi if credentials are provided in secrets.h
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
  Serial.printf("[WiFi] Connecting to SSID: %s ", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retry_count = 0;
  while (WiFi.status() != WL_CONNECTED && retry_count < 30) {
    delay(500);
    Serial.print(".");
    retry_count++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\n[WiFi] Connected successfully! IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] Connection failed! Will retry in the background.");
  }
#endif

  // Configure Matter endpoints and tie callback handlers
  matterLight1.onChange(setLight1OnOff);
  matterLight2.onChange(setLight2OnOff);
  matterLight3.onChange(setLight3OnOff);
  matterFan.onChange(setFanOnOff);
  matterAc.onChangeMode(setAcMode);
  matterAc.onChangeCoolingSetpoint(setAcTemperature);
  matterAcFan.onChange(setAcFan);
  matterOnboardLed.onChange(setOnboardLedOnOff);

  // Initialize endpoints with current state
  matterLight1.begin(relayState[0]);
  matterLight2.begin(relayState[1]);
  matterLight3.begin(relayState[2]);
  matterFan.begin(relayState[3] ? 100 : 0, relayState[3] ? MatterFan::FAN_MODE_ON : MatterFan::FAN_MODE_OFF, MatterFan::FAN_MODE_SEQ_OFF_HIGH);
  
  matterAc.begin(MatterThermostat::THERMOSTAT_SEQ_OP_COOLING_HEATING);
  matterAc.setMode(acPower ? MatterThermostat::THERMOSTAT_MODE_COOL : MatterThermostat::THERMOSTAT_MODE_OFF);
  matterAc.setCoolingHeatingSetpoints(0xffff, acTemp);
  matterAc.setLocalTemperature(acTemp);

  // Initialize AC fan speed endpoint
  MatterFan::FanMode_t initMode = MatterFan::FAN_MODE_AUTO;
  uint8_t initPercent = 0;
  if (acFanSpeed == kMideaACFanLow) {
    initMode = MatterFan::FAN_MODE_LOW;
    initPercent = 33;
  } else if (acFanSpeed == kMideaACFanMed) {
    initMode = MatterFan::FAN_MODE_MEDIUM;
    initPercent = 66;
  } else if (acFanSpeed == kMideaACFanHigh) {
    initMode = MatterFan::FAN_MODE_HIGH;
    initPercent = 100;
  }
  matterAcFan.begin(initPercent, initMode, MatterFan::FAN_MODE_SEQ_OFF_LOW_MED_HIGH_AUTO);

  matterOnboardLed.begin(onboardLedState);
  matterTempSensor.begin(25.5);

  // Start the Matter protocol stack
  Serial.println("[Matter] Starting stack...");
  Matter.begin();

  // Sync Matter endpoints with loaded NVS states after stack starts
  matterLight1.setOnOff(relayState[0]);
  matterLight2.setOnOff(relayState[1]);
  matterLight3.setOnOff(relayState[2]);
  matterFan.setOnOff(relayState[3]);

  // Sync AC Thermostat state
  matterAc.setMode(acPower ? MatterThermostat::THERMOSTAT_MODE_COOL : MatterThermostat::THERMOSTAT_MODE_OFF);
  matterAc.setCoolingHeatingSetpoints(0xffff, acTemp);

  // Sync AC Fan Speed
  MatterFan::FanMode_t acFanMode = MatterFan::FAN_MODE_AUTO;
  uint8_t acFanPercent = 0;
  if (acFanSpeed == kMideaACFanLow) {
    acFanMode = MatterFan::FAN_MODE_LOW;
    acFanPercent = 33;
  } else if (acFanSpeed == kMideaACFanMed) {
    acFanMode = MatterFan::FAN_MODE_MEDIUM;
    acFanPercent = 66;
  } else if (acFanSpeed == kMideaACFanHigh) {
    acFanMode = MatterFan::FAN_MODE_HIGH;
    acFanPercent = 100;
  }
  matterAcFan.setSpeedPercent(acFanPercent);
  matterAcFan.setMode(acFanMode);

  // Display commissioning pairing codes if the device is not yet onboarded
  if (!Matter.isDeviceCommissioned()) {
    Serial.println("\n==================================================");
    Serial.println("DEVICE NOT COMMISSIONED!");
    Serial.println("Scan the QR code or enter the manual code in Alexa:");
    Serial.printf("Manual Pairing Code: %s\n", Matter.getManualPairingCode().c_str());
    Serial.printf("QR Code URL:         %s\n", Matter.getOnboardingQRCodeUrl().c_str());
    Serial.println("==================================================\n");
  } else {
    Serial.println("[Matter] Node is already commissioned and connected to Wi-Fi.");
  }

  // Create FreeRTOS task for non-blocking switch debouncing
  xTaskCreate(
    switchDebounceTask,
    "SwitchDebounce",
    2048,
    NULL,
    1,
    NULL
  );

  // Trigger initial AC IR code send if it was loaded as ON
  if (acPower) {
    acDirty = true;
  }

  // Configure OTA
  ArduinoOTA.setHostname("smarthome");
#if defined(OTA_PASSWORD)
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }
    Serial.println("[OTA] Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] End");
    // Blink 3rd light (relay index 2) 3 times to indicate successful update completion
    for (int i = 0; i < 3; i++) {
      digitalWrite(RELAY_PINS[2], RELAY_ON_LEVEL);
      delay(200);
      digitalWrite(RELAY_PINS[2], RELAY_OFF_LEVEL);
      delay(200);
    }
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.println("[OTA] Over-The-Air updates initialized.");
}

uint32_t lastTempUpdateMs = 0;
#define TEMP_UPDATE_INTERVAL_MS 10000 // 10 seconds

void updateTempSensor() {
  uint32_t now = millis();
  if (now - lastTempUpdateMs >= TEMP_UPDATE_INTERVAL_MS) {
    lastTempUpdateMs = now;
    // Simulate temperature fluctuating between 24.0 and 27.0
    double simulatedTemp = 25.5 + 1.5 * sin(now / 50000.0);
    matterTempSensor.setTemperature(simulatedTemp);
    Serial.printf("[Temp Sensor] Simulated temperature updated to %.2f C\n", simulatedTemp);
  }
}

void loop() {
  ArduinoOTA.handle();

  // Check for serial decommission/reset command
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("decommission") || cmd.equalsIgnoreCase("reset")) {
      Serial.println("[System] Decommissioning device and erasing credentials...");
      Matter.decommission();
      delay(2000);
      ESP.restart();
    }
  }

  // Handle physical switch state updates in the main loop thread to prevent stack overflow
  for (int i = 0; i < RELAY_COUNT; i++) {
    if (switchDirty[i]) {
      switchDirty[i] = false;
      Serial.printf("[System] Syncing Matter endpoint for channel %d to %s\n", i, relayState[i] ? "ON" : "OFF");
      if (i == 0) matterLight1.setOnOff(relayState[0]);
      else if (i == 1) matterLight2.setOnOff(relayState[1]);
      else if (i == 2) matterLight3.setOnOff(relayState[2]);
      else if (i == 3) matterFan.setOnOff(relayState[3]);
      markStateDirty();
    }
  }

  // Execute IR send commands off the Matter task to prevent heap starvation
  if (acDirty) {
    acDirty = false;
    sendAc();
  }

  // Update simulated temperature sensor
  updateTempSensor();

  // Coalesce state changes and write to NVS
  if (stateDirty && (millis() - lastStateChangeMs) >= PERSIST_DELAY_MS) {
    stateDirty = false;
    persistNow();
  }

  delay(50); // Low-duty sleep cycle
}
