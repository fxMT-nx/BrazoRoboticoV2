void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.print("A0=");
  Serial.println(A0);
  Serial.print("A1=");
  Serial.println(A1);
  Serial.print("A2=");
  Serial.println(A2);
  Serial.print("A3=");
  Serial.println(A3);
  Serial.print("A4=");
  Serial.println(A4);
  Serial.print("A5=");
  Serial.println(A5);
  Serial.print("LED_BUILTIN=");
  Serial.println(LED_BUILTIN);
}
void loop() {}
