// =============================================================================
// ESP32 Smart-Home Attachment
//   - 3 lights + 1 fan via a 4-channel relay (on/off)
//   - Croma AC via IR (on/off, temperature, mode)
//   - Alexa voice on/off via local Philips-Hue emulation (Espalexa, no cloud)
//   - Existing wall rockers kept live: any flip inverts the current state
//   - Self-hosted web dashboard for precise AC temperature/mode
// Everything runs on the ESP32 itself; no separate server or subscription.
// =============================================================================

#ifndef ESPALEXA_ASYNC
#define ESPALEXA_ASYNC
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <Espalexa.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <IRsend.h>

#include "config.h"
#include "dashboard.h"
#include "ac_codes.h"

// ---------- Globals ----------
AsyncWebServer server(80);
Espalexa       espalexa;
Preferences    prefs;
IRsend         irsend(IR_LED_PIN);

bool relayState[RELAY_COUNT] = {false, false, false, false};
bool acPower = false;
int  acTemp  = DEFAULT_AC_TEMP;
int  acMode  = AC_MODE_COOL;

// Async-safe IR: handlers/Alexa callbacks may run on the AsyncTCP task, where a
// blocking sendRaw() would starve the stack. They request a send; loop() does it.
volatile bool acDirty = false;

// Deferred NVS persistence (coalesced in loop() to limit flash wear).
bool     stateDirty = false;
uint32_t lastStateChangeMs = 0;
bool     savedRelay[RELAY_COUNT];   // shadow of last-persisted values
bool     savedAcPower;
int      savedAcTemp;
int      savedAcMode;

struct SwitchInput {
  bool     lastReading;
  bool     lastStable;
  uint32_t lastChangeMs;
};
SwitchInput switches[RELAY_COUNT];

EspalexaDevice* devLight[3] = {nullptr, nullptr, nullptr};
EspalexaDevice* devFan      = nullptr;
EspalexaDevice* devAc       = nullptr;

// ---------- Persistence ----------
// Mark state changed; the real NVS write is coalesced in loop() (flash wear).
void markStateDirty() {
  stateDirty = true;
  lastStateChangeMs = millis();
}

// Write only the keys whose value actually changed.
void persistNow() {
  if (memcmp(savedRelay, relayState, sizeof(relayState)) != 0) {
    prefs.putBytes("relays", relayState, sizeof(relayState));
    memcpy(savedRelay, relayState, sizeof(relayState));
  }
  if (savedAcPower != acPower) { prefs.putBool("acPower", acPower); savedAcPower = acPower; }
  if (savedAcTemp  != acTemp)  { prefs.putInt("acTemp", acTemp);    savedAcTemp  = acTemp; }
  if (savedAcMode  != acMode)  { prefs.putInt("acMode", acMode);    savedAcMode  = acMode; }
}

void loadState() {
  if (prefs.getBytes("relays", relayState, sizeof(relayState)) != sizeof(relayState)) {
    memset(relayState, 0, sizeof(relayState));   // fresh/mismatched NVS -> all off
  }
  acPower = prefs.getBool("acPower", false);
  acTemp  = prefs.getInt("acTemp", DEFAULT_AC_TEMP);
  acMode  = prefs.getInt("acMode", AC_MODE_COOL);
  // Seed shadow so the first persistNow() doesn't rewrite unchanged keys.
  memcpy(savedRelay, relayState, sizeof(relayState));
  savedAcPower = acPower; savedAcTemp = acTemp; savedAcMode = acMode;
}

// ---------- Outputs ----------
void applyRelay(int i) {
  digitalWrite(RELAY_PINS[i], relayState[i] ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
}

// Keep the Espalexa (Alexa app) view in sync after local/manual changes.
void syncAlexaRelay(int i) {
  if (i < 3) {
    if (devLight[i]) devLight[i]->setValue(relayState[i] ? 255 : 0);
  } else if (devFan) {
    devFan->setValue(relayState[i] ? 255 : 0);
  }
}

// Reflect AC state into the Alexa (Hue) device. Brightness encodes temperature,
// so dashboard/temperature changes round-trip to the Alexa app's slider.
void syncAlexaAc() {
  if (devAc) devAc->setValue(acPower ? map(acTemp, AC_TEMP_MIN, AC_TEMP_MAX, 1, 254) : 0);
}

void sendAc() {
  sendAcCommand(irsend, acPower, acTemp, acMode);
}

// ---------- Manual wall switches: edge-toggle ----------
// Rockers are maintained SPST. We only care about *changes* in position: each
// flip toggles the relay, so the switch inverts whatever the current state is
// (whether Alexa, the dashboard, or a previous flip set it).
void handleSwitches() {
  uint32_t now = millis();
  for (int i = 0; i < RELAY_COUNT; i++) {
    bool reading = digitalRead(SWITCH_PINS[i]);
    if (reading != switches[i].lastReading) {
      switches[i].lastReading = reading;
      switches[i].lastChangeMs = now;
    }
    if ((now - switches[i].lastChangeMs) >= DEBOUNCE_MS &&
        reading != switches[i].lastStable) {
      switches[i].lastStable = reading;       // accept the new stable position
      relayState[i] = !relayState[i];          // any flip toggles
      applyRelay(i);
      syncAlexaRelay(i);
      markStateDirty();
    }
  }
}

// ---------- Alexa (Espalexa) ----------
void deviceChanged(EspalexaDevice* d) {
  if (!d) return;
  for (int i = 0; i < 3; i++) {
    if (d == devLight[i]) {
      relayState[i] = d->getValue() > 0;
      applyRelay(i);
      markStateDirty();
      return;
    }
  }
  if (d == devFan) {
    relayState[3] = d->getValue() > 0;
    applyRelay(3);
    markStateDirty();
    return;
  }
  if (d == devAc) {
    acPower = d->getValue() > 0;
    // Full brightness ("turn on AC") keeps the current temperature; an explicit
    // dim level ("set AC to 50%") maps onto the 16..30 C range.
    if (acPower && d->getValue() < 255) {
      acTemp = map(d->getValue(), 1, 254, AC_TEMP_MIN, AC_TEMP_MAX);
    }
    acDirty = true;        // transmit from loop(), not this (possibly async) context
    markStateDirty();
    return;
  }
}

void setupEspalexa() {
  devLight[0] = new EspalexaDevice("Light 1", deviceChanged);
  devLight[1] = new EspalexaDevice("Light 2", deviceChanged);
  devLight[2] = new EspalexaDevice("Light 3", deviceChanged);
  devFan      = new EspalexaDevice("Fan", deviceChanged);
  devAc       = new EspalexaDevice("AC", deviceChanged);
  for (int i = 0; i < 3; i++) {
    devLight[i]->setValue(relayState[i] ? 255 : 0);
    espalexa.addDevice(devLight[i]);
  }
  devFan->setValue(relayState[3] ? 255 : 0);
  devAc->setValue(acPower ? 255 : 0);
  espalexa.addDevice(devFan);
  espalexa.addDevice(devAc);
}

// ---------- Web API ----------
void sendStateJson(AsyncWebServerRequest* req) {
  StaticJsonDocument<256> doc;
  JsonArray arr = doc.createNestedArray("relays");
  for (int i = 0; i < RELAY_COUNT; i++) arr.add(relayState[i]);
  doc["acPower"] = acPower;
  doc["acTemp"]  = acTemp;
  doc["acMode"]  = acMode;
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

void setupRoutes() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", DASHBOARD_HTML);
  });

  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
    sendStateJson(req);
  });

  server.on("/api/relay", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("ch") || !req->hasParam("state")) {
      req->send(400, "application/json", "{\"error\":\"ch and state required\"}");
      return;
    }
    int ch = req->getParam("ch")->value().toInt();
    int st = req->getParam("state")->value().toInt();
    if (ch < 0 || ch >= RELAY_COUNT) {
      req->send(400, "application/json", "{\"error\":\"invalid ch\"}");
      return;
    }
    relayState[ch] = (st != 0);
    applyRelay(ch);
    syncAlexaRelay(ch);
    markStateDirty();
    sendStateJson(req);
  });

  server.on("/api/ac", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (req->hasParam("power"))
      acPower = req->getParam("power")->value().toInt() != 0;
    if (req->hasParam("temp"))
      acTemp = constrain(req->getParam("temp")->value().toInt(), AC_TEMP_MIN, AC_TEMP_MAX);
    if (req->hasParam("mode"))
      acMode = constrain(req->getParam("mode")->value().toInt(), AC_MODE_COOL, AC_MODE_AUTO);
    acDirty = true;        // transmit from loop(), not this async handler
    syncAlexaAc();
    markStateDirty();
    sendStateJson(req);
  });
}

// ---------- Setup / loop ----------
void setup() {
  Serial.begin(115200);

  prefs.begin(NVS_NAMESPACE, false);
  loadState();

  for (int i = 0; i < RELAY_COUNT; i++) {
    // Drive the OFF level into the latch BEFORE enabling the output, so an
    // active-LOW board doesn't briefly energize the relay as the pin switches
    // to OUTPUT (which would otherwise default LOW = ON).
    digitalWrite(RELAY_PINS[i], relayState[i] ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
    pinMode(RELAY_PINS[i], OUTPUT);
    applyRelay(i);                       // restore saved state
    pinMode(SWITCH_PINS[i], INPUT_PULLUP);
  }
  // Seed switch positions WITHOUT toggling (only later changes toggle).
  for (int i = 0; i < RELAY_COUNT; i++) {
    bool r = digitalRead(SWITCH_PINS[i]);
    switches[i] = { r, r, 0 };
  }

  irsend.begin();

  WiFi.setAutoReconnect(true);           // recover automatically if the router blips
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  // Password-protected setup AP so the captive portal isn't an open access point.
  if (!wm.autoConnect(AP_NAME, AP_PASSWORD)) {
    Serial.println(F("WiFi config timed out, restarting."));
    ESP.restart();
  }

  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);  // require a password to push firmware
  ArduinoOTA.begin();

  setupRoutes();
  setupEspalexa();
  espalexa.begin(&server);   // attaches to our AsyncWebServer and starts it

  if (acPower) acDirty = true;  // re-assert restored AC state on the unit (absolute frames)

  Serial.printf("Ready at http://%s.local/\n", HOSTNAME);
}

void loop() {
  handleSwitches();
  espalexa.loop();
  ArduinoOTA.handle();

  // Transmit AC IR here (off the async task) when requested.
  if (acDirty) {
    acDirty = false;
    sendAc();
  }

  // Flush persisted state once changes settle, coalescing bursts.
  if (stateDirty && (millis() - lastStateChangeMs) >= PERSIST_DELAY_MS) {
    stateDirty = false;
    persistNow();
  }
}
