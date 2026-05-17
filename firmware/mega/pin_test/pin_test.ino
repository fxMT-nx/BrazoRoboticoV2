#include <Servo.h>
Servo s;
void setup() {
  s.attach(7);
  Serial.begin(115200);
  Serial.println("MOVIENDO PIN D7...");
  for (int i = 0; i < 3; i++) {
    for (int p = 1000; p <= 2000; p += 20) { s.writeMicroseconds(p); delay(15); }
    for (int p = 2000; p >= 1000; p -= 20) { s.writeMicroseconds(p); delay(15); }
  }
  s.writeMicroseconds(1500);
  Serial.println("FIN PIN D7");
}
void loop() {}
