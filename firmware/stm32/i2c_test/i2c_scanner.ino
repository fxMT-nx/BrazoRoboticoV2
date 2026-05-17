/*
 * I2C Scanner - STM32U585
 * Busca dispositivos en Wire2 (A4/A5)
 * Salida por Serial (USB CDC = ttyACM0)
 */
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  Wire2.begin();
  delay(500);
  Serial.println("I2C Scanner iniciado en Wire2");
}

void loop() {
  Serial.println("Escaneando...");
  
  for (byte addr = 1; addr < 127; addr++) {
    Wire2.beginTransmission(addr);
    byte error = Wire2.endTransmission();
    
    if (error == 0) {
      Serial.print("Dispositivo en 0x");
      Serial.println(addr, HEX);
    }
  }
  
  Serial.println("---");
  delay(5000);
}
