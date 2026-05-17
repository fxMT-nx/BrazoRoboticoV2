void setup() {
  pinMode(39, INPUT);
  Serial.begin(115200);
  Serial.println("TEST D39");
}
void loop() {
  Serial.println(digitalRead(39));
  delay(200);
}
