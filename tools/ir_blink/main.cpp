#include <Arduino.h>

#define IR_LED_PIN 4
#define ONBOARD_LED_PIN 2

void setup() {
  Serial.begin(115200);
  pinMode(IR_LED_PIN, OUTPUT);
  pinMode(ONBOARD_LED_PIN, OUTPUT);
  Serial.println("[IR Test] Dual blink test started (GPIO 2 & GPIO 4).");
}

void loop() {
  digitalWrite(IR_LED_PIN, HIGH);
  digitalWrite(ONBOARD_LED_PIN, HIGH);
  Serial.println("[IR Test] GPIO 2 & 4 set to HIGH (LEDs should turn ON)");
  delay(1000);
  
  digitalWrite(IR_LED_PIN, LOW);
  digitalWrite(ONBOARD_LED_PIN, LOW);
  Serial.println("[IR Test] GPIO 2 & 4 set to LOW (LEDs should turn OFF)");
  delay(1000);
}
