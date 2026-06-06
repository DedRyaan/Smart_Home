#pragma once
#include <Arduino.h>
#include <IRsend.h>
#include "config.h"

// =============================================================================
// Croma AC IR codes
// -----------------------------------------------------------------------------
// Croma has no dedicated class in IRremoteESP8266, and AC IR is stateful, so we
// CAPTURE raw frames from your physical remote (see tools/ir_capture) and replay
// them here with IRsend::sendRaw().
//
// HOW TO FILL THIS IN after capturing:
//   1. For Power OFF, paste the captured timing array into AC_OFF_RAW.
//   2. For each temperature 16..30 C in COOL mode, capture the frame and paste it
//      into the matching row of AC_COOL_FRAMES below.
//   3. (Optional) add FAN/DRY/AUTO tables the same way and extend sendAcCommand().
//
// Until a frame is filled, its pointer is nullptr and the send is safely skipped
// (a warning is logged), so the firmware compiles and runs from day one.
// =============================================================================

#define AC_TEMP_COUNT (AC_TEMP_MAX - AC_TEMP_MIN + 1)

struct AcFrame {
  const uint16_t* data;  // raw mark/space timings in microseconds
  uint16_t        len;   // number of entries in `data`
};

// ----- Power OFF -----
// const uint16_t AC_OFF_DATA[] = { 9000, 4500, /* ... */ };
static const AcFrame AC_OFF = { nullptr, 0 };  // TODO: capture

// ----- COOL mode, indexed by (temp - AC_TEMP_MIN), i.e. row 0 == 16 C -----
// Example once captured:
//   static const uint16_t COOL_24[] = { 9000, 4500, 560, 560, /* ... */ };
//   ... { COOL_24, sizeof(COOL_24)/sizeof(COOL_24[0]) } ...
static const AcFrame AC_COOL_FRAMES[AC_TEMP_COUNT] = {
  /* 16 */ { nullptr, 0 },
  /* 17 */ { nullptr, 0 },
  /* 18 */ { nullptr, 0 },
  /* 19 */ { nullptr, 0 },
  /* 20 */ { nullptr, 0 },
  /* 21 */ { nullptr, 0 },
  /* 22 */ { nullptr, 0 },
  /* 23 */ { nullptr, 0 },
  /* 24 */ { nullptr, 0 },
  /* 25 */ { nullptr, 0 },
  /* 26 */ { nullptr, 0 },
  /* 27 */ { nullptr, 0 },
  /* 28 */ { nullptr, 0 },
  /* 29 */ { nullptr, 0 },
  /* 30 */ { nullptr, 0 },
};

// Sends the appropriate frame for the requested AC state.
// Returns true if a real frame was transmitted, false if none is captured yet.
inline bool sendAcCommand(IRsend& ir, bool power, int temp, int mode) {
  if (!power) {
    if (AC_OFF.data && AC_OFF.len) {
      ir.sendRaw(AC_OFF.data, AC_OFF.len, IR_CARRIER_KHZ);
      return true;
    }
    Serial.println(F("[AC] OFF frame not captured yet (tools/ir_capture)."));
    return false;
  }

  // Only COOL is wired up by default; extend here for FAN/DRY/AUTO.
  int idx = constrain(temp, AC_TEMP_MIN, AC_TEMP_MAX) - AC_TEMP_MIN;
  const AcFrame& f = AC_COOL_FRAMES[idx];
  if (f.data && f.len) {
    ir.sendRaw(f.data, f.len, IR_CARRIER_KHZ);
    return true;
  }
  Serial.printf("[AC] COOL %d C frame not captured yet (tools/ir_capture).\n", temp);
  return false;
}
