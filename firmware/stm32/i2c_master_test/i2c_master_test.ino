/*
 * I2C Master Test + Scanner - STM32U585 (Arduino UNO Q)
 * Primero escanea Wire (I2C2) y Wire2 (I2C3) en busca de dispositivos,
 * luego envía "HOLA\n" por ambos buses al Mega (Slave 0x08)
 */
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  delay(500);
  
  Wire.begin();   // I2C2 = PB10(SCL) PB11(SDA)
  Wire2.begin();  // I2C3 = PC0(SCL=A5) PC1(SDA=A4)
  
  Serial.println("\n=== I2C MASTER TEST ===");
  Serial.println("Wire (PB10/PB11) y Wire2 (A4/A5) iniciados");
}

void scanBus(const char* name, TwoWire &bus) {
  Serial.print("Escaneando ");
  Serial.print(name);
  Serial.println(":");
  
  for (byte addr = 1; addr < 127; addr++) {
    bus.beginTransmission(addr);
    byte error = bus.endTransmission();
    if (error == 0) {
      Serial.print("  Encontrado 0x");
      Serial.println(addr, HEX);
    }
  }
}

void sendTest(const char* name, TwoWire &bus, const char* msg) {
  Serial.print("Enviando por ");
  Serial.print(name);
  Serial.print(": ");
  Serial.print(msg);
  
  bus.beginTransmission(8);
  bus.write(msg);
  byte error = bus.endTransmission();
  
  if (error == 0) {
    Serial.println("-> OK");
  } else {
    Serial.print("-> ERROR ");
    Serial.println(error);
  }
}

void loop() {
  scanBus("Wire", Wire);
  scanBus("Wire2", Wire2);
  
  Serial.println("--- Envios a 0x08 ---");
  sendTest("Wire", Wire, "HOLA\n");
  sendTest("Wire2", Wire2, "HOLA\n");
  
  Serial.println("=====================\n");
  delay(3000);
}
