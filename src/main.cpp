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
void saveState() {
  prefs.putBytes("relays", relayState, sizeof(relayState));
  prefs.putBool("acPower", acPower);
  prefs.putInt("acTemp", acTemp);
  prefs.putInt("acMode", acMode);
}

void loadState() {
  prefs.getBytes("relays", relayState, sizeof(relayState));
  acPower = prefs.getBool("acPower", false);
  acTemp  = prefs.getInt("acTemp", DEFAULT_AC_TEMP);
  acMode  = prefs.getInt("acMode", AC_MODE_COOL);
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
      saveState();
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
      saveState();
      return;
    }
  }
  if (d == devFan) {
    relayState[3] = d->getValue() > 0;
    applyRelay(3);
    saveState();
    return;
  }
  if (d == devAc) {
    acPower = d->getValue() > 0;
    // Full brightness ("turn on AC") keeps the current temperature; an explicit
    // dim level ("set AC to 50%") maps onto the 16..30 C range.
    if (acPower && d->getValue() < 255) {
      acTemp = map(d->getValue(), 1, 254, AC_TEMP_MIN, AC_TEMP_MAX);
    }
    sendAc();
    saveState();
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
    saveState();
    sendStateJson(req);
  });

  server.on("/api/ac", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (req->hasParam("power"))
      acPower = req->getParam("power")->value().toInt() != 0;
    if (req->hasParam("temp"))
      acTemp = constrain(req->getParam("temp")->value().toInt(), AC_TEMP_MIN, AC_TEMP_MAX);
    if (req->hasParam("mode"))
      acMode = constrain(req->getParam("mode")->value().toInt(), AC_MODE_COOL, AC_MODE_AUTO);
    sendAc();
    if (devAc) devAc->setValue(acPower ? 255 : 0);
    saveState();
    sendStateJson(req);
  });
}

// ---------- Setup / loop ----------
void setup() {
  Serial.begin(115200);

  prefs.begin(NVS_NAMESPACE, false);
  loadState();

  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    applyRelay(i);                       // restore saved state before anything else
    pinMode(SWITCH_PINS[i], INPUT_PULLUP);
  }
  // Seed switch positions WITHOUT toggling (only later changes toggle).
  for (int i = 0; i < RELAY_COUNT; i++) {
    bool r = digitalRead(SWITCH_PINS[i]);
    switches[i] = { r, r, 0 };
  }

  irsend.begin();

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect(AP_NAME)) {
    Serial.println(F("WiFi config timed out, restarting."));
    ESP.restart();
  }

  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.begin();

  setupRoutes();
  setupEspalexa();
  espalexa.begin(&server);   // attaches to our AsyncWebServer and starts it

  Serial.printf("Ready at http://%s.local/\n", HOSTNAME);
}

void loop() {
  handleSwitches();
  espalexa.loop();
  ArduinoOTA.handle();
}
