#include <Arduino.h>

#define LASER_PIN 4
#define BAUD_RATE 250000

const char* messages[] = {
  "HELLO WORLD",
  "TERAHERTZ UART TEST",
  "OPTICAL LINK ESTABLISHED",
  "MARYLAND ENGINEERING",
  "NO MORE CLOCK DRIFT"
};
const int NUM_MESSAGES = 5;

// ============================================================
// CALCULATE XOR CHECKSUM
// ============================================================
uint8_t calculateChecksum(String payload) {
  uint8_t checksum = 0;
  for (int i = 0; i < payload.length(); i++) {
    checksum ^= payload[i]; // XOR each character
  }
  return checksum;
}

void setup() {
  // USB Debugging
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n================================");
  Serial.println("  TERAHERTZ PACKET SENDER");
  Serial.println("================================\n");

  // Laser Hardware UART (Inverted = true)
  Serial1.begin(BAUD_RATE, SERIAL_8N1, -1, LASER_PIN, true);
  
  randomSeed(micros());
}

void loop() {
  // 1. Pick a random message
  int index = random(0, NUM_MESSAGES);
  String payload = messages[index];

  // 2. Calculate the checksum
  uint8_t checksum = calculateChecksum(payload);

  // 3. Convert checksum to a 2-character HEX string
  char hexBuffer[3];
  sprintf(hexBuffer, "%02X", checksum);
  String checksumHex = String(hexBuffer);

  // 4. Construct the packet: <PAYLOAD|CHECKSUM>
  String packet = "<" + payload + "|" + checksumHex + ">";

  // 5. Print to USB for debugging
  Serial.print("Transmitting: ");
  Serial.println(packet);

  // 6. Fire the laser
  Serial1.print(packet);

  delay(2000); // Wait 2 seconds before next burst
}