#pragma once
#include <Arduino.h>

// ---------- Relay outputs (active-LOW module) ----------
// Channels: 0 = Light 1, 1 = Light 2, 2 = Light 3, 3 = Fan
#define RELAY_COUNT 4
static const uint8_t RELAY_PINS[RELAY_COUNT]  = {23, 22, 21, 19};
#define RELAY_ON_LEVEL  LOW
#define RELAY_OFF_LEVEL HIGH

// ---------- Manual wall switches (rocker -> GND, INPUT_PULLUP) ----------
// One per relay channel, same index order as RELAY_PINS.
static const uint8_t SWITCH_PINS[RELAY_COUNT] = {13, 14, 27, 26};
#define DEBOUNCE_MS 30

// ---------- IR ----------
#define IR_LED_PIN  4    // -> 330 ohm -> 2N2222 base; LED in collector path
#define IR_RECV_PIN 15   // only used by tools/ir_capture during code capture
#define IR_CARRIER_KHZ 38

// ---------- AC ----------
#define AC_TEMP_MIN     16
#define AC_TEMP_MAX     30
#define DEFAULT_AC_TEMP 24
enum AcMode { AC_MODE_COOL = 0, AC_MODE_FAN = 1, AC_MODE_DRY = 2, AC_MODE_AUTO = 3 };

// ---------- Network ----------
#define HOSTNAME "smarthome"        // reachable at http://smarthome.local
#define AP_NAME  "SmartHome-Setup"  // captive-portal SSID on first boot

// ---------- Persistence ----------
#define NVS_NAMESPACE "smarthome"
