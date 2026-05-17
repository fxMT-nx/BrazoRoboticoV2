/*
 * I2C SCANNER: busca dispositivos en el bus I2C
 * Escanea direcciones 1-127
 */
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  Wire.begin();  // Master mode
  Serial.println("\n=== I2C SCANNER MEGA ===");
  Serial.println("Escaneando bus I2C...");
  
  int count = 0;
  for (byte addr = 1; addr < 128; addr++) {
    Wire.beginTransmission(addr);
    byte error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("Dispositivo encontrado en 0x");
      if (addr < 16) Serial.print("0");
      Serial.print(addr, HEX);
      Serial.print(" (");
      Serial.print(addr, DEC);
      Serial.println(")");
      count++;
    }
    delay(5);
  }
  
  if (count == 0) {
    Serial.println("No se encontraron dispositivos I2C");
  } else {
    Serial.print("Total: ");
    Serial.print(count);
    Serial.println(" dispositivo(s)");
  }
  Serial.println("=== SCAN COMPLETADO ===");
}

void loop() {
  // Escanear cada 5 segundos
  delay(5000);
  
  Serial.println("\n--- Re-escan ---");
  for (byte addr = 1; addr < 128; addr++) {
    Wire.beginTransmission(addr);
    byte error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("0x");
      if (addr < 16) Serial.print("0");
      Serial.println(addr, HEX);
    }
    delay(3);
  }
  Serial.println("--- Fin scan ---");
}
