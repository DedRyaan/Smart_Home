// =============================================================================
// One-time Croma AC IR capture sketch
// -----------------------------------------------------------------------------
// Flash this ALONE (it's a standalone Arduino sketch) with a VS1838B/TSOP1838 IR
// receiver wired to IR_RECV_PIN. Point your Croma remote at the receiver and
// press one button at a time. For each press it prints:
//   - the decoded protocol (if recognised), and
//   - a raw timing array you can paste into include/ac_codes.h.
//
// Capture: Power OFF, and Power ON at each temperature 16..30 C in COOL mode.
// If the protocol decodes to a known AC class (Coolix/Voltas/Gree/Toshiba/...),
// prefer that class over raw frames; otherwise use the raw arrays.
//
// Wiring: VS1838B OUT -> GPIO15, VCC -> 3V3, GND -> GND.
// =============================================================================

#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>

const uint16_t kRecvPin     = 15;
const uint16_t kCaptureBufferSize = 1024;  // big enough for long AC frames
const uint8_t  kTimeout     = 50;          // ms of no signal = end of frame
const uint16_t kMinUnknownSize = 12;

IRrecv irrecv(kRecvPin, kCaptureBufferSize, kTimeout, true);
decode_results results;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(50);
  irrecv.setUnknownThreshold(kMinUnknownSize);
  irrecv.enableIRIn();
  Serial.println(F("IR capture ready. Point the Croma remote and press a button."));
}

void loop() {
  if (irrecv.decode(&results)) {
    Serial.println(F("\n--------------------------------------------------"));
    Serial.print(F("Protocol : ")); Serial.println(typeToString(results.decode_type, results.repeat));
    Serial.print(F("Bits     : ")); Serial.println(results.bits);
    // Paste this array (drop the leading length count) into ac_codes.h:
    Serial.println(F("Raw array (for sendRaw):"));
    Serial.println(resultToSourceCode(&results));
    yield();
  }
}
