#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Midea.h>

#define IR_LED_PIN 18
#define ONBOARD_LED_PIN 2

IRMideaAC ac(IR_LED_PIN);

void setup() {
  Serial.begin(115200);
  pinMode(ONBOARD_LED_PIN, OUTPUT);
  
  // Initialize the Midea AC transmitter
  ac.begin();
  
  Serial.println("[Midea Test] Initialized Midea AC control on GPIO 18.");
}

void loop() {
  // Turn onboard LED ON to indicate transmission starts
  
  Serial.println("[Midea Test] Configuring command...");
  
  // Configure Midea AC Command
  ac.setUseCelsius(true);
  ac.setPower(true);
  ac.setTemp(21, true); // 21 C
  ac.setMode(kMideaACCool);
  ac.setFan(kMideaACFanHigh);
  
  // Transmit command
  ac.send();
  
  // Print the raw hex representation of the 48-bit command and human readable string
  Serial.printf("[Midea Test] Transmitted state (Hex): 0x%012llX\n", ac.getRaw());
  Serial.printf("[Midea Test] Transmitted state (Text): %s\n", ac.toString().c_str());
  
  digitalWrite(ONBOARD_LED_PIN, HIGH);
  // Keep LED ON for a short time so the flash is visible
  delay(100);
  
  // Turn onboard LED OFF
  digitalWrite(ONBOARD_LED_PIN, LOW);
  
  // Wait 7 seconds before transmitting again
  delay(5000);
}
