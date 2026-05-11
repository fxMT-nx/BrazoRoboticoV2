/*
 * BrazoRoboticoV2 — STM32U585 Bridge Sketch para Arduino UNO Q (ABX00162)
 * ===========================================================================
 *
 * ─── Rol ───────────────────────────────────────────────────────────────────
 * Este sketch corre en el **STM32U585** del Arduino UNO Q. Actúa como bridge
 * serial entre el Qualcomm QRB2210 (Debian Linux, Flask) y el Arduino Mega
 * 2560. También controla la **LED Matrix 12×8** integrada en el UNO Q para
 * mostrar el estado del sistema (número de dedos levantados o smiley).
 *
 * ─── Arquitectura ──────────────────────────────────────────────────────────
 *   QRB2210 (Flask) → USB CDC ACM → STM32 (Serial) → Serial1 (USART1 D0/D1)
 *   → Mega 2560 (Serial1) → PWM → Servos MG996R ×6
 *
 *   En detalle:
 *   Flask → TCP:7500 → SOCAT → /dev/ttyGS0 → USB Gadget → STM32 Serial
 *   → Serial1 (D0/D1, USART1, 115200 baud) → Mega Serial1
 *
 * ─── Mapeo Serial del STM32U585 ────────────────────────────────────────────
 *   Serial  = Serial2 = LPUART1 → USB CDC ACM (desde QRB2210 vía SOCAT)
 *   Serial1 = USART1 → pines D0 (RX) y D1 (TX) del header → Mega Serial1
 *   Serial2 = LPUART1 (alias, no usar — compite con el router)
 *
 * ─── Protocolo V2 ──────────────────────────────────────────────────────────
 *   F<idx> <pwm_us>\n  — Command: mueve el servo idx al PWM especificado
 *                         idx: 0-5 (thumb, index, middle, ring, pinky, wrist)
 *                         pwm_us: 800-2200 µs
 *   H\n                 — Heartbeat: keep-alive desde Flask
 *   OK\n                — ACK: respuesta a Heartbeat
 *   D\n                 — Debug: toggle modo debug (muestra raw por USB)
 *
 * ─── LED Matrix (12×8 LEDs rojos) ──────────────────────────────────────────
 *   1-5 dedos levantados → muestra el número con Font_5x7
 *   0 dedos / puño       → muestra smiley 🙂
 *   Sin tracking (idle)  → smiley fijo
 *   Error de comunicación → "E" parpadeante
 *   Heartbeat activo     → smiley parpadea suavemente (indicador de conexión)
 *
 * ─── Mejoras sobre V1 ──────────────────────────────────────────────────────
 *   1. ✅ Buffer circular para recepción serial (ISR-ready)
 *   2. ✅ Timeout LED: 3s sin comandos → smiley (modo idle)
 *   3. ✅ Parpadeo de heartbeat: smiley parpadea cuando hay conexión activa
 *   4. ✅ Modo debug: D\n toggle, muestra raw de comandos por USB
 *   5. ✅ Anti-parpadeo: 400ms de histéresis antes de cambiar display
 *   6. ✅ Sin malloc, buffer estático, volatile para contexto ISR
 *
 * ─── Librerías ─────────────────────────────────────────────────────────────
 *   - Arduino_LED_Matrix.h  (control de la matriz LED 12×8 del UNO Q)
 *   - ArduinoGraphics.h     (renderizado de texto Font_5x7 en la matriz)
 *
 * ===========================================================================
 * Autor:    BrazoRoboticoV2 Team
 * Licencia: MIT
 * ===========================================================================
 */

#include <ArduinoGraphics.h>
#include <Arduino_LED_Matrix.h>

// ─── Instancia global de la LED Matrix ────────────────────────────────────
Arduino_LED_Matrix matrix;

// ══════════════════════════════════════════════════════════════════════════
//  CONSTANTES
// ══════════════════════════════════════════════════════════════════════════

// ── Velocidad de los puertos seriales ─────────────────────────────────────
// La socat.service usa b115200 para /dev/ttyGS0 → USB CDC ACM.
// Serial1 se comunica con el Mega Serial1 a la misma velocidad.
static const unsigned long MEGA_BAUD    = 115200;
static const unsigned long SERIAL_BAUD  = 115200;

// ── Configuración de la mano robótica ─────────────────────────────────────
static const int NUM_SERVOS = 6;        // thumb, index, middle, ring, pinky, wrist
static const int PWM_MIN    = 800;      // Límite seguro inferior (servo_calibration.yaml)
static const int PWM_MAX    = 2200;     // Límite seguro superior
static const int PWM_CENTER = 1500;     // Safe pose / posición central

// ── Umbrales para detección de dedo "abierto" ────────────────────────────
// Coinciden con servo_calibration.yaml:
//   thumb_threshold_open_pwm: 1200
//   otros: open_pwm=1000, closed_pwm=2000 → umbral a 1150 (~15% flexión)
static const int THUMB_OPEN_PWM   = 1200;   // PWM por debajo = pulgar abierto
static const int FINGER_OPEN_PWM  = 1150;   // PWM por debajo = dedo abierto
static const int WRIST_OPEN_PWM   = 1200;   // PWM por debajo = muñeca extendida

// ── Temporizadores ────────────────────────────────────────────────────────
static const unsigned long IDLE_TIMEOUT_MS   = 3000;   // 3s sin comandos → idle
static const unsigned long POSE_HOLD_MS      = 400;    // Anti-parpadeo (histéresis)
static const unsigned long HEARTBEAT_TIMEOUT_MS = 2000; // 2s sin heartbeat → sin conexión
// ── Animaciones de la LED Matrix ──────────────────────────────────
static const unsigned long EYE_BLINK_INTERVAL_MS  = 3000;  // Período de parpadeo de ojos
static const unsigned long EYE_BLINK_DURATION_MS  = 150;   // Duración del parpadeo (ms)
static const unsigned long PULSE_INTERVAL_MS      = 1000;  // Período de pulso de heartbeat
static const unsigned long RECORDING_PULSE_ON_MS  = 800;   // On time del icono grabación
static const unsigned long RECORDING_PULSE_OFF_MS = 400;   // Off time del icono grabación
static const unsigned long ERROR_BLINK_MS         = 500;   // Período de parpadeo de error
static const unsigned long LOADING_STEP_MS        = 400;   // Paso de animación de carga

// ── Buffer circular para recepción serial ────────────────────────────────
// Tamaño: 128 bytes — suficiente para ráfagas de hasta ~25 comandos completos
// (cada comando F<idx> <pwm>\n tiene ~11 bytes máximo).
// Usamos volatile porque este buffer puede ser llenado desde ISR en el futuro.
static const uint8_t RX_BUF_SIZE = 128;
static volatile char rx_buffer[RX_BUF_SIZE];
static volatile uint8_t rx_head = 0;    // Índice de escritura (productor)
static volatile uint8_t rx_tail = 0;    // Índice de lectura (consumidor)

// ══════════════════════════════════════════════════════════════════════════
//  ESTADO GLOBAL
// ══════════════════════════════════════════════════════════════════════════

// ── PWM actual de cada servo (en µs) ─────────────────────────────────────
static int servo_pwm[NUM_SERVOS] = {
    PWM_CENTER, PWM_CENTER, PWM_CENTER, PWM_CENTER, PWM_CENTER, PWM_CENTER
};

// ── Temporización ─────────────────────────────────────────────────────────
static unsigned long last_cmd_time       = 0;   // Último comando F<idx> recibido
static unsigned long last_pose_change    = 0;   // Último cambio en LED Matrix
static unsigned long last_heartbeat_time = 0;   // Último Heartbeat H recibido

// ── Display ───────────────────────────────────────────────────────────────
// current_display: -1 = sin display, 0 = smiley, 1-5 = número del dedo
static int current_display = -1;

// ── Heartbeat ─────────────────────────────────────────────────────────────
static bool heartbeat_seen = false;     // true si hemos recibido al menos un H

// ── Debug ─────────────────────────────────────────────────────────────────
static bool debug_mode = false;

// ── Animaciones ───────────────────────────────────────────────────────────
static unsigned long last_blink_ms = 0;
static bool blink_state = false;        // true = ojos cerrados (parpadeo)

static unsigned long last_pulse_ms = 0;
static bool pulse_state = false;        // true = pulso atenuado

static unsigned long last_recording_pulse_ms = 0;
static bool recording_pulse_state = false;

static unsigned long last_error_blink_ms = 0;
static bool error_blink_state = false;

static unsigned long last_loading_ms = 0;
static int loading_step = 0;

// ── Control de redibujo ──────────────────────────────────
static bool display_dirty = true;   // true = necesita redibujar

// ══════════════════════════════════════════════════════════════════════════
//  PARSER DE COMANDOS (MÁQUINA DE ESTADOS)
// ══════════════════════════════════════════════════════════════════════════

// Estados del parser de comandos F<idx> <pwm>\n
enum ParserState : uint8_t {
    PARSER_WAITING_F,     // Esperando 'F' inicial
    PARSER_READY_IDX,     // Leyendo índice del servo (1 dígito: 0-5)
    PARSER_READY_PWM,     // Leyendo valor PWM (hasta 4 dígitos + '\n')
};

static ParserState parser_state = PARSER_WAITING_F;
static char parse_buffer[8];          // Buffer temporal para el valor PWM
static uint8_t parse_pos = 0;         // Posición en parse_buffer
static int parse_idx = 0;             // Índice del servo siendo parseado

// ─── Procesa un byte individual del protocolo V2 ─────────────────────────
// Cada byte recibido por USB puede ser:
//   - Inicio de comando 'F' → máquina de estados para F<idx> <pwm>\n
//   - Heartbeat 'H'        → respuesta OK\n
//   - Debug 'D'            → toggle debug mode
//   - Cualquier otro byte  → se ignora (se reenvía al Mega transparentemente)
//
// Postcondición: si el comando es válido, actualiza servo_pwm[] y last_cmd_time
static void processByte(const char c) {
    // ── Modo debug: mostrar byte raw (excepto newline) ──────────────
    if (debug_mode && c != '\n') {
        Serial.print("RAW: 0x");
        Serial.print(static_cast<uint8_t>(c), HEX);
        Serial.print(" '");
        if (c >= 32 && c < 127) {
            Serial.write(c);
        }
        Serial.println("'");
    }

    // ── Comando Heartbeat: H\n ──────────────────────────────────────
    // Responde con OK\n y marca el heartbeat como recibido.
    // El Mega también recibe H\n a través de Serial1 y maneja su propio
    // watchdog de 2s.
    if (c == 'H' && parser_state == PARSER_WAITING_F) {
        heartbeat_seen = true;
        last_heartbeat_time = millis();
        Serial.println("OK");
        return;
    }

    // ── Comando Debug: D\n ──────────────────────────────────────────
    // Toggle del modo debug. Cuando está activo, muestra por USB todos
    // los bytes recibidos en formato raw y los comandos parseados.
    if (c == 'D' && parser_state == PARSER_WAITING_F) {
        debug_mode = !debug_mode;
        Serial.print("DEBUG ");
        Serial.println(debug_mode ? "ON" : "OFF");
        return;
    }

    // ── Máquina de estados para F<idx> <pwm_us>\n ───────────────────
    if (c == 'F') {
        parser_state = PARSER_READY_IDX;
        parse_pos = 0;
        parse_idx = 0;
        return;
    }

    // Estado: leyendo índice (0-5)
    if (parser_state == PARSER_READY_IDX) {
        if (c >= '0' && c <= '5') {
            parse_idx = c - '0';
            parser_state = PARSER_READY_PWM;
            parse_pos = 0;
            parse_buffer[0] = '\0';
        } else {
            // Carácter inesperado → reiniciar
            parser_state = PARSER_WAITING_F;
        }
        return;
    }

    // Estado: leyendo valor PWM (1-4 dígitos)
    if (parser_state == PARSER_READY_PWM) {
        // Ignorar espacios entre idx y pwm
        if (c == ' ') {
            return;
        }

        // Acumular dígitos en el buffer
        if (c >= '0' && c <= '9' && parse_pos < sizeof(parse_buffer) - 1) {
            parse_buffer[parse_pos++] = c;
            parse_buffer[parse_pos] = '\0';
            return;
        }

        // Fin de comando: newline
        if (c == '\n') {
            if (parse_pos > 0) {
                const int pwm_raw = atoi(parse_buffer);
                const int pwm = constrain(pwm_raw, PWM_MIN, PWM_MAX);
                servo_pwm[parse_idx] = pwm;
                last_cmd_time = millis();

                if (debug_mode) {
                    Serial.print("F");
                    Serial.print(parse_idx);
                    Serial.print(" ");
                    Serial.print(pwm);
                    Serial.println(" µs");
                }
            }
            parser_state = PARSER_WAITING_F;
            return;
        }

        // Carácter inesperado dentro del PWM → reiniciar
        parser_state = PARSER_WAITING_F;
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  BUFFER CIRCULAR — LECTURA DESDE SERIAL
// ══════════════════════════════════════════════════════════════════════════

// ─── Vacía Serial (USB CDC ACM) al buffer circular ──────────────────────
// Lee todos los bytes disponibles desde Serial y los almacena en el buffer
// circular. Si el buffer está lleno, descarta el byte más antiguo (política
// de overwrite para datos seriales en tiempo real).
//
// El buffer es ISR-ready: las variables head/tail son volátiles y el patrón
// permite que en el futuro un ISR llene el buffer mientras el loop consume.
static void drainSerialToBuffer(void) {
    while (Serial.available() > 0) {
        const uint8_t next = (rx_head + 1) % RX_BUF_SIZE;
        if (next != rx_tail) {
            // Buffer tiene espacio → escribir
            rx_buffer[rx_head] = static_cast<char>(Serial.read());
            rx_head = next;
        } else {
            // Buffer lleno → descartar el byte más antiguo
            // (avanzamos tail para sobrescribir la entrada más vieja)
            rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
            rx_buffer[rx_head] = static_cast<char>(Serial.read());
            rx_head = (rx_head + 1) % RX_BUF_SIZE;
        }
    }
}

// ─── Procesa toda la comunicación serial entrante ─────────────────────────
// 1. Vacía Serial (USB) al buffer circular
// 2. Para cada byte en el buffer: procesa localmente y reenvía al Mega
// 3. Reenvía respuestas del Mega de vuelta al QRB2210 por USB
//
// Este diseño asegura que:
//   - Todos los comandos del QRB2210 llegan al Mega (transparencia)
//   - El STM32 también parsea los comandos para la LED Matrix
//   - Las respuestas del Mega vuelven al QRB2210
static void processSerialInput(void) {
    // ── Paso 1: vaciar USB al buffer circular ───────────────────────
    drainSerialToBuffer();

    // ── Paso 2: procesar bytes del buffer ───────────────────────────
    // Cada byte se parsea localmente (para LED Matrix) y se reenvía
    // al Mega (para control de servos). Esto es intencional: ambos
    // procesadores interpretan el mismo flujo de comandos de forma
    // independiente.
    while (rx_tail != rx_head) {
        const char c = rx_buffer[rx_tail];
        rx_tail = (rx_tail + 1) % RX_BUF_SIZE;

        // Procesar comando localmente (Heartbeat, Debug, F<idx>)
        processByte(c);

        // Reenviar al Mega si hay espacio en buffer TX (no bloqueante)
        if (Serial1.availableForWrite() > 0) {
            Serial1.write(c);
        }
        // Si no hay espacio, el byte se pierde (mejor que bloquear todo el sistema)
    }

    // ── Paso 3: reenviar respuestas del Mega al QRB2210 ─────────────
    // El Mega puede enviar datos de vuelta (estado, debug).
    // Los reenviamos al QRB2210 por USB.
    while (Serial1.available() > 0) {
        const char c = static_cast<char>(Serial1.read());
        Serial.write(c);
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  LED MATRIX — FUNCIONES DE DIBUJO
// ══════════════════════════════════════════════════════════════════════════

// ─── Smiley 🙂 — 20 puntos ──────────────────────────────────────────────
// Cara sonriente con ojos, mejillas y boca curva.
// Soporta animaciones vía blink_state (oculta ojos) y pulse_state
// (atenúa mejillas+pupilas para efecto de latido).
static void drawSmiley(void) {
    matrix.beginDraw();
    matrix.clear();
    matrix.stroke(255, 255, 255);

    // Boca sonrisa (curva de 7 puntos)
    matrix.point(2, 5); matrix.point(3, 6); matrix.point(4, 7);
    matrix.point(5, 7); matrix.point(6, 7); matrix.point(7, 6);
    matrix.point(8, 5);

    // Ojos (arco superior) — se ocultan durante parpadeo
    if (!blink_state) {
        matrix.point(2, 1); matrix.point(3, 0); matrix.point(4, 0);
        matrix.point(5, 0); matrix.point(6, 0); matrix.point(7, 0);
        matrix.point(8, 1);
    }

    // Mejillas + pupilas — se atenúan durante pulso de heartbeat
    if (!pulse_state) {
        matrix.point(1, 3); matrix.point(1, 4);
        matrix.point(9, 3); matrix.point(9, 4);
        matrix.point(4, 3); matrix.point(6, 3);
    }

    matrix.endDraw();
}

// ─── HappyFace 😀 — 22 puntos ──────────────────────────────────────────
// Sonrisa grande con boca abierta (dientes) y cejas alegres.
static void drawHappyFace(void) {
    matrix.beginDraw();
    matrix.clear();
    matrix.stroke(255, 255, 255);

    // Boca abierta (arco de 7 puntos + dientes superiores)
    matrix.point(2, 5); matrix.point(3, 6); matrix.point(4, 7);
    matrix.point(5, 7); matrix.point(6, 7); matrix.point(7, 6);
    matrix.point(8, 5);
    matrix.point(3, 5); matrix.point(4, 5); matrix.point(5, 5);
    matrix.point(6, 5); matrix.point(7, 5);

    // Cejas alegres (arqueadas hacia arriba)
    matrix.point(2, 0); matrix.point(3, 0); matrix.point(4, 1);
    matrix.point(6, 1); matrix.point(7, 0); matrix.point(8, 0);

    // Ojos
    matrix.point(4, 2); matrix.point(6, 2);

    // Mejillas
    matrix.point(1, 4); matrix.point(9, 4);

    matrix.endDraw();
}

// ─── SadFace 😟 — 17 puntos ────────────────────────────────────────────
// Cara triste: cejas inclinadas, boca invertida (frown) y lágrimas.
static void drawSadFace(void) {
    matrix.beginDraw();
    matrix.clear();
    matrix.stroke(255, 255, 255);

    // Boca triste (arco ∩ invertido — 7 puntos)
    matrix.point(2, 6); matrix.point(3, 5); matrix.point(4, 4);
    matrix.point(5, 4); matrix.point(6, 4); matrix.point(7, 5);
    matrix.point(8, 6);

    // Cejas inclinadas hacia abajo (6 puntos)
    matrix.point(1, 0); matrix.point(2, 1); matrix.point(3, 2);
    matrix.point(7, 2); matrix.point(8, 1); matrix.point(9, 0);

    // Ojos pequeños
    matrix.point(4, 3); matrix.point(6, 3);

    // Lágrimas
    matrix.point(3, 6); matrix.point(7, 6);

    matrix.endDraw();
}

// ─── WinkFace 😉 — 16 puntos ─────────────────────────────────────────
// Guiño: ojo izquierdo abierto (arco), derecho cerrado (línea), sonrisa.
static void drawWinkFace(void) {
    matrix.beginDraw();
    matrix.clear();
    matrix.stroke(255, 255, 255);

    // Sonrisa (7 puntos)
    matrix.point(2, 5); matrix.point(3, 6); matrix.point(4, 7);
    matrix.point(5, 7); matrix.point(6, 7); matrix.point(7, 6);
    matrix.point(8, 5);

    // Ojo izquierdo abierto (arco)
    matrix.point(3, 1); matrix.point(4, 0); matrix.point(5, 1);

    // Ojo derecho cerrado (línea horizontal)
    matrix.point(7, 1); matrix.point(8, 1); matrix.point(9, 1);

    // Ceja derecha levantada
    matrix.point(8, 0);

    // Mejilla
    matrix.point(9, 3); matrix.point(9, 4);

    matrix.endDraw();
}

// ─── Número + Dots de Dedos 🔢 — 4-28 pts según estado ────────────────
// Muestra el número de dedos levantados (Font_5x7, centrado filas 0-4)
// y 5 dots uno por dedo en filas 6-7:
//   ● (2×2) = dedo abierto,  ○ (2 pts diagonal) = dedo cerrado
//   Columnas: 1=pulgar, 3=índice, 5=corazón, 7=anular, 9=meñique
static void drawNumberWithFingers(int count) {
    matrix.beginDraw();
    matrix.clear();
    matrix.stroke(255, 255, 255);

    // Número grande centrado (Font_5x7)
    matrix.textFont(Font_5x7);
    char s[2] = {'0' + count, '\0'};
    matrix.text(s, 3, 0);

    // 5 dots de dedos en filas 6-7 (excluye muñeca índice 5)
    for (int i = 0; i < NUM_SERVOS - 1; i++) {
        const int x = 1 + i * 2;  // 1, 3, 5, 7, 9
        const bool isOpen = (i == 0)
            ? (servo_pwm[i] < THUMB_OPEN_PWM)
            : (servo_pwm[i] < FINGER_OPEN_PWM);

        if (isOpen) {
            // Dot brillante (abierto) — cuadro 2×2
            matrix.point(x, 6); matrix.point(x + 1, 6);
            matrix.point(x, 7); matrix.point(x + 1, 7);
        } else {
            // Dot tenue (cerrado) — 2 pts en diagonal
            matrix.point(x, 6);
            matrix.point(x + 1, 7);
        }
    }

    matrix.endDraw();
}

// ─── Carácter alfanumérico (0-9) ─────────────────────────────────────────
// Dibuja un dígito usando Font_5x7 (ArduinoGraphics).
// Centrado aproximadamente en la matriz de 12×8.
static void drawChar(const char ch) {
    matrix.beginDraw();
    matrix.clear();
    matrix.stroke(255, 255, 255);
    matrix.textFont(Font_5x7);
    const char text[2] = {ch, '\0'};
    matrix.text(text, 3, 0);
    matrix.endDraw();
}

// ─── Recording Icon 🔴 — 12/4 pts ──────────────────────────────────────
// Círculo rojo pulsante: alterna entre grande (12 pts) y pequeño (4 pts).
static void drawRecordingIcon(bool large) {
    matrix.beginDraw();
    matrix.clear();
    matrix.stroke(255, 255, 255);

    if (large) {
        // Círculo grande ~3×3 (12 puntos)
        matrix.point(4, 2); matrix.point(5, 2); matrix.point(6, 2); matrix.point(7, 2);
        matrix.point(3, 3); matrix.point(8, 3);
        matrix.point(3, 4); matrix.point(8, 4);
        matrix.point(4, 5); matrix.point(5, 5); matrix.point(6, 5); matrix.point(7, 5);
    } else {
        // Círculo pequeño 2×2 (4 puntos)
        matrix.point(5, 3); matrix.point(6, 3);
        matrix.point(5, 4); matrix.point(6, 4);
    }

    matrix.endDraw();
}

// ─── Playing Icon ▶ — 7 pts ──────────────────────────────────────────
// Triángulo de play apuntando a la derecha (4×4).
static void drawPlayingIcon(void) {
    matrix.beginDraw();
    matrix.clear();
    matrix.stroke(255, 255, 255);

    // Triángulo derecho: |▷
    matrix.point(4, 2); matrix.point(4, 3); matrix.point(4, 4); matrix.point(4, 5);
    matrix.point(5, 3); matrix.point(5, 4);
    matrix.point(6, 4);

    matrix.endDraw();
}

// ─── Error X ❌ — 16 pts ──────────────────────────────────────────────
// X grande que diagonaliza toda la matriz. Reemplaza la antigua "E".
static void drawErrorX(void) {
    matrix.beginDraw();
    matrix.clear();
    matrix.stroke(255, 255, 255);

    // Diagonal \ (arriba-izquierda → abajo-derecha)
    matrix.point(2, 0); matrix.point(3, 1); matrix.point(4, 2); matrix.point(5, 3);
    matrix.point(6, 4); matrix.point(7, 5); matrix.point(8, 6); matrix.point(9, 7);

    // Diagonal / (arriba-derecha → abajo-izquierda)
    matrix.point(9, 0); matrix.point(8, 1); matrix.point(7, 2); matrix.point(6, 3);
    matrix.point(5, 4); matrix.point(4, 5); matrix.point(3, 6); matrix.point(2, 7);

    matrix.endDraw();
}

// ─── Loading ⋯ — 1 pt por paso ──────────────────────────────────────
// Animación de carga: 3 puntos en fila 4 (columnas 3,6,9) que se
// encienden secuencialmente de izquierda a derecha.
// Llamar repetidamente con step++ para animar.
static void drawLoading(int step) {
    matrix.beginDraw();
    matrix.clear();
    matrix.stroke(255, 255, 255);

    static const int dots_x[3] = {3, 6, 9};
    matrix.point(dots_x[step % 3], 4);

    matrix.endDraw();
}

// ══════════════════════════════════════════════════════════════════════════
//  LÓGICA DE NEGOCIO — CONTEO DE DEDOS
// ══════════════════════════════════════════════════════════════════════════

// ─── Cuenta cuántos dedos están levantados (abiertos) ────────────────────
// Usa los PWM actuales de cada servo y los umbrales definidos:
//   - Pulgar (índice 0): THUMB_OPEN_PWM (1200 µs) — umbral más amplio
//   - Índice, Medio, Anular, Meñique: FINGER_OPEN_PWM (1150 µs)
//
// Un PWM menor al umbral significa que el servo está más cerca de la
// posición de abierto (1000 µs = extendido). PWM mayor significa
// que el dedo está flexionado (2000 µs = cerrado).
//
// NOTA: El servo 5 (muñeca) no se cuenta como dedo.
//
// Returns: número de dedos levantados (0-5)
static int countFingersUp(void) {
    int count = 0;

    // Pulgar (índice 0): umbral diferente por su rango de movimiento distinto
    if (servo_pwm[0] < THUMB_OPEN_PWM) {
        count++;
    }

    // Resto de dedos (índices 1-4, excluye muñeca índice 5)
    for (int i = 1; i < NUM_SERVOS - 1; i++) {
        if (servo_pwm[i] < FINGER_OPEN_PWM) {
            count++;
        }
    }

    return count;
}

// ══════════════════════════════════════════════════════════════════════════
//  LED MATRIX — ANIMACIONES
// ══════════════════════════════════════════════════════════════════════════

// ─── Actualiza el estado de parpadeo de ojos (cada 3s, 150ms cerrados) ──
static void updateEyeBlink(unsigned long now) {
    if (now - last_blink_ms >= EYE_BLINK_INTERVAL_MS) {
        blink_state = true;
        last_blink_ms = now;
    }
    if (blink_state && (now - last_blink_ms >= EYE_BLINK_DURATION_MS)) {
        blink_state = false;
    }
}

// ─── Actualiza el pulso de heartbeat (cada 1s = 500ms cada fase) ────────
// Solo se activa si hay heartbeat recibido recientemente.
static void updateHeartbeatPulse(unsigned long now) {
    const bool heartbeat_active =
        heartbeat_seen &&
        (now - last_heartbeat_time < HEARTBEAT_TIMEOUT_MS);
    if (heartbeat_active && (now - last_pulse_ms >= PULSE_INTERVAL_MS)) {
        pulse_state = !pulse_state;
        last_pulse_ms = now;
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  LED MATRIX — ACTUALIZACIÓN DEL DISPLAY
// ══════════════════════════════════════════════════════════════════════════

// ─── Determina qué mostrar en la LED Matrix ──────────────────────────────
// Evalúa el estado del sistema y decide el display adecuado:
//
//   Sin heartbeat por >2s      → ❌ Error X parpadeante (500ms on/off)
//   Sin comandos por >3s (idle) → 🙂 Smiley (parpadeo ojos + pulso latido)
//   Conexión activa, 0 dedos    → 🙂 Smiley (parpadeo ojos + pulso latido)
//   Conexión activa, 1-5 dedos  → 🔢 Número grande + ● dots de dedos
//
// Anti-parpadeo: 400ms de histéresis para evitar flicker entre estados.
// Animaciones: parpadeo ojos (3s), pulso heartbeat (1s), error blink (500ms).
static void updateDisplay(void) {
    const unsigned long now = millis();

    // ── Timeouts ────────────────────────────────────────────────────
    const bool heartbeat_timeout =
        heartbeat_seen && (now - last_heartbeat_time > HEARTBEAT_TIMEOUT_MS);
    const bool cmd_timeout = (now - last_cmd_time > IDLE_TIMEOUT_MS);

    // ═════════════════════════════════════════════════════════════════
    //  ESTADO 1: ERROR — sin heartbeat > 2s → ❌ X parpadeante
    // ═════════════════════════════════════════════════════════════════
    if (heartbeat_timeout) {
        // Actualizar blink de error (500ms on/off)
        if (now - last_error_blink_ms > ERROR_BLINK_MS) {
            last_error_blink_ms = now;
            error_blink_state = !error_blink_state;
        }
        // Forzar cambio inmediato (sin histéresis para error)
        if (current_display != -2) {
            current_display = -2;
            last_pose_change = now;
        }
        // Dibujar: X o vacío según estado
        if (error_blink_state) {
            drawErrorX();
        } else {
            matrix.beginDraw(); matrix.clear(); matrix.endDraw();
        }
        return;  // ← Salir: el error tiene prioridad absoluta
    }

    // ═════════════════════════════════════════════════════════════════
    //  ESTADOS 2-3: Determinar display objetivo
    // ═════════════════════════════════════════════════════════════════
    int target_display;

    if (cmd_timeout) {
        target_display = -1;   // Modo idle → smiley
    } else {
        // Conexión activa → contar dedos en tiempo real
        const int fingers = countFingersUp();
        target_display = (fingers == 0) ? 0 : fingers;  // 0=smiley, 1-5=número
    }

    // ── Anti-parpadeo: histéresis de 400ms ──────────────────────────
    if (target_display != current_display &&
        (now - last_pose_change < POSE_HOLD_MS)) {
        return;   // Mantener display actual, no cambiar aún
    }

    // ── Aplicar cambio de display ───────────────────────────────────
    if (target_display != current_display) {
        last_pose_change = now;
        current_display = target_display;
        display_dirty = true;
        // Resetear TODAS las animaciones al cambiar de pantalla
        blink_state = false;
        pulse_state = false;
        last_blink_ms = now;
        last_pulse_ms = now;
        error_blink_state = false;
        recording_pulse_state = false;
    }

    // ═════════════════════════════════════════════════════════════════
    //  DIBUJAR SEGÚN DISPLAY ACTUAL
    // ═════════════════════════════════════════════════════════════════

    if (current_display == -1 || current_display == 0) {
        // ── 🙂 Smiley con animaciones ────────────────────────────────
        // Guardar estado anterior de animaciones para detectar cambios
        bool prev_blink = blink_state;
        bool prev_pulse = pulse_state;

        // 1. Parpadeo de ojos (cada 3s, duran 150ms cerrados)
        updateEyeBlink(now);
        // 2. Pulso de heartbeat (1s período = 500ms cada fase)
        updateHeartbeatPulse(now);
        // 3. Solo redibujar si cambió algo o está dirty
        if (display_dirty || blink_state != prev_blink || pulse_state != prev_pulse) {
            drawSmiley();
            display_dirty = false;
        }

    } else if (current_display >= 1 && current_display <= 5) {
        // ── 🔢 Número de dedos + dots de estado ──────────────────────
        drawNumberWithFingers(current_display);
        // No redibujar hasta que cambie current_display
        display_dirty = false;
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  SETUP — INICIALIZACIÓN DEL SISTEMA
// ══════════════════════════════════════════════════════════════════════════

void setup() {
    // ── Inicializar puertos seriales ─────────────────────────────────
    // Serial:   USB CDC ACM (comunicación con QRB2210 vía SOCAT)
    // Serial1:  USART1 D0/D1 (comunicación con Mega 2560 Serial1)
    Serial.begin(SERIAL_BAUD);
    Serial1.begin(MEGA_BAUD);

    // Esperar hasta 3 segundos a que el USB CDC ACM se enumere.
    // Si no hay host USB conectado, continuamos igual (modo autónomo).
    // La espera evita perder los primeros comandos si Flask ya envió datos
    // antes de que el STM32 termine de iniciarse.
    const unsigned long usb_deadline = millis() + 3000;
    while (!Serial && millis() < usb_deadline) {
        // Espera activa pero breve
    }

    // ── Inicializar LED Matrix (12×8) ────────────────────────────────
    matrix.begin();
    drawSmiley();
    current_display = 0;

    // ── Inicializar temporizadores ───────────────────────────────────
    last_cmd_time       = millis();
    last_heartbeat_time = millis();
    last_pose_change    = millis();

    // ── Mensaje de bienvenida por USB ────────────────────────────────
    // Solo visible si hay un monitor serial conectado.
    Serial.println("STM32 BRIDGE V2: READY");
    Serial.print("Protocolo: F<idx> <pwm_us>\\n  |  ");
    Serial.print("Heartbeat: H\\n -> OK  |  ");
    Serial.println("Debug: D\\n -> toggle");

    // Nota: debug_mode arranca en false.
    // Enviar D\n por Serial para activar modo debug.
}

// ══════════════════════════════════════════════════════════════════════════
//  LOOP PRINCIPAL — CICLO DE VIDA DEL BRIDGE
// ══════════════════════════════════════════════════════════════════════════

void loop() {
    // ── 1. Procesar comunicación serial ──────────────────────────────
    // Lee del USB (QRB2210), parsea, reenvía al Mega, y viceversa.
    // Esta función es bloqueante breve: procesa todos los bytes disponibles
    // y retorna inmediatamente. No espera por datos nuevos.
    processSerialInput();

    // ── 2. Actualizar LED Matrix ─────────────────────────────────────
    // Evalúa el estado (timeouts, conteo de dedos, heartbeat) y actualiza
    // el display con anti-parpadeo.
    updateDisplay();

    // ── 3. Pequeña pausa para estabilidad ────────────────────────────
    // Previene saturación del CPU en caso de que no haya datos seriales.
    // 5ms es suficiente para darles tiempo a los periféricos sin afectar
    // la latencia de respuesta (< 1 frame a 30fps).
    delay(5);
}
