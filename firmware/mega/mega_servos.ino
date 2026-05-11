/*
 * BrazoRoboticoV2 — Firmware Arduino Mega 2560
 * =============================================
 *
 * Controla 6 servos MG996R (5 dedos + muñeca) con protocolo serial V2.
 * Recibe comandos desde el UNO Q (Flask) vía Serial1 a 115200 baud.
 *
 * Arquitectura de comunicación:
 *   UNO Q (Flask) → TCP:7500 → SOCAT → /dev/ttyGS0 → STM32 Bridge →
 *   Serial1(D0/D1) → Mega Serial1(RX1=D19, TX1=D18)
 *
 * Protocolo V2:
 *   - Command:   F<idx> <pwm_us>\n   (ej: "F0 1500\n")
 *   - Heartbeat: H\n                  (cada 500ms desde Flask)
 *   - ACK:       OK\n                 (respuesta a heartbeat)
 *
 * Features:
 *   - Buffer estático (sin String / memoria dinámica)
 *   - Aceleración trapezoidal (movimiento suave sin tirones)
 *   - Watchdog heartbeat (safe pose automática en timeout)
 *   - ACK en heartbeat (OK\n)
 *   - Monitor USB para debug con toggle
 *
 * Pines:
 *   D3  ─ Servo 0  ─ Pulgar  (Thumb)
 *   D5  ─ Servo 1  ─ Índice  (Index)
 *   D6  ─ Servo 2  ─ Medio   (Middle)
 *   D9  ─ Servo 3  ─ Anular  (Ring)
 *   D10 ─ Servo 4  ─ Meñique (Pinky)
 *   D11 ─ Servo 5  ─ Muñeca  (Wrist)
 *
 * Serial1: RX1=D19, TX1=D18  — comunicación con UNO Q
 * Serial:  USB              — debug/monitor por terminal
 *
 * Fecha:   2026-05-10
 * Versión: 2.0.0
 */

#include <Servo.h>

// ═══════════════════════════════════════════════════════════
//  CONSTANTES
// ═══════════════════════════════════════════════════════════

#define NUM_SERVOS      6
#define SERIAL_BAUD     115200

// Límites del parser y buffer
#define CMD_BUF_SIZE    16       // Suficiente para "F4 2500\n\0"
#define PWM_MIN_US      500      // Límite inferior hardware MG996R (0°)
#define PWM_MAX_US      2500     // Límite superior hardware MG996R (180°)
#define PWM_CENTER_US   1500     // Safe pose / centro (90°)

// Watchdog
#define HEARTBEAT_TIMEOUT_MS  2500   // 2.5s sin heartbeat → safe pose
                                     // (margen sobre los 2s del backend)
// Interpolación trapezoidal
#define STEP_INTERVAL_MS      10     // Intervalo entre pasos de interpolación (ms)
#define ACCEL_STEP            2      // Incremento/decremento de velocidad (µs/paso²)
#define MAX_SPEED             8      // Velocidad máxima (µs/paso)
#define MIN_SPEED             1      // Velocidad mínima (> 0 para movimiento continuo)

// ═══════════════════════════════════════════════════════════
//  PINES DE LOS SERVOS
// ═══════════════════════════════════════════════════════════

const uint8_t SERVO_PINS[NUM_SERVOS] = {3, 5, 6, 9, 10, 11};

// ═══════════════════════════════════════════════════════════
//  ESTADO GLOBAL DE LOS SERVOS
// ═══════════════════════════════════════════════════════════

Servo servos[NUM_SERVOS];        // Objetos Servo (librería estándar)
int current_pwm[NUM_SERVOS];     // PWM actual (µs)
int target_pwm[NUM_SERVOS];      // PWM destino  (µs)
int velocity[NUM_SERVOS];        // Velocidad actual del movimiento (µs/paso)

// ═══════════════════════════════════════════════════════════
//  PARSER DE COMANDOS — MÁQUINA DE ESTADOS
// ═══════════════════════════════════════════════════════════
//
// Estados del autómata:
//   WAITING_F  → esperando 'F' para comando, o 'H' para heartbeat
//   READY_IDX  → 'F' recibido, esperando dígito del índice (0-5)
//   READY_PWM  → índice recibido, acumulando dígitos del PWM
//   NEWLINE    → '\n' recibido, comando listo para ejecutar
//
// Formato: F<idx> <pwm_us>\n

enum ParserState {
  WAITING_F,
  READY_IDX,
  READY_PWM,
  NEWLINE
};

ParserState pstate = WAITING_F;
char cmd_buffer[CMD_BUF_SIZE];    // Buffer estático para dígitos del PWM
uint8_t cmd_pos = 0;              // Posición actual en cmd_buffer
int cmd_idx = 0;                  // Índice del servo parseado (0-5)
int cmd_pwm = 0;                  // PWM parseado (µs)

// ═══════════════════════════════════════════════════════════
//  WATCHDOG — TIMER DE SEGURIDAD
// ═══════════════════════════════════════════════════════════
//
// Se resetea con CADA comando válido (F o H) recibido.
// Si transcurren HEARTBEAT_TIMEOUT_MS sin ningún comando →
// todos los servos van a Safe Pose (1500 µs).

unsigned long last_command_ms = 0;   // Último comando o heartbeat recibido
unsigned long last_step_ms = 0;      // Último paso de interpolación

// ═══════════════════════════════════════════════════════════
//  MONITOR USB — MODO DEBUG
// ═══════════════════════════════════════════════════════════
//
// Cuando está activo, todo el tráfico de Serial1 se refleja en USB.
// Se activa enviando 'M' por USB (sin newline).

bool monitor_mode = false;

// ═══════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════

void setup() {
  // Inicializar Serial USB (debug)
  Serial.begin(SERIAL_BAUD);

  // Inicializar Serial1 (comunicación con UNO Q vía STM32 bridge)
  Serial1.begin(SERIAL_BAUD);

  // Esperar puerto USB hasta 3s (en Mega el Serial nativo no necesita
  // while(!Serial) pero lo dejamos por compatibilidad con placas que sí)
  unsigned long timeout_start = millis();
  while (!Serial && (millis() - timeout_start < 3000)) {
    // Espera activa por el monitor serie
  }

  // Inicializar servos en Safe Pose
  for (int i = 0; i < NUM_SERVOS; i++) {
    servos[i].attach(SERVO_PINS[i]);
    servos[i].writeMicroseconds(PWM_CENTER_US);
    current_pwm[i] = PWM_CENTER_US;
    target_pwm[i] = PWM_CENTER_US;
    velocity[i] = 0;
  }

  // Inicializar temporizadores
  last_command_ms = millis();
  last_step_ms = millis();

  // Mensaje de bienvenida por USB
  Serial.println(F("BRAZO-V2 MEGA: READY"));
  Serial.print(F("Servos en pines: "));
  for (int i = 0; i < NUM_SERVOS; i++) {
    Serial.print(SERVO_PINS[i]);
    if (i < NUM_SERVOS - 1) Serial.print(F(", "));
  }
  Serial.println();
  Serial.println(F("Protocolo: F<idx> <pwm_us>\\n | Heartbeat: H\\n"));
  Serial.println(F("Monitor: envie 'M' por USB para toggle"));
}

// ═══════════════════════════════════════════════════════════
//  LOOP PRINCIPAL
// ═══════════════════════════════════════════════════════════

void loop() {
  // ── Leer Serial1 (desde UNO Q vía STM32) ──────────────
  while (Serial1.available() > 0) {
    char c = Serial1.read();
    parse_char(c);

    // Passthrough a USB si el monitor está activo
    if (monitor_mode) {
      Serial.write(c);
    }
  }

  // ── Leer Serial (USB debug) → passthrough a Serial1 ───
  // Permite enviar comandos manuales desde el monitor serie
  // para depuración sin necesidad del UNO Q.
  while (Serial.available() > 0) {
    char c = Serial.read();

    // 'M' togglea el modo monitor
    if (c == 'M') {
      monitor_mode = !monitor_mode;
      Serial.print(F("Monitor: "));
      Serial.println(monitor_mode ? F("ON") : F("OFF"));
    } else {
      // Cualquier otro carácter se reenvía a Serial1
      Serial1.write(c);
    }
  }

  // ── Paso de interpolación (movimiento suave) ──────────
  unsigned long now = millis();
  if (now - last_step_ms >= STEP_INTERVAL_MS) {
    last_step_ms = now;
    update_servos();
  }

  // ── Watchdog ──────────────────────────────────────────
  check_watchdog();
}

// ═══════════════════════════════════════════════════════════
//  PARSER DE COMANDOS
// ═══════════════════════════════════════════════════════════
//
// Autómata de estados para parsear:
//   - Comando:    F<idx> <pwm_us>\n
//   - Heartbeat:  H\n
//
// El watchdog se resetea con CADA comando o heartbeat válido
// recibido (según ADR-0002).

void parse_char(char c) {
  // ── Heartbeat: 'H' se procesa en CUALQUIER estado ─────
  // Esto garantiza robustez: si llega ruido y el parser queda
  // en un estado intermedio, el heartbeat sigue funcionando.
  // Además resetea el parser a WAITING_F.
  if (c == 'H') {
    Serial1.println(F("OK"));          // ACK al UNO Q
    last_command_ms = millis();        // ADR-0002: heartbeat resetea watchdog
    pstate = WAITING_F;                // Reset del parser
    cmd_pos = 0;
    return;
  }

  // ── Inicio de comando: 'F' ────────────────────────────
  if (c == 'F' && pstate == WAITING_F) {
    pstate = READY_IDX;
    cmd_pos = 0;
    cmd_buffer[0] = '\0';
    cmd_idx = 0;
    return;
  }

  // ── READY_IDX: esperando dígito del índice (0-5) ──────
  if (pstate == READY_IDX) {
    if (c >= '0' && c <= '5') {
      cmd_idx = c - '0';               // Conversión ASCII → entero
      pstate = READY_PWM;
      cmd_pos = 0;
      cmd_buffer[0] = '\0';
    } else {
      // Carácter inesperado: reset
      pstate = WAITING_F;
    }
    return;
  }

  // ── READY_PWM: acumulando dígitos del valor PWM ───────
  if (pstate == READY_PWM) {
    // Ignorar espacios (separador entre índice y PWM)
    if (c == ' ') {
      return;
    }

    // Acumular dígitos en el buffer estático
    if (c >= '0' && c <= '9') {
      if (cmd_pos < CMD_BUF_SIZE - 1) {
        cmd_buffer[cmd_pos++] = c;
        cmd_buffer[cmd_pos] = '\0';
      }
      return;
    }

    // Fin de línea: ejecutar comando
    if (c == '\n') {
      if (cmd_pos > 0) {
        cmd_pwm = atoi(cmd_buffer);                 // ASCII → entero
        cmd_pwm = constrain(cmd_pwm, PWM_MIN_US, PWM_MAX_US);  // Clampeo seguro
        target_pwm[cmd_idx] = cmd_pwm;              // Actualizar target
        last_command_ms = millis();                  // ADR-0002: comando resetea watchdog
      }
      pstate = WAITING_F;
      return;
    }

    // Carácter inesperado: reset
    pstate = WAITING_F;
  }

  // ── WAITING_F + '\n' suelto: ignorar ──────────────────
  // (evita resets por newlines huérfanos)
}

// ═══════════════════════════════════════════════════════════
//  MOVIMIENTO SUAVE — ACELERACIÓN TRAPEZOIDAL
// ═══════════════════════════════════════════════════════════
//
// Implementa un perfil de velocidad trapezoidal:
//
//   Velocidad
//      ↑
//   MAX ──────╱╲──────────
//      ╱╲    ╱  ╲    ╱╲
//     ╱  ╲  ╱    ╲  ╱  ╲
//    ╱    ╲╱      ╲╱    ╲
//   ─┴──────┴──────┴──────┴──→ Distancia
//     Acel   Crucero  Decel
//
// Fórmula de desaceleración: dist = v² / (2 * a)
// Donde v es la velocidad actual y a la desaceleración (ACCEL_STEP).

void update_servos() {
  for (int i = 0; i < NUM_SERVOS; i++) {
    int diff = target_pwm[i] - current_pwm[i];   // Diferencia con signo

    if (abs(diff) <= ACCEL_STEP) {
      // ── Llegó al target (dentro de 1 paso de aceleración) ──
      // Snap directo para evitar micro-oscilaciones
      current_pwm[i] = target_pwm[i];
      velocity[i] = 0;
    } else {
      // ── En movimiento: calcular perfil trapezoidal ─────────
      int direction = (diff > 0) ? 1 : -1;
      int dist_to_target = abs(diff);

      // Distancia necesaria para frenar desde la velocidad actual
      // Usando cinemática: v² = 2·a·d  →  d = v² / (2·a)
      // Como trabajamos con enteros, usamos división entera.
      // MAX_SPEED=8 → d_freno = 64/4 = 16 pasos.
      int decel_distance = (velocity[i] * velocity[i]) / (2 * ACCEL_STEP);

      if (dist_to_target <= decel_distance) {
        // ── ZONA DE DESACELERACIÓN ──────────────────────
        // Reducir velocidad pero nunca por debajo de MIN_SPEED
        // para evitar que el servo se detenga antes de llegar.
        if (velocity[i] > MIN_SPEED) {
          velocity[i] = max(velocity[i] - ACCEL_STEP, MIN_SPEED);
        }
      } else if (velocity[i] < MAX_SPEED) {
        // ── ZONA DE ACELERACIÓN ─────────────────────────
        // Aumentar velocidad hasta el máximo permitido
        velocity[i] = min(velocity[i] + ACCEL_STEP, MAX_SPEED);
      }
      // else: ZONA DE CRUCERO — mantener MAX_SPEED

      // Aplicar movimiento
      current_pwm[i] += direction * velocity[i];
      current_pwm[i] = constrain(current_pwm[i], PWM_MIN_US, PWM_MAX_US);
    }

    // Escribir PWM al servo
    servos[i].writeMicroseconds(current_pwm[i]);

    // debug USB (opcional, comentar para producción)
    // Serial.print("S"); Serial.print(i);
    // Serial.print(" T:"); Serial.print(target_pwm[i]);
    // Serial.print(" C:"); Serial.print(current_pwm[i]);
    // Serial.print(" V:"); Serial.println(velocity[i]);
  }
}

// ═══════════════════════════════════════════════════════════
//  WATCHDOG — TIMEOUT DE SEGURIDAD
// ═══════════════════════════════════════════════════════════
//
// Si no se recibe ningún comando (F o H) durante
// HEARTBEAT_TIMEOUT_MS, se asume que la comunicación
// con el UNO Q está caída y se lleva todos los servos
// a Safe Pose (1500 µs).
//
// El timeout se reporta por USB la primera vez que ocurre.
// Cuando la comunicación se restablece, el flag se resetea
// para poder reportar el próximo timeout.

static bool watchdog_triggered = false;   // Flag de reporte único

void check_watchdog() {
  unsigned long now = millis();

  // ── Detección de overflow de millis() ──────────────────
  // millis() vuelve a 0 cada ~50 días. En un sistema
  // embebido que funciona 24/7 esto puede ocurrir.
  // Si el tiempo actual es menor que el último reset,
  // es porque millis() hizo wraparound.
  if (now < last_command_ms) {
    // Wraparound detectado: resetear contadores
    last_command_ms = now;
    last_step_ms = now;
    watchdog_triggered = false;
    return;
  }

  // ── Timeout ────────────────────────────────────────────
  if ((now - last_command_ms) > HEARTBEAT_TIMEOUT_MS) {
    // ¡Timeout! Llevar todos los servos a Safe Pose
    for (int i = 0; i < NUM_SERVOS; i++) {
      target_pwm[i] = PWM_CENTER_US;
    }
    last_command_ms = now;   // Evita re-trigger continuo en cada loop

    // Notificar por USB una sola vez por evento de timeout
    if (!watchdog_triggered) {
      Serial.println(F("WATCHDOG: Timeout! Safe Pose activado."));
      watchdog_triggered = true;
    }
  } else {
    // Comunicación restablecida: resetear flag para próximo timeout
    watchdog_triggered = false;
  }
}
