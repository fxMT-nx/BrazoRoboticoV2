void setup() {
  pinMode(22, INPUT);
  Serial.begin(115200);
  Serial.println("TEST VOLTAJE: UNO Q D2 (3.3V) -> Mega D22");
}
void loop() {
  int val = digitalRead(22);
  Serial.println(val);
  delay(100);
}
