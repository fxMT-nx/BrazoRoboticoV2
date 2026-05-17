#include <Wire.h>
#define MY_ADDRESS 0x08
#define LOG_PREFIX "[MEGA-SLAVE]"

float latRecibida = 0;
float lonRecibida = 0;
bool  datoNuevo   = false;

void setup() {
  Serial.begin(115200);
  Wire.begin(MY_ADDRESS);
  Wire.onReceive(recibirDatos);
  Serial.println(LOG_PREFIX " Iniciado como SLAVE I2C en direccion 0x08");
}

void loop() {
  if (datoNuevo) {
    Serial.print(LOG_PREFIX " Coordenadas -> lat: ");
    Serial.print(latRecibida, 6);
    Serial.print(" lon: ");
    Serial.println(lonRecibida, 6);
    datoNuevo = false;
  }
}

void recibirDatos(int numBytes) {
  if (numBytes == 8) {
    byte bufLat[4], bufLon[4];
    for (int i = 0; i < 4; i++) bufLat[i] = Wire.read();
    for (int i = 0; i < 4; i++) bufLon[i] = Wire.read();
    memcpy(&latRecibida, bufLat, 4);
    memcpy(&lonRecibida, bufLon, 4);
    datoNuevo = true;
  } else {
    while (Wire.available()) Wire.read();
  }
}
