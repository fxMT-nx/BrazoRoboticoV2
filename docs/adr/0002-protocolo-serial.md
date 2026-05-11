# ADR-0002: Protocolo Serial basado en PWM

**Fecha:** 2026-05-10
**Última actualización:** 2026-05-11 (6 servos, añadida muñeca)
**Estado:** Aceptado
**Decidido por:** Equipo de desarrollo
**Revisión programada:** 2026-11-10

## Contexto

V1 utilizaba un protocolo serial con formato `M<idx> <angle>\n` donde el significado de los parámetros era ambiguo:

- `M` no descriptivo: ¿Move? ¿Motor? ¿Manus? ¿Manuscrito?
- `<angle>` en unidades no especificadas: ¿grados? ¿radianes? ¿pasos?
- Sin heartbeat ni watchdog: si el servidor caía, los servos quedaban en la última posición.
- Sin safe pose: no había mecanismo de seguridad ante timeouts o errores.

En V2, la comunicación es:

```
Flask (QRB2210) → TCP:7500 → SOCAT (systemd) → /dev/ttyGS0 → USB Gadget
    → STM32 (bridge serial) → Serial1(D0/D1) → Mega Serial1 → PWM → Servos
```

Se necesita un protocolo:
1. **Sin ambigüedad de unidades**: PWM en microsegundos es el estándar de la industria para servos.
2. **Auto-documentado**: El prefijo debe indicar claramente la función.
3. **Con heartbeat y watchdog**: Para detección de fallos y safe pose automática.
4. **Simple**: Un comando por línea, sin framing complejo.

## Decisión

Adoptar el protocolo `F<idx> <pwm_us>\n` con heartbeat `H\n` y safe pose por timeout.

### Formato de comandos

| Comando | Formato | Ejemplo | Descripción |
|---------|---------|---------|-------------|
| Finger | `F<idx> <pwm_us>\n` | `F0 1500\n` | Posiciona servo `<idx>` (0-5) a `<pwm_us>` µs |
| Heartbeat | `H\n` | `H\n` | Mantiene vivo el watchdog. Enviado cada 500ms. |

**Índices de servo (0–5):**
- `0` = Pulgar (Thumb)
- `1` = Índice (Index)
- `2` = Medio (Middle)
- `3` = Anular (Ring)
- `4` = Meñique (Pinky)
- `5` = Muñeca (Wrist)

El servo de muñeca (índice 5) usa el **mismo formato** `F<idx> <pwm_us>\n`.
SerialManager valida con `VALID_IDX_RANGE = range(0, 6)`.

### Rango PWM

- **Estándar MG996R**: 500 µs (0°) — 1500 µs (90°) — 2500 µs (180°)
- **Rango seguro configurado**: 800 µs — 2200 µs (límites por YAML)
- **Safe pose**: 1500 µs (posición central, todos los servos)

### Heartbeat y Watchdog

- Frecuencia heartbeat: 500 ms
- Timeout watchdog: 2000 ms (configurable en `servo_calibration.yaml: safe_pose_fallback_ms`)
- Al alcanzar el timeout → Mega lleva todos los servos a **Safe Pose** (1500 µs)
- El timeout se resetea con cada comando `F` o `H` recibido

### Interpolación en el Mega

El Mega implementa una **aceleración trapezoidal** (perfil de velocidad con aceleración, crucero y desaceleración) desde el PWM actual hacia el PWM target. Esto evita movimientos bruscos que podrían dañar los servos o la estructura mecánica.

## Alternativas consideradas

| Alternativa | Descartada por |
|-------------|---------------|
| `M<idx> <angle>\n` (V1) | Unidades ambiguas, sin heartbeat, sin safe pose. |
| JSON sobre serial | Sobrecarga de parsing en el Mega (memoria limitada). |
| Protocolo binario | Mayor complejidad de debugging. `F<idx> <pwm_us>\n` se depura con `screen` o `cat`. |
| `S<idx> <us>\n` | `S` podría confundirse con "Stop" o "Status". `F` de "Finger" es descriptivo. |

## Consecuencias

**Positivas:**
- Unidades inequívocas: PWM en µs es el estándar en servos RC.
- `F` de "Finger" es descriptivo y auto-documentado (aunque idx 5 es muñeca, se mantiene la `F` por consistencia).
- Heartbeat + timeout garantiza safe pose automática si el servidor se cuelga.
- Formato texto plano: debugging simple con `screen /dev/ttyACM0 115200`.
- Interpolación trapezoidal en Mega protege los servos de cambios bruscos.

**Negativas / trade-offs:**
- Incompatible con V1: requiere actualizar firmware del Mega y código del backend.
- El render de rampa suave en Mega añade latencia (~20-50ms adicionales).
- Ancho de banda serial ligeramente mayor que protocolo binario (irrelevante a 115200 baud).

## Términos de dominio afectados

- **Command** — ver `CONTEXT.md`
- **Heartbeat** — ver `CONTEXT.md`
- **Safe Pose** — ver `CONTEXT.md`
- **Interpolation** — ver `CONTEXT.md`
- **Wrist** — ver `CONTEXT.md`

## Señales de revisión

- Si en el futuro se añaden sensores (feedback de posición, fuerza) que requieran comunicación bidireccional, el protocolo debería extenderse (ej: `S\n` para solicitar estado).
- Si la latencia de interpolación en Mega resulta problemática, considerar mover la interpolación al backend.
