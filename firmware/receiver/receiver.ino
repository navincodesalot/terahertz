#include <Arduino.h>

#define RECEIVER_PIN 5
#define BAUD_RATE 250000

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n================================");
  Serial.println("  TERAHERTZ UART RECEIVER");
  Serial.println("================================\n");
  Serial.println("Waiting for optical data...\n");

  // Match the validated sender configuration: inverted 250000 baud UART.
  Serial1.begin(BAUD_RATE, SERIAL_8N1, RECEIVER_PIN, -1, true);
}

void loop() {
  if (!Serial1.available()) return;

  String receivedMessage = Serial1.readStringUntil('\n');
  receivedMessage.trim();

  if (receivedMessage.length() > 0) {
    Serial.println("--------------------------------");
    Serial.print("RECEIVED: ");
    Serial.println(receivedMessage);
  }
}
