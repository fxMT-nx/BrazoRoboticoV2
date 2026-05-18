# 🦾 BrazoRoboticoV2 — Memoria del Proyecto

> **Última actualización:** 19 de mayo de 2026  
> **Versión:** 2.3 (Beta)  
> **Estado:** ✅ COMPLETAMENTE OPERATIVO

---

## 1. Resumen Ejecutivo

Brazo robótico articulado de **6 GDL** (5 dedos + muñeca) controlado por visión artificial en tiempo real con MediaPipe.js. Un **Arduino UNO Q** (Qualcomm QRB2210) corre Flask con el **PoseMapper**, que convierte landmarks 3D a PWM. Los comandos viajan por **USB OTG** a un **Arduino Mega 2560** que mueve 6 servos **MG996R**.

**Demo en vivo:** [https://brazo.nxserve.org](https://brazo.nxserve.org) (Cloudflare Tunnel)

## 2. Stack Tecnológico

| Capa | Tecnología |
|------|-----------|
| **Frontend** | React 18 + TypeScript + Vite + Three.js + Recharts |
| **Tracking** | MediaPipe.js HandLandmarker (21 landmarks 3D) |
| **Backend** | Python 3.13 + Flask 3.1 + Flask-Sock |
| **Bridge** | SOCAT (TCP:7500 ↔ /dev/ttyACM0) |
| **Firmware Mega** | Arduino C++ (Servo.h, watchdog, trapezoidal) |
| **Firmware STM32** | Arduino C++ (LED Matrix, legacy) |
| **Infra** | Cloudflare Tunnel, systemd, Debian Linux |
| **Acceso** | `https://brazo.nxserve.org` (Cloudflare Tunnel) |

## 3. Arquitectura de Comunicación

```
👋 Mano → 🎥 Cámara → 🧠 MediaPipe.js → 🌐 WebSocket
  → 🐍 Flask (UNO Q) → 🔗 SOCAT → 🔌 USB OTG
  → 🔌 Mega 2560 → ⚙️ Servos MG996R ×6
```

**Protocolo serie:** `F<idx> <pwm_us>\n` @ 115200 baud  
**Heartbeat:** `H\n` cada 500ms (watchdog Mega: 2500ms)  
**Safe pose:** Al detener/desconectar, cada servo va a su `open_pwm` calibrado

## 4. Pinout de Servos

| Servo | Dedo | Pin Mega | Abierto 🖐️ | Cerrado ✊ |
|:-----:|------|:--------:|:----------:|:----------:|
| 0 | 👍 Pulgar | **D7** | 2000 µs | 800 µs |
| 1 | ☝️ Índice | **D6** | 2000 µs | 600 µs |
| 2 | 🖕 Corazón | **D5** | 1700 µs | 600 µs |
| 3 | 💍 Anular | **D4** | 1900 µs | 600 µs |
| 4 | 🤙 Meñique | **D3** | 1900 µs | 600 µs |
| 5 | ↕️ Muñeca | **D2** | 2000 µs | 600 µs |

> ⚠️ Orden en firmware: `SERVO_PINS[] = {7, 6, 5, 4, 3, 2}` (inverso a la lectura intuitiva)

## 5. Alimentación

| Línea | Tensión | Corriente |
|-------|:-------:|:---------:|
| Servos + Mega | **5 V** | **10 A+** |
| UNO Q (VIN) | 6.5–12 V | 2 A |
| GND | Común entre ambas fuentes | — |

## 6. Historial de Versiones

| Versión | Fecha | Cambios |
|:-------:|:-----:|---------|
| **2.3** | 19/05/26 | Auditoría y limpieza del repo. Safe pose inteligente al detener. Servicios systemd corregidos. Documentación sincronizada. Eliminados 18 sketches huérfanos y 36MB de binarios. |
| **2.3** | 18/05/26 | Migración a USB OTG. Rescate del UNO Q via EDL. Calibración física de cada servo. Muñeca rotacional por profundidad Z. Cloudflare Tunnel operativo. |
| **2.2** | 13/05/26 | Dominio permanente `brazo.nxserve.org`. Cloudflare Tunnel. Eliminado DuckDNS. |
| **2.1** | 12/05/26 | Cloudflare Tunnel + Quick Tunnel. LED Matrix con dominio. |
| **2.0** | 10/05/26 | Sexto servo de muñeca. Green dot. Handedness. Interpolación suave. PoseMapper. |
| **1.0** | 04/06/26 | Primer prototipo: 5 dedos, tracking básico. |

## 7. Problemas Conocidos

1. **USB OTG post-reinicio:** El `arduino-router` y SOCAT de fábrica pueden recuperar el puerto 7500. El servicio `brazo-init.service` los mata al arrancar.
2. **Safe pose del watchdog:** Si el watchdog del Mega salta, va a 1300 µs (centro de muñeca). Los dedos quedan a media apertura.
3. **ACKs perdidos:** El Mega responde al heartbeat por Serial1 (no por USB). El backend no recibe confirmación, pero no la necesita.
4. **mDNS lento:** `brazorobotico.local` puede tardar hasta 30s en resolverse tras un arranque.

## 8. URLs de Acceso

| Método | URL | Puerto |
|--------|-----|:------:|
| 🌍 Web | `https://brazo.nxserve.org` | 443 → 3000 |
| 🏠 Local | `http://brazorobotico.local:3000` | 3000 |
| 🔐 SSH | `ssh arduino@brazorobotico.local` | 22 |
| 🩺 Health | `https://brazo.nxserve.org/api/health` | — |

## 9. Estructura del Repositorio

```
BrazoRoboticoV2/
├── backend/
│   ├── server.py              # Flask + WebSocket
│   ├── pose_mapper.py         # ★ Core: landmarks → PWM
│   ├── serial_manager.py      # Comunicación TCP con SOCAT
│   ├── config/                # YAMLs (calibración, tracking, red)
│   └── static/                # Build frontend
├── frontend/                  # React 18 + TypeScript
├── firmware/
│   ├── mega_servos/           # ★ Firmware activo del Mega
│   └── stm32/                 # STM32 (LED Matrix, legacy)
├── deploy/
│   ├── systemd/               # socat-mega, robot-hand, brazo-init
│   ├── udev/                  # Reglas USB
│   └── requirements.txt       # Dependencias Python
├── docs/
│   ├── CONTEXT.md             # Lenguaje ubicuo
│   ├── memoria-tecnica.html   # Memoria imprimible
│   └── adr/                   # Decisiones arquitectónicas
└── tests/
    └── test_serial_manager.py
```

---

> **Repositorio:** [github.com/fxMT-nx/BrazoRoboticoV2](https://github.com/fxMT-nx/BrazoRoboticoV2)  
> **Demo:** [brazo.nxserve.org](https://brazo.nxserve.org)  
> **Licencia:** MIT
