#include <Arduino.h>

void setup() {
 	Serial.begin(115200);
  while(!Serial);
  Serial.println("Setup...");

      
  pinMode(LED_BUILTIN, OUTPUT);
	digitalWrite(LED_BUILTIN, HIGH);
	delay(500);
	digitalWrite(LED_BUILTIN, LOW);
	delay(500);
	digitalWrite(LED_BUILTIN, HIGH);
	delay(500);
	digitalWrite(LED_BUILTIN, LOW);
	delay(500);

}

void loop() {
  Serial.println("loop...");
  delay(500);
  // put your main code here, to run repeatedly:
}

