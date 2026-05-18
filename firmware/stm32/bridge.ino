/*
 * BrazoRoboticoV2 — STM32U585 Bridge Sketch para Arduino UNO Q (ABX00162)
 * ===========================================================================
 *
 * ─── Rol ───────────────────────────────────────────────────────────────────
 * Este sketch corre en el **STM32U585** del Arduino UNO Q. Actúa como bridge
 * serial entre el Qualcomm QRB2210 (Debian Linux, Flask) y el Arduino Mega
 * 2560. También controla la **LED Matrix 12×8** integrada en el UNO Q para
 * mostrar el dominio DuckDNS en scroll horizontal.
 *
 * ─── Arquitectura ──────────────────────────────────────────────────────────
 *   QRB2210 (Flask) → SOCAT → /dev/ttyHS1 → STM32 Serial2 (LPUART1) → Serial1 (USART1 D0/D1)
 *   → Mega 2560 (Serial1) → PWM → Servos MG996R ×6
 *
 *   En detalle:
 *   Flask → TCP:7500 → SOCAT → /dev/ttyHS1 → LPUART1 → STM32 Serial2
 *   → Serial1 (D0/D1, USART1, 115200 baud) → Mega Serial1
 *
 * ─── Mapeo Serial del STM32U585 ────────────────────────────────────────────
 *   Serial  = USB CDC ACM              → Debug por USB (cuando el PC está conectado)
 *   Serial1 = USART1 (D0/D1)           → Comunicación con Arduino Mega 2560
 *   Serial2 = LPUART1 = ttyHS1         → Comunicación con Qualcomm QRB2210 (Flask vía SOCAT)
 *
 * ─── Protocolo V2 ──────────────────────────────────────────────────────────
 *   F<idx> <pwm_us>\n  — Command: mueve el servo idx al PWM especificado
 *                         idx: 0-5 (thumb, index, middle, ring, pinky, wrist)
 *                         pwm_us: 800-2200 µs
 *   H\n                 — Heartbeat: keep-alive desde Flask
 *   I<texto>\n          — IP/Domain: establece el texto a mostrar en la
 *                         LED Matrix (scroll horizontal)
 *   OK\n                — ACK: respuesta a Heartbeat
 *   D\n                 — Debug: toggle modo debug (muestra raw por USB)
 *
 * ─── LED Matrix (12×8 LEDs rojos) ──────────────────────────────────────────
 *   Muestra el dominio DuckDNS en scroll horizontal continuo.
 *   El texto se recibe desde Flask mediante el comando I<texto>\n.
 *   Por defecto muestra "manomagica.duckdns.org".
 *
 * ─── Mejoras sobre V1 ──────────────────────────────────────────────────────
 *   1. ✅ Buffer circular para recepción serial (ISR-ready)
 *   2. ✅ Scroll horizontal del dominio DuckDNS
 *   3. ✅ Comando I<texto>\n para actualizar el dominio desde Flask
 *   4. ✅ Modo debug: D\n toggle, muestra raw de comandos por USB
 *   5. ✅ Sin malloc, buffer estático, volatile para contexto ISR
 *
 * ===========================================================================
 * Autor:    BrazoRoboticoV2 Team
 * Licencia: MIT
 * ===========================================================================
 */

#include <ArduinoGraphics.h>
#include <Arduino_LED_Matrix.h>
#include <Wire.h>

#define MEGA_I2C_ADDR 0x08

// ─── Instancia global de la LED Matrix ────────────────────────────────────
Arduino_LED_Matrix matrix;

// ══════════════════════════════════════════════════════════════════════════
//  CONSTANTES
// ══════════════════════════════════════════════════════════════════════════

// ── Velocidad de los puertos seriales ─────────────────────────────────────
static const unsigned long MEGA_BAUD    = 115200;
static const unsigned long SERIAL_BAUD  = 115200;

// ── Configuración de la mano robótica ─────────────────────────────────────
static const int NUM_SERVOS = 6;        // thumb, index, middle, ring, pinky, wrist
static const int PWM_MIN    = 800;      // Límite seguro inferior
static const int PWM_MAX    = 2200;     // Límite seguro superior
static const int PWM_CENTER = 1500;     // Safe pose / posición central

// ── Temporizadores ────────────────────────────────────────────────────────
static const unsigned long HEARTBEAT_TIMEOUT_MS = 2000; // 2s sin heartbeat → sin conexión

// ── LED Matrix — scroll de dominio ────────────────────────────────────────
#define IP_BUF_SIZE 48
#define SCROLL_INTERVAL_MS 350

// ── Buffer circular para recepción serial ────────────────────────────────
static const uint8_t RX_BUF_SIZE = 128;
static volatile char rx_buffer[RX_BUF_SIZE];
static volatile uint8_t rx_head = 0;
static volatile uint8_t rx_tail = 0;

// ══════════════════════════════════════════════════════════════════════════
//  ESTADO GLOBAL
// ══════════════════════════════════════════════════════════════════════════

// ── PWM actual de cada servo (en µs) ─────────────────────────────────────
static int servo_pwm[NUM_SERVOS] = {
    PWM_CENTER, PWM_CENTER, PWM_CENTER, PWM_CENTER, PWM_CENTER, PWM_CENTER
};

// ── Temporización ─────────────────────────────────────────────────────────
static unsigned long last_cmd_time       = 0;
static unsigned long last_heartbeat_time = 0;

// ── Heartbeat ─────────────────────────────────────────────────────────────
static bool heartbeat_seen = false;

// ── Debug ─────────────────────────────────────────────────────────────────
static bool debug_mode = false;

// ── LED Matrix — scroll de dominio ────────────────────────────────────────
static char domain_display[IP_BUF_SIZE] = "manomagica.duckdns.org  ";
static int scroll_pos = 0;
static unsigned long last_scroll_ms = 0;

// ── Parser de comando I<texto>\n ─────────────────────────────────────────
static bool ip_recv_mode = false;
static uint8_t ip_recv_pos = 0;

// ══════════════════════════════════════════════════════════════════════════
//  PARSER DE COMANDOS (MÁQUINA DE ESTADOS)
// ══════════════════════════════════════════════════════════════════════════

enum ParserState : uint8_t {
    PARSER_WAITING_F,
    PARSER_READY_IDX,
    PARSER_READY_PWM,
};

static ParserState parser_state = PARSER_WAITING_F;
static char parse_buffer[8];
static uint8_t parse_pos = 0;
static int parse_idx = 0;

// ─── Dibuja 2 caracteres del dominio en la matriz ──────────────────────
static void drawDomain(void) {
    matrix.beginDraw();
    matrix.clear();
    matrix.stroke(255, 255, 255);
    matrix.textFont(Font_5x7);
    char buf[3] = {
        domain_display[scroll_pos],
        domain_display[scroll_pos + 1],
        '\0'
    };
    matrix.text(buf, 1, 0);
    matrix.endDraw();
}

// ─── Actualiza scroll (llamar en cada loop) ────────────────────────────
static void updateScroll(void) {
    unsigned long now = millis();
    if (now - last_scroll_ms >= SCROLL_INTERVAL_MS) {
        scroll_pos++;
        if (scroll_pos >= (int)strlen(domain_display) - 1) {
            scroll_pos = 0;
        }
        last_scroll_ms = now;
        drawDomain();
    }
}

// ─── Recibir nuevo dominio desde Flask (comando I<texto>\n) ───────────
// Se llama desde processByte para cada byte mientras ip_recv_mode está activo.
// El caracter 'I' inicia el modo, y '\n' lo finaliza.
static void processIPCommand(const char c) {
    if (c == 'I') {
        // Iniciar modo de captura de dominio
        ip_recv_mode = true;
        ip_recv_pos = 0;
        memset(domain_display, 0, IP_BUF_SIZE);
        return;
    }

    if (c == '\n') {
        // Finalizar: añadir espacios para scroll suave
        domain_display[ip_recv_pos] = ' ';
        domain_display[ip_recv_pos + 1] = ' ';
        domain_display[ip_recv_pos + 2] = '\0';
        ip_recv_mode = false;
        scroll_pos = 0;
        drawDomain();
        if (debug_mode) {
            Serial.print("DOMAIN: ");
            Serial.println(domain_display);
        }
        return;
    }

    // Acumular caracteres
    if (ip_recv_pos < IP_BUF_SIZE - 3) {
        domain_display[ip_recv_pos++] = c;
    }
}

// ─── Procesa un byte individual del protocolo V2 ─────────────────────────
static void processByte(const char c) {
    // ── Modo debug: mostrar byte raw ──────────────────────────────
    if (debug_mode && c != '\n') {
        Serial.print("RAW: 0x");
        Serial.print(static_cast<uint8_t>(c), HEX);
        Serial.print(" '");
        if (c >= 32 && c < 127) {
            Serial.write(c);
        }
        Serial.println("'");
    }

    // ── Comando de dominio: I<texto>\n ──────────────────────────
    // Si recibimos 'I' o estamos en modo de captura, desviamos a processIPCommand
    if (c == 'I' || ip_recv_mode) {
        processIPCommand(c);
        return;
    }

    // ── Comando Heartbeat: H\n ──────────────────────────────────────
    if (c == 'H' && parser_state == PARSER_WAITING_F) {
        heartbeat_seen = true;
        last_heartbeat_time = millis();
        Serial.println("OK");
        return;
    }

    // ── Comando Debug: D\n ──────────────────────────────────────────
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

    if (parser_state == PARSER_READY_IDX) {
        if (c >= '0' && c <= '5') {
            parse_idx = c - '0';
            parser_state = PARSER_READY_PWM;
            parse_pos = 0;
            parse_buffer[0] = '\0';
        } else {
            parser_state = PARSER_WAITING_F;
        }
        return;
    }

    if (parser_state == PARSER_READY_PWM) {
        if (c == ' ') {
            return;
        }
        if (c >= '0' && c <= '9' && parse_pos < sizeof(parse_buffer) - 1) {
            parse_buffer[parse_pos++] = c;
            parse_buffer[parse_pos] = '\0';
            return;
        }
        if (c == '\n') {
            if (parse_pos > 0) {
                const int pwm_raw = atoi(parse_buffer);
                const int pwm = constrain(pwm_raw, PWM_MIN, PWM_MAX);
                servo_pwm[parse_idx] = pwm;
                last_cmd_time = millis();

                // ── Reenviar comando completo por I2C al Mega ──
                // Envía "F<idx> <pwm>\n" como una transacción Wire.
                Wire.beginTransmission(MEGA_I2C_ADDR);
                Wire.write('F');
                Wire.write('0' + parse_idx);
                Wire.write(' ');
                char pwm_str[8];
                itoa(pwm, pwm_str, 10);
                for (char* p = pwm_str; *p; p++) {
                    Wire.write(*p);
                }
                Wire.write('\n');
                Wire.endTransmission();

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
        parser_state = PARSER_WAITING_F;
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  BUFFER CIRCULAR — LECTURA DESDE SERIAL
// ══════════════════════════════════════════════════════════════════════════

static void drainSerialToBuffer(void) {
    while (Serial.available() > 0) {                                        // Serial = USB CDC ACM = ttyGS0
        const uint8_t next = (rx_head + 1) % RX_BUF_SIZE;
        if (next != rx_tail) {
            rx_buffer[rx_head] = static_cast<char>(Serial.read());
            rx_head = next;
        } else {
            rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
            rx_buffer[rx_head] = static_cast<char>(Serial.read());
            rx_head = (rx_head + 1) % RX_BUF_SIZE;
        }
    }
}

static void processSerialInput(void) {
    drainSerialToBuffer();             // drena Serial (USB CDC ACM = ttyGS0 = datos desde Flask/SOCAT)
    while (rx_tail != rx_head) {
        const char c = rx_buffer[rx_tail];
        rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
        processByte(c);
        if (Serial1.availableForWrite() > 0) {
            Serial1.write(c);
        }
    }
    while (Serial1.available() > 0) {
        const char c = static_cast<char>(Serial1.read());
        Serial.write(c);
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  LED MATRIX — ACTUALIZACIÓN DEL DISPLAY
// ══════════════════════════════════════════════════════════════════════════

// Muestra SOLO el dominio DuckDNS en scroll horizontal.
// Sin smileys, sin animaciones, sin conteo de dedos.
static void updateDisplay(void) {
    updateScroll();
}

// ══════════════════════════════════════════════════════════════════════════
//  SETUP — INICIALIZACIÓN DEL SISTEMA
// ══════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(SERIAL_BAUD);          // USB CDC ACM ← datos desde Flask vía SOCAT/ttyGS0
    Serial1.begin(MEGA_BAUD);           // USART1 D0/D1 → Mega 2560

    Wire.begin();                       // I2C Master → Mega Slave (addr 0x08)

    // No bloquear esperando USB CDC ACM — en modo autónomo no hay USB conectado
    // const unsigned long usb_deadline = millis() + 3000;
    // while (!Serial && millis() < usb_deadline) {}

    matrix.begin();
    drawDomain();

    last_cmd_time       = millis();
    last_heartbeat_time = millis();

    Serial.println("STM32 BRIDGE V2: READY");
    Serial.print("Protocolo: F<idx> <pwm_us>\\n  |  ");
    Serial.print("Heartbeat: H\\n -> OK  |  ");
    Serial.print("Domain: I<texto>\\n  |  ");
    Serial.println("Debug: D\\n -> toggle");
}

// ══════════════════════════════════════════════════════════════════════════
//  LOOP PRINCIPAL — CICLO DE VIDA DEL BRIDGE
// ══════════════════════════════════════════════════════════════════════════

void loop() {
    processSerialInput();
    updateDisplay();
    delay(5);
}
