void setup() {
  pinMode(22, INPUT);
  Serial.begin(115200);
  Serial.println("TEST: Leyendo D22 cada 200ms");
}
void loop() {
  Serial.println(digitalRead(22));
  delay(200);
}
