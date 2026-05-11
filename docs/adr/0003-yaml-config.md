# ADR-0003: Configuración en YAML por Dominio

**Fecha:** 2026-05-10
**Estado:** Aceptado
**Decidido por:** Equipo de desarrollo
**Revisión programada:** 2026-11-10

## Contexto

V1 utilizaba un único archivo de configuración (formato propietario) que mezclaba calibración de servos, parámetros de red y ajustes de tracking en un mismo lugar sin estructura clara.

Para V2 se necesita:
1. **Separación por dominio**: Calibración de servos, parámetros de tracking y configuración de red son conceptos distintos que cambian con frecuencias diferentes.
2. **Comentarios permitidos**: La calibración de servos requiere documentación in-situ (límites mecánicos, unidades, notas de calibración).
3. **Formato estándar en robótica**: YAML es el estándar de facto (ROS, etc.).
4. **Validación opcional**: Poder validar la estructura con Pydantic sin obligar a que sea un prerrequisito de arranque.
5. **Carga resiliente**: El sistema debe arrancar aunque falte un archivo YAML, usando valores por defecto.

## Decisión

Usar **archivos YAML separados por dominio** en `backend/config/`:

| Archivo | Propósito | Frecuencia de cambio |
|---------|-----------|---------------------|
| `servo_calibration.yaml` | PWM open/closed, deadband, límites seguros, safe pose | Baja (por calibración mecánica) |
| `tracking.yaml` | MediaPipe confidence, EMA alpha, sensibilidad, configuración serial | Media (ajustes de tracking) |
| `network.yaml` | IP, puerto, SSL, WiFi AP, WebSocket | Baja (configuración de red) |

### Estructura de archivos

#### `servo_calibration.yaml`
```yaml
# Calibración de servos MG996R por dedo
# Unidades: microsegundos PWM (rango típico 500-2500)
fingers:
  thumb:
    open_pwm: 1000      # Dedo extendido
    closed_pwm: 2000    # Dedo flexionado
    pwm_min: 800        # Límite seguro mínimo
    pwm_max: 2200       # Límite seguro máximo
    deadband_pwm: 40    # Zona muerta en µs
    invert: false       # false = abierto=menor PWM
safe_pose_pwm: 1500
safe_pose_fallback_ms: 2000
```

#### `tracking.yaml`
```yaml
mediapipe:
  min_detection_confidence: 0.5
  min_tracking_confidence: 0.5
  max_num_hands: 1
smoothing:
  ema_alpha: 0.25
  stability_frames: 3
  max_change_per_frame: 5
pose_mapper:
  sensitivity: 0.85
  thumb_x_threshold: 0.02
serial:
  protocol: "F<idx> <pwm_us>\n"
  host: "127.0.0.1"
  port: 7500
  baudrate: 115200
  heartbeat_interval_ms: 500
  heartbeat_timeout_ms: 2000
```

#### `network.yaml`
```yaml
server:
  host: "0.0.0.0"
  port: 3000
  ssl_cert: "cert.pem"
  ssl_key: "key.pem"
websocket:
  path: "/ws"
  ping_interval_s: 30
wifi_ap:
  ssid: "RobotHand"
  password: "robot2026"
```

### Carga resiliente

En `PoseMapper.__init__()` y `init_system()` en `server.py`:

```python
# Si el YAML no existe o está corrupto → fallback a defaults
try:
    with open(config_path) as f:
        cfg = yaml.safe_load(f)
except (FileNotFoundError, yaml.YAMLError):
    cfg = _DEFAULT_CALIBRATION  # diccionario hardcodeado
```

Esto asegura que el sistema arranque incluso con configuración faltante — crítico para el primer despliegue.

## Alternativas consideradas

| Alternativa | Descartada por |
|-------------|---------------|
| JSON | Sin comentarios. Crítico: la calibración de servos necesita documentación in-situ. |
| TOML | Menos estándar en robótica. ROS usa YAML. |
| Variables de entorno | Escala mal con calibración por dedo (5 dedos × 6 parámetros = 30 vars). |
| Base de datos SQLite | Overkill para configuración estática que cambia con poca frecuencia. |
| Un solo archivo grande | Mezcla conceptos ortogonales. Diferentes frecuencias de cambio. |
| Pydantic obligatorio | Añade dependencia y complejidad. La validación debe ser opcional para desarrollo rápido. |

## Consecuencias

**Positivas:**
- Separación limpia por dominio de configuración.
- Comentarios YAML permiten documentar valores de calibración in-situ.
- Formato estándar en robótica (ROS, etc.) — facilita la adopción.
- Carga resiliente: el sistema arranca aunque falten archivos.
- Validación con Pydantic disponible como capa opcional.

**Negativas / trade-offs:**
- Tres archivos en lugar de uno: más puntos de gestión.
- Sin validación Pydantic por defecto: errores tipográficos en YAML pueden pasar desapercibidos hasta runtime.
- YAML permite estructuras complejas que pueden ser difíciles de debuggear.

## Términos de dominio afectados

- **Config** — ver `CONTEXT.md`

## Señales de revisión

- Si la configuración crece a más de 5 archivos, considerar un directorio `config/` con subdirectorios o un sistema de merging.
- Si aparecen errores recurrentes por YAML mal formado, implementar validación Pydantic como paso obligatorio de CI.
