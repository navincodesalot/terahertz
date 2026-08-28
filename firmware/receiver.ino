#include <Arduino.h>

#define RECEIVER_PIN 5
#define BAUD_RATE 250000

// ============================================================
// CALCULATE XOR CHECKSUM
// ============================================================
uint8_t calculateChecksum(String payload) {
  uint8_t checksum = 0;
  for (int i = 0; i < payload.length(); i++) {
    checksum ^= payload[i];
  }
  return checksum;
}

void setup() {
  // USB Debugging
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n================================");
  Serial.println("  TERAHERTZ PACKET RECEIVER");
  Serial.println("================================\n");
  Serial.println("Waiting for optical packets...\n");

  // Photodiode Hardware UART (Inverted = true)
  Serial1.begin(BAUD_RATE, SERIAL_8N1, RECEIVER_PIN, -1, true);
}

void loop() {
  // If data is coming in from the laser
  if (Serial1.available()) {
    
    // Read the stream until we hit the '>' end marker
    String incoming = Serial1.readStringUntil('>');
    
    // Check if it has a valid start marker '<'
    if (incoming.startsWith("<")) {
      
      // Find the separator
      int pipeIndex = incoming.lastIndexOf('|');
      
      if (pipeIndex > 1) {
        // Extract the pieces
        String payload = incoming.substring(1, pipeIndex);
        String receivedHex = incoming.substring(pipeIndex + 1);
        
        // Convert received HEX string back to a number
        uint8_t receivedChecksum = (uint8_t) strtol(receivedHex.c_str(), NULL, 16);
        
        // Calculate what the checksum SHOULD be
        uint8_t expectedChecksum = calculateChecksum(payload);
        
        Serial.println("--------------------------------");
        
        // Compare them to see if the packet survived the air
        if (expectedChecksum == receivedChecksum) {
          Serial.println("[SUCCESS] VALID PACKET");
          Serial.print("DATA: ");
          Serial.println(payload);
        } else {
          Serial.println("[ERROR] CORRUPTED PACKET (DROPPED)");
          Serial.print("Expected Checksum: ");
          Serial.print(expectedChecksum, HEX);
          Serial.print(" | Received: ");
          Serial.println(receivedChecksum, HEX);
          Serial.print("Garbage seen: ");
          Serial.println(payload);
        }
      }
    }
  }
}