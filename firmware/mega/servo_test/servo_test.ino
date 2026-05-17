/*
 * Servo Test — BrazoRoboticoV2
 * Prueba individual de cada servo conectado a pines D2-D7.
 * 
 * Mueve cada servo de 1000 a 2000 µs en pasos de 50µs,
 * uno por uno, con pausa de 2s entre cada servo.
 * 
 * Pines:
 *   D2: Servo 0 (Pulgar)
 *   D3: Servo 1 (Índice)
 *   D4: Servo 2 (Corazón)
 *   D5: Servo 3 (Anular)
 *   D6: Servo 4 (Meñique)
 *   D7: Servo 5 (Muñeca)
 */

#include <Servo.h>

#define NUM_SERVOS 6
#define SERVO_MIN 1000
#define SERVO_MAX 2000
#define SERVO_CENTER 1500
#define STEP_DELAY 20
#define PAUSE_BETWEEN 2000

const uint8_t PINS[NUM_SERVOS] = {2, 3, 4, 5, 6, 7};
Servo servos[NUM_SERVOS];

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  
  Serial.println(F("=== SERVO TEST ==="));
  Serial.print(F("Pines: "));
  for (int i = 0; i < NUM_SERVOS; i++) {
    servos[i].attach(PINS[i]);
    servos[i].writeMicroseconds(SERVO_CENTER);
    Serial.print(PINS[i]);
    if (i < NUM_SERVOS - 1) Serial.print(F(", "));
  }
  Serial.println();
  delay(1000);
}

void loop() {
  // Probar cada servo individualmente
  for (int s = 0; s < NUM_SERVOS; s++) {
    Serial.print(F("Servo "));
    Serial.print(s);
    Serial.print(F(" (pin D"));
    Serial.print(PINS[s]);
    Serial.println(F("):"));
    
    // Abrir gradualmente (1000 → 2000)
    for (int pwm = SERVO_MIN; pwm <= SERVO_MAX; pwm += 50) {
      servos[s].writeMicroseconds(pwm);
      Serial.print(pwm);
      Serial.print(F(" "));
      delay(STEP_DELAY);
    }
    Serial.println(F(" OK"));
    
    delay(500);
    
    // Cerrar gradualmente (2000 → 1000)
    for (int pwm = SERVO_MAX; pwm >= SERVO_MIN; pwm -= 50) {
      servos[s].writeMicroseconds(pwm);
      Serial.print(pwm);
      Serial.print(F(" "));
      delay(STEP_DELAY);
    }
    Serial.println(F(" OK"));
    
    // Volver al centro
    servos[s].writeMicroseconds(SERVO_CENTER);
    Serial.println(F("→ Centro"));
    delay(PAUSE_BETWEEN);
  }
  
  // Todos los servos juntos
  Serial.println(F("\n=== TODOS JUNTOS ==="));
  for (int pwm = SERVO_MIN; pwm <= SERVO_MAX; pwm += 100) {
    for (int s = 0; s < NUM_SERVOS; s++) {
      servos[s].writeMicroseconds(pwm);
    }
    Serial.print(pwm);
    Serial.print(F(" "));
    delay(50);
  }
  Serial.println(F(" OK"));
  
  for (int pwm = SERVO_MAX; pwm >= SERVO_MIN; pwm -= 100) {
    for (int s = 0; s < NUM_SERVOS; s++) {
      servos[s].writeMicroseconds(pwm);
    }
    Serial.print(pwm);
    Serial.print(F(" "));
    delay(50);
  }
  Serial.println(F(" OK"));
  
  // Centro todos
  for (int s = 0; s < NUM_SERVOS; s++) {
    servos[s].writeMicroseconds(SERVO_CENTER);
  }
  Serial.println(F("→ Centro todos"));
  
  delay(5000);
  Serial.println(F("\n=== REPETIR ===\n"));
}
