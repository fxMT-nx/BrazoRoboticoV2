void setup() {
  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);
}
void loop() {
  digitalWrite(2, HIGH);
  delay(1000);
  digitalWrite(2, LOW);
  delay(1000);
}
