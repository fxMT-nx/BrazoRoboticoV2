void setup() {
  pinMode(38, INPUT);
  Serial.begin(115200);
  Serial.println("TEST: Esperando senal en D38...");
}
void loop() {
  int val = digitalRead(38);
  Serial.println(val);
  delay(100);
}
