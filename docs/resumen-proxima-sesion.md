# Resumen para próxima sesión — BrazoRoboticoV2

> **Fecha:** Lunes, 18 de mayo de 2026  
> **Versión actual:** Beta v2.3 — "OTG USB + Calibración física"  
> **Estado:** ✅ SISTEMA COMPLETAMENTE OPERATIVO

---

## 1. RESUMEN DE LA SESIÓN

### 🆘 Rescate del UNO Q
El UNO Q estaba **petado** (no arrancaba). Se recuperó vía **modo EDL** (jumper de recovery):
1. Jumper en pines EDL → conectado por USB → detectado como `05c6:9008`
2. Descargado `arduino-flasher-cli` y flasheada imagen Debian de fábrica
3. Configurado WiFi (Fephone), SSH, contraseña sudo

### 🔧 Infraestructura instalada
| Servicio | Descripción |
|----------|-------------|
| `robot-hand.service` | Flask + WebSocket + PoseMapper |
| `socat-mega.service` | SOCAT bridge: TCP:7500 ↔ Mega 2560 por USB OTG |
| `cloudflared-tunnel.service` | Cloudflare Tunnel → `brazo.nxserve.org` |
| `usb-host-mode.service` | Fuerza modo host USB al arranque |

### 📡 Ruta de comunicación definitiva
```
https://brazo.nxserve.org → Cloudflare Tunnel → UNO Q
  → Flask (:3000) → TCP:7500 → SOCAT
  → /dev/ttyACM0 (USB OTG) → Mega 2560
  → PWM → Servos MG996R ×6
```

El STM32 bridge se eliminó de la ruta de comunicación. El Mega se conecta directamente por USB OTG al UNO Q.

### 🔌 Conexiones físicas
- **UNO Q** → Alimentación externa 6.5-12V + USB-C (OTG) al Mega
- **Mega 2560** → Alimentación externa 5V/10A + Sensor Shield V2.0
- **Mega → UNO Q**: Solo USB (con adaptador OTG). No usa UART (D0/D1)

### 🦾 Calibración de servos (por dedo)
| Servo | Abrir (µs) | Cerrar (µs) | Límites |
|:-----:|:----------:|:-----------:|:-------:|
| 0 - Pulgar | 2000 | 800 | 700-2100 |
| 1 - Índice | 2000 | 600 | 500-2100 |
| 2 - Corazón | 1700 | 600 | 500-1800 |
| 3 - Anular | 1900 | 600 | 500-2000 |
| 4 - Meñique | 1900 | 600 | 500-2000 |
| 5 - Muñeca | 2000 | 600 | 700-2000 |

**Safe pose:** 1300 µs (centro de muñeca)

### 🧠 Mejoras en PoseMapper
- **Pulgar**: Sensibilidad aumentada (rango ±0.01 → 0-1)
- **Dedos largos**: Ganancia 1.5x para aprovechar todo el rango PWM
- **Muñeca**: Cambiada de flexión vertical a **rotación** (usa diferencia de profundidad Z entre índice y meñique)

### 📋 Problemas conocidos
1. **Calibración**: Si se cambia la posición de la cámara o la iluminación, reajustar sensibilidad del pulgar en `pose_mapper.py`
2. **Watchdog en Mega**: Se dispara si no hay heartbeats. Normal durante arranque.
3. **USB OTG**: Al reconectar el Mega, el modo host USB puede no activarse. El servicio `usb-host-mode.service` lo fuerza.
4. **Heartbeat timeout**: SerialManager no recibe ACK del Mega (el Mega responde por Serial1, no por USB). No afecta funcionalidad.

### 🔐 Acceso
| Método | URL |
|--------|-----|
| **Dominio** | `https://brazo.nxserve.org` |
| **SSH** | `ssh arduino@10.222.228.203` (pass: `arduino`) |
| **Healthcheck** | `https://brazo.nxserve.org/api/health` |

### 📌 Próximos pasos sugeridos
1. Ajustar calibración fina de cada dedo si es necesario
2. Probar grabación/reproducción (ReplayControls)
3. Considerar migrar a producción con Gunicorn/Waitress
4. Generar certificados SSL para HTTPS local
