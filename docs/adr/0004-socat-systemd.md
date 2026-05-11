# ADR-0004: SOCAT como Servicio systemd

**Fecha:** 2026-05-10
**Estado:** Aceptado
**Decidido por:** Equipo de desarrollo
**Revisión programada:** 2026-11-10

## Contexto

En V1, el bridge TCP ↔ Serial se iniciaba manualmente con:
```bash
sudo socat file:/dev/ttyGS0,raw,echo=0,b115200 tcp:127.0.0.1:7500,reuseaddr &
```

Esto tenía varios problemas:
1. **Inicio manual**: Requería intervención humana en cada arranque del sistema.
2. **Sin supervisión**: Si socat moría, el brazo perdía comunicación y nadie lo reiniciaba.
3. **Sin integración con el ciclo de vida del sistema**: Debía arrancar antes que Flask, pero no había manera de expresar esta dependencia.

En V2, la pila de comunicación es:

```
Flask → TCP:7500 → SOCAT → /dev/ttyGS0 → USB Gadget → STM32 → Mega
```

Además, existe un servicio `arduino-router.service` (parte del BSP del UNO Q) que en sus hooks `ExecStartPre` y `ExecStopPost` puede **resetear el STM32**, causando pérdida de calibración y estados inconsistentes.

## Decisión

Implementar SOCAT como **servicio systemd** (`socat.service`) con las siguientes características:

### 1. Servicio principal (`deploy/systemd/socat.service`)

```ini
[Unit]
Description=SOCAT bridge: TCP:7500 ↔ USB Gadget Serial
After=multi-user.target
Requires=dev-ttyGS0.device

[Service]
Type=simple
ExecStartPre=/bin/sleep 3
ExecStart=/usr/bin/socat file:/dev/ttyGS0,raw,echo=0,b115200,crtscts=0 \
    tcp:127.0.0.1:7500,reuseaddr
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Puntos clave del diseño:
- **`Requires=dev-ttyGS0.device`**: No arranca hasta que el USB Gadget Serial esté disponible.
- **`ExecStartPre=/bin/sleep 3`**: Espera 3 segundos a que el dispositivo se estabilice.
- **`Restart=always`**: Si socat muere por cualquier razón, systemd lo reinicia automáticamente en 5 segundos.
- **`RestartSec=5`**: Pausa entre reintentos para evitar ciclos de reinicio rápidos.

### 2. Dependencia con robot-hand.service

```ini
[Unit]
Description=BrazoRoboticoV2 — Flask + WebSocket
After=network-online.target socat.service
Wants=socat.service
```

- `After=socat.service`: Flask arranca **después** de que socat esté activo.
- `Wants=socat.service`: Si socat no arranca, Flask arranca igual (modo offline degradado).

### 3. Drop-in para arduino-router.service

El archivo `deploy/systemd/arduino-router-dropin.conf` se instala en `/etc/systemd/system/arduino-router.service.d/20-no-reset.conf`:

```ini
[Service]
ExecStopPost=
ExecStartPre=
```

Esto **elimina los hooks** que el BSP del UNO Q ejecuta y que pueden resetear el STM32 durante arranque/apagado.

### 4. Instalación automatizada

El script `deploy/install.sh` maneja todo:
```bash
# Instalar servicios
cp deploy/systemd/socat.service /etc/systemd/system/
cp deploy/systemd/robot-hand.service /etc/systemd/system/

# Instalar drop-in
mkdir -p /etc/systemd/system/arduino-router.service.d/
cp deploy/systemd/arduino-router-dropin.conf \
    /etc/systemd/system/arduino-router.service.d/20-no-reset.conf

# Habilitar
systemctl daemon-reload
systemctl enable socat.service robot-hand.service
```

### 5. Reglas udev complementarias

El archivo `deploy/udev/99-arduino-uno-q.rules` asegura permisos:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="2341", ATTR{idProduct}=="0078", MODE="0666"
SUBSYSTEM=="tty", KERNEL=="ttyGS0", MODE="0666", GROUP="dialout"
```

## Alternativas consideradas

| Alternativa | Descartada por |
|-------------|---------------|
| Inicio manual con `&` (V1) | Sin supervisión, sin dependencias, sin reinicio automático. |
| Python script con `subprocess` | Más frágil que systemd. Reinventar la rueda (systemd ya hace supervisión). |
| Ser2net | Menos estándar que socat. Una dependencia más que mantener. |
| STM32 como único bridge (sin SOCAT) | El QRB2210 necesita el USB Gadget para que Flask acceda al STM32. |
| Socket CAN | Overkill para una comunicación serial simple a 115200 baud. |

## Consecuencias

**Positivas:**
- SOCAT se inicia automáticamente al arrancar el sistema.
- Reinicio automático si el proceso muere (`Restart=always`).
- Dependencia explícita: Flask espera a que SOCAT esté listo.
- Drop-in previene resets del STM32 causados por `arduino-router.service`.
- Reglas udev aseguran permisos sin intervención manual.
- Integración con `install.sh`: despliegue completo con un solo comando.

**Negativas / trade-offs:**
- Complejidad adicional de systemd para un equipo pequeño.
- Dependencia de `dev-ttyGS0.device` — si el gadget USB no se configura, el servicio falla.
- `sleep 3` en `ExecStartPre` es un workaround; idealmente el dispositivo debería estar listo antes.

## Términos de dominio afectados

- **SOCAT** — ver `CONTEXT.md`
- **Bridge** — ver `CONTEXT.md`
- **Qualcomm** — ver `CONTEXT.md`

## Señales de revisión

- Si el USB Gadget Serial cambia de nombre (ej: `ttyGS1`), actualizar el servicio y las reglas udev.
- Si se elimina la necesidad de SOCAT (comunicación directa STM32→QRB2210 por otro bus), este servicio se vuelve obsoleto.
