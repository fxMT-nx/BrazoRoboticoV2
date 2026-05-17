#include <Wire.h>
#define MEGA_ADDRESS 0x08
#define LOG_PREFIX "[UNO-MASTER]"

float latitud  = 37.3891;
float longitud = -5.9845;

void setup() {
  Serial2.begin(115200);
  Wire2.begin();
  Serial2.println(LOG_PREFIX " Iniciado como MASTER I2C (Wire2 en A4/A5)");
  delay(1000);
}

void loop() {
  latitud  += 0.0001;
  longitud += 0.0001;

  Serial2.print(LOG_PREFIX " Enviando -> lat: ");
  Serial2.print(latitud, 6);
  Serial2.print(" lon: ");
  Serial2.println(longitud, 6);

  Wire2.beginTransmission(MEGA_ADDRESS);
  byte* bLat = (byte*)&latitud;
  byte* bLon = (byte*)&longitud;
  Wire2.write(bLat, 4);
  Wire2.write(bLon, 4);
  byte error = Wire2.endTransmission();

  if (error == 0) {
    Serial2.println(LOG_PREFIX " OK: transmision correcta");
  } else {
    Serial2.print(LOG_PREFIX " ERROR: codigo ");
    Serial2.println(error);
  }
  delay(500);
}
