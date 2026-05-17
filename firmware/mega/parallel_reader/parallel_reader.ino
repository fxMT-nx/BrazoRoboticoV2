/*
 * Parallel Reader - Mega 2560
 * Recibe comandos F<idx> <pwm_us> por puerto paralelo de 8 bits
 * Protocolo nuevo (sin pines ID):
 *   Byte 1: ID del servo (0-5)
 *   Byte 2: PWM bits altos (>> 4)
 *   Byte 3: PWM bits bajos (& 0xFF)
 * 
 * Pines:
 *   D30-D37 = datos (8 bits, LSB en D30)
 *   D39     = STROBE (flanco subida = dato listo)
 *   D38     = no usado (antes STROBE)
 *   D40-D41 = no usados (antes ID)
 */

#include <Servo.h>

#define STROBE 39
#define NUM_SERVOS 6
const uint8_t SERVO_PINS[NUM_SERVOS] = {7, 6, 5, 4, 3, 2};

Servo servos[NUM_SERVOS];
int current_pwm[NUM_SERVOS] = {1500,1500,1500,1500,1500,1500};
int target_pwm[NUM_SERVOS] = {1500,1500,1500,1500,1500,1500};
int velocity[NUM_SERVOS] = {0};

volatile byte recv_buffer[3];
volatile int recv_pos = 0;
volatile bool cmd_ready = false;
volatile byte cmd_servo = 0;
volatile int cmd_pwm = 0;

void setup() {
  Serial.begin(115200);
  
  for (int i = 30; i <= 39; i++) {
    pinMode(i, INPUT);
  }
  
  attachInterrupt(digitalPinToInterrupt(STROBE), onStrobe, RISING);
  
  for (int i = 0; i < NUM_SERVOS; i++) {
    servos[i].attach(SERVO_PINS[i]);
    servos[i].writeMicroseconds(1500);
    current_pwm[i] = 1500;
    target_pwm[i] = 1500;
  }
  
  Serial.println("PARALLEL READER READY (STROBE=D39, 3-byte protocol)");
  Serial.print("Servos pines: ");
  for (int i = 0; i < NUM_SERVOS; i++) {
    Serial.print(SERVO_PINS[i]);
    if (i < NUM_SERVOS-1) Serial.print(", ");
  }
  Serial.println();
}

void loop() {
  if (cmd_ready) {
    cmd_ready = false;
    if (cmd_servo < NUM_SERVOS) {
      target_pwm[cmd_servo] = cmd_pwm;
      Serial.print("F");
      Serial.print(cmd_servo);
      Serial.print(" ");
      Serial.println(cmd_pwm);
    }
  }
  
  // Interpolación trapezoidal simple
  for (int i = 0; i < NUM_SERVOS; i++) {
    int diff = target_pwm[i] - current_pwm[i];
    if (diff > 0) {
      current_pwm[i] += min(diff, 5);
    } else if (diff < 0) {
      current_pwm[i] += max(diff, -5);
    }
    if (current_pwm[i] != current_pwm[i] || current_pwm[i] < 500) current_pwm[i] = 1500;
    servos[i].writeMicroseconds(current_pwm[i]);
  }
  
  delay(10);
}

void onStrobe() {
  byte data = 0;
  for (int bit = 0; bit < 8; bit++) {
    if (digitalRead(30 + bit)) data |= (1 << bit);
  }
  
  if (recv_pos < 3) recv_buffer[recv_pos++] = data;
  if (recv_pos >= 3) {
    cmd_servo = recv_buffer[0];
    cmd_pwm = ((int)recv_buffer[1] << 4) | recv_buffer[2];
    cmd_pwm = constrain(cmd_pwm, 500, 2500);
    cmd_ready = true;
    recv_pos = 0;
  }
}
