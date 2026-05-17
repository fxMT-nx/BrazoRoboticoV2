/*
 * Parallel Writer - STM32U585 (UNO Q)
 * Envia comando F<idx> <pwm_us> por puerto paralelo de 8 bits
 * Protocolo nuevo (sin pines ID):
 *   Byte 1: ID del servo (0-5)
 *   Byte 2: PWM bits altos (>> 4)
 *   Byte 3: PWM bits bajos (& 0xFF)
 * 
 * Pines:
 *   D2-D9  = datos (8 bits, LSB en D2)
 *   D11    = STROBE (flanco subida = dato listo)
 *   D10    = no usado (antes STROBE)
 *   D12-D13 = no usados (antes ID)
 */

#define STROBE 11

void setup() {
  for (int i = 2; i <= 11; i++) {
    pinMode(i, OUTPUT);
    digitalWrite(i, LOW);
  }
  Serial2.begin(115200);
  Serial2.println("PARALLEL WRITER READY (STROBE=D11, 3-byte protocol)");
}

void sendByte(byte data) {
  for (int bit = 0; bit < 8; bit++) {
    digitalWrite(2 + bit, (data >> bit) & 1);
  }
  digitalWrite(STROBE, HIGH);
  delayMicroseconds(5);
  digitalWrite(STROBE, LOW);
  delayMicroseconds(5);
}

void sendPWM(int servo, int pwm) {
  sendByte(servo);          // Byte 1: ID del servo
  sendByte(pwm >> 4);       // Byte 2: PWM bits altos
  sendByte(pwm & 0xFF);     // Byte 3: PWM bits bajos
}

void loop() {
  // Enviar secuencia de prueba: mover cada servo de 1000 a 2000 y vuelta
  for (int servo = 0; servo < 6; servo++) {
    // Abrir (1000)
    sendPWM(servo, 1000);
    delay(200);
    // Cerrar (2000)
    sendPWM(servo, 2000);
    delay(200);
    // Centro (1500)
    sendPWM(servo, 1500);
    delay(100);
  }
  
  Serial2.println("Ciclo completo");
  delay(1000);
}
