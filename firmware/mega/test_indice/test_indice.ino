#include <Servo.h>
Servo s;
void setup() {
  s.attach(6);  // D6 = Índice
  Serial.begin(115200);
  Serial.println("MOVIENDO INDICE (D6)...");
  for (int i = 0; i < 5; i++) {
    s.writeMicroseconds(1000);
    delay(800);
    s.writeMicroseconds(2000);
    delay(800);
  }
  s.writeMicroseconds(1500);
  Serial.println("FIN");
}
void loop() {}
