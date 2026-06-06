#include <Wire.h>

#define OLED_SDA A4
#define OLED_SCL A5

unsigned long lastScan = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(OLED_SDA, OLED_SCL);

  Serial.println();
  Serial.println("Continuous I2C Scanner");
}

void loop() {

  // Scan every 3 seconds
  if (millis() - lastScan >= 3000) {

    lastScan = millis();

    Serial.println();
    Serial.println("Scanning I2C bus...");

    bool foundDevice = false;

    for (byte address = 1; address < 127; address++) {

      Wire.beginTransmission(address);
      byte error = Wire.endTransmission();

      if (error == 0) {

        foundDevice = true;

        Serial.print("I2C device found at 0x");

        if (address < 16)
          Serial.print("0");

        Serial.println(address, HEX);

      } else if (error == 4) {

        Serial.print("Unknown error at 0x");

        if (address < 16)
          Serial.print("0");

        Serial.println(address, HEX);
      }
    }

    if (!foundDevice) {
      Serial.println("No I2C devices found");
    }

    Serial.println("Scan complete");
  }
}