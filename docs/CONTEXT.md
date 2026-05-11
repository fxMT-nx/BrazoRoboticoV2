# Brazo Robotico V2 — Domain Context

Sistema de control de una mano robótica de 6 servos mediante visión por computador.
Arquitectura dual-board: Arduino UNO Q (Qualcomm QRB2210) como cerebro + Arduino Mega 2560 como controlador PWM.

## Subdomain Classification

| Subdominio | Tipo | Módulos | ¿Por qué? |
|------------|------|---------|-----------|
| **Landmark Tracking** | Supporting | `frontend/src/hooks/useWebSocket.ts`, `frontend/src/components/CameraView.tsx` | MediaPipe.js es una solución existente. No hay ventaja competitiva en implementar detección de mano propia. |
| **Pose Mapping** | **Core** | `backend/pose_mapper.py` | La estrategia de mapeo landmarks→PWM (umbral de pulgar por eje X, ángulo PIP para dedos largos, green dot + auto-calibración para muñeca, filtros EMA+deadband+rate limit) es la ventaja competitiva. Cómo se traduce una pose a PWM define la experiencia. |
| **Serial Communication** | Supporting | Protocolo `F<idx> <pwm_us>\n`, SOCAT bridge | Comunicación serial es un problema resuelto. SOCAT + USB Gadget es infraestructura commodity. |
| **Record / Replay** | Supporting | `frontend/src/components/ReplayControls.tsx` | Almacenar y reproducir secuencias de landmarks+angles es útil pero no diferenciador. |
| **Web UI** | Supporting | `frontend/src/` (React + Three.js + Recharts) | La UI es necesaria pero no es la ventaja competitiva. Podría sustituirse por una CLI si fuera necesario. |
| **WiFi AP** | Generic | `deploy/wifi-ap/` | Punto de acceso WiFi es un problema completamente resuelto (hostapd + dnsmasq). |

## Language

**Angles (PWM)**:
Vector de 6 enteros (800–2200 µs) representando la posición PWM de cada Servo.
Cada valor corresponde a un servo en orden: `[thumb, index, middle, ring, pinky, wrist]`.
_Evitar_: posición, grados, valor

**Bridge**:
Sketch del STM32U585 dentro del UNO Q que actúa como puente UART entre Qualcomm Linux y Arduino Mega. También controla la LED Matrix (12×8 LEDs rojos que muestra el número de dedos levantados o un smiley). Reenvía todos los comandos `F<idx>` y `H\n` al Mega de forma transparente.
_Evitar_: puente

**Command**:
Mensaje serial en formato `F<idx> <pwm_us>\n` desde Flask al Mega. Donde `idx` es el índice del servo (0–5: thumb=0, index=1, middle=2, ring=3, pinky=4, wrist=5) y `pwm_us` es el ancho de pulso en microsegundos (800–2200 µs).
_Evitar_: mensaje, trama

**Config**:
Archivos YAML separados por dominio de configuración: `servo_calibration.yaml` (calibración de servos), `tracking.yaml` (parámetros de MediaPipe y mapeo), `network.yaml` (IP, puerto, SSL, WiFi AP). Carga resiliente: si falta un archivo, se usan valores por defecto.
_Evitar_: configuración única

**Deep / Regular Finger**:
Los 5 dedos se dividen en dos tipos: **Thumb** (pulgar, servo 0) con estrategia de mapeo basada en eje X, y **RegularFinger** (index, middle, ring, pinky, servos 1–4) con estrategia basada en ángulo PIP. La **Wrist** (muñeca, servo 5) es un caso aparte: calculada desde green dot tracking (método principal) o landmarks 2D (fallback), con auto-calibración de rango en vivo.

**Finger (Dedo)**:
Cada uno de los 5 actuadores de los dedos. 1 DOF (grado de libertad) por Finger, controlado por un servo MG996R. La muñeca (Wrist) es el sexto servo pero no se considera un "dedo".
| Índice | Dedo / Servo | Nombre canónico | Pin Mega | PWM open | PWM closed |
|--------|-------------|-----------------|----------|----------|------------|
| 0 | Pulgar | Thumb | D3 | 1000 µs | 2000 µs |
| 1 | Índice | Index | D5 | 1000 µs | 2000 µs |
| 2 | Medio | Middle | D6 | 1000 µs | 2000 µs |
| 3 | Anular | Ring | D9 | 1000 µs | 2000 µs |
| 4 | Meñique | Pinky | D10 | 1000 µs | 2000 µs |
| 5 | Muñeca | Wrist | D11 | 1000 µs | 2000 µs |
_Evitar_: dedo genérico (usar Finger o el nombre específico)

**FingerMapper**:
Estrategia específica de mapeo para cada tipo de dedo dentro de PoseMapper.
- **ThumbMapper**: Comparación de eje X entre `THUMB_TIP(4)` y `THUMB_IP(3)`. Si `tip.x - ip.x < -threshold` → cerrado; si `> +threshold` → abierto. Interpolación lineal en la zona de transición. Acepta `handedness` para invertir la lógica según la mano detectada (izquierda/derecha).
- **RegularFingerMapper**: Cálculo del ángulo 3D en la articulación PIP. `flexion = joint_angle_3D(landmarks, MCP, PIP, TIP)`. Dedo recto ≈ 180°, flexionado ≈ 0°. `raw = (180 - flexion) × sensitivity`.
- **Wrist**: No tiene mapper separado; el cálculo se hace directamente en `_compute_all_pwm()` usando el green dot o el fallback de landmarks 2D, con auto-calibración de rango y EMA smoothing.

**Green Dot**:
Marcador verde físico pegado en el antebrazo robótico. Detectado por `findGreenDot()` en el frontend (`CameraView.tsx`) escaneando píxeles con G > 150, R < 120, B < 120 y G > R + 50. Devuelve la posición del centroide (x, y) o null si no se encuentra. Usado como referencia para calcular el **wrist angle** con precisión.

**Handedness**:
Detección de mano izquierda o derecha desde `HandLandmarker` de MediaPipe. Se extrae del campo `handResult.handedness[0][0].categoryName` ('Left' o 'Right'). Se envía al backend en cada frame y se usa en **ThumbMapper** para invertir la lógica del pulgar según la mano detectada: si es 'Right', el pulgar se abre cuando `tip.x > ip.x`; si es 'Left', al revés. Si no hay handedness disponible, se infiere por la posición del pulgar.

**Heartbeat**:
Comando `H\n` enviado cada 500ms desde Flask al Mega para mantener vivo el watchdog del Mega. Si transcurren 2000ms sin heartbeat (`safe_pose_fallback_ms`), el Mega lleva todos los servos a Safe Pose (1500 µs). El STM32 Bridge también usa el heartbeat para determinar si la conexión está activa (modo debug del LED Matrix).
_Evitar_: ping, latido

**Interpolation**:
Rampa suave (perfil de velocidad trapezoidal: aceleración, crucero, desaceleración) implementada en el firmware del Mega. Mueve el servo desde el PWM actual hacia el PWM target en pasos de 10ms con aceleración de 2 µs/paso² y velocidad máxima de 8 µs/paso. Previene movimientos bruscos que podrían dañar los servos o la estructura mecánica.

**Landmarks**:
21 puntos 3D (x, y, z) del tracking de mano de MediaPipe. Normalizados en rango [0.0, 1.0]. Cada landmark tiene un índice e índices canónicos según la convención de MediaPipe Hands:
- 0: Wrist, 1-4: Thumb (CMC, MCP, IP, TIP)
- 5-8: Index (MCP, PIP, DIP, TIP), 9-12: Middle, 13-16: Ring, 17-20: Pinky
_Evitar_: puntos, keypoints

**LED Matrix**:
Matriz de 12×8 LEDs rojos integrada en el UNO Q. Controlada por el STM32. Muestra el número de dedos levantados (0–5, la muñeca no se cuenta como dedo) o un smiley según el estado del sistema. Incluye animaciones: parpadeo de ojos cada 3s, pulso de heartbeat, blink de error, e iconos de grabación/reproducción.

**Mega**:
Arduino Mega 2560 — microcontrolador secundario responsable de generar las señales PWM para los 6 servos MG996R. Recibe comandos por **Serial1** (RX1=D19, TX1=D18) desde el STM32 Bridge. Implementa interpolación suave con perfil trapezoidal y watchdog de heartbeat con timeout a safe pose.

**PoseMapper**:
Clase Python (`backend/pose_mapper.py`) que convierte 21 landmarks 3D de MediaPipe + wrist_angle + handedness en 6 valores PWM para servos MG996R. Pipeline completo por frame:
1. Validación de entrada (21 landmarks con x, y, z)
2. Sin EMA sobre coordenadas (el suavizado viene del frontend)
3. Cálculo de PWM por dedo (ThumbMapper o RegularFingerMapper, con handedness)
4. Cálculo de PWM de muñeca (green dot: mapeo lineal ±90° → 0–1; fallback: atan2 desde centro de palma; auto-calibración de rango + EMA smoothing)
5. Deadband por servo (evita micro-oscilaciones)
6. Rate limit por frame (protección mecánica)
7. Actualización de estado interno
Si los landmarks son inválidos o hay error → retorna Safe Pose (1500 µs × 6).
_Evitar_: IK Engine (ver ADR-0001)

**Qualcomm**:
SoC QRB2210 dentro del Arduino UNO Q (ABX00162). Ejecuta Debian Linux, Flask, y sirve el frontend React. No tiene acceso directo a UART en D0/D1 (usados por STM32), por lo que la comunicación con el Mega requiere el Bridge STM32 + USB Gadget Serial + SOCAT.
_Evitar_: el Linux

**Record / Replay**:
Grabación y reproducción de secuencias de frames. Cada frame contiene `{landmarks: Landmark[21], angles: Angles, timestamp: number}`. La grabación se almacena en el frontend (memoria del navegador) y se puede descargar como JSON. La reproducción envía los landmarks grabados a través del mismo pipeline de WebSocket.

**Safe Pose**:
Posición segura a la que van todos los servos cuando ocurre una condición de error o timeout: **1500 µs** (posición central, 90°). Se dispara en:
- Timeout de heartbeat (2s sin `H\n`)
- Error en `landmarks_to_pwm()`
- Desconexión del cliente WebSocket
- Error en el bucle de procesamiento de frames
Afecta a los 6 servos (5 dedos + muñeca).

**SerialManager**:
Módulo Python (`backend/serial_manager.py`, 518 líneas) que gestiona la conexión TCP al puerto 7500 (SOCAT bridge). Envía comandos `F<idx> <pwm_us>\n` y heartbeat `H\n`. Reconexión automática con backoff exponencial (0.5s → 1s → 2s … max 30s). Thread-safe con Lock. Valida índices mediante `VALID_IDX_RANGE = range(0, 6)` y clampea PWM a límites seguros. Implementa context manager (`with SerialManager() as sm:`).

**Servo**:
Actuador Tower Pro MG996R. Servo estándar RC con rango PWM de 500–2500 µs (mapeado a 0°–180°). En este sistema se usa en el rango seguro 800–2200 µs. Torque: 9.4 kg·cm a 4.8V, 11 kg·cm a 6V. Velocidad: 0.17s/60°.
_Evitar_: motor

**SmoothAngles**:
Hook React `useSmoothAngles` (`frontend/src/hooks/useSmoothAngles.ts`) que interpola valores de ángulos (Angles[6]) para transiciones visuales suaves en el frontend. Cuando rawAngles cambia, anima desde el valor mostrado actual al nuevo usando cubic ease-out durante **40ms** vía requestAnimationFrame. Previene saltos visuales en la UI sin afectar los valores reales enviados al hardware.

**SOCAT**:
Herramienta Linux que crea un bridge bidireccional entre TCP:7500 y `/dev/ttyGS0` (USB Gadget Serial). Gestionado como servicio systemd (`socat.service`) con `Restart=always`. Permite que Flask se comunique con el STM32/Mega vía TCP sin necesidad de permisos de dispositivo serial.
_Evitar_: proxy serial

**STM32**:
MCU STM32U585 dentro del UNO Q. Gestiona el Bridge serial (UART entre USB Gadget y Mega) y la LED Matrix. Se comunica con el Mega a través de **Serial1** (USART1, pines D0/D1) que están conectados internamente al Mega Serial1 en la placa UNO Q. Reenvía todos los comandos al Mega y las respuestas del Mega de vuelta al QRB2210.
_Evitar_: microcontrolador

**Tracking**:
Detección en tiempo real de mano humana usando MediaPipe Hands.js en el navegador. Produce 21 landmarks 3D por frame a ~30 FPS. Configurable vía `tracking.yaml` (confidence, número de manos, complejidad del modelo). También extrae `handedness` ('Left'/'Right') desde el resultado de HandLandmarker.
_Evitar_: detección

**UNO Q**:
Arduino UNO Q (ABX00162) — placa principal del sistema. Contiene dos chips: Qualcomm QRB2210 (Debian Linux, aplicación principal) y STM32U585 (bridge serial + LED Matrix). Expone USB Gadget Serial como `/dev/ttyGS0` para comunicación con el exterior.

**Wrist (Muñeca)**:
Servo 5 (índice 5, pin D11 del Mega). Controla flexión/extensión vertical de la muñeca robótica. No se cuenta como "dedo" en la LED Matrix ni en la UI de conteo. El ángulo se calcula mediante dos métodos:
1. **Principal**: Green dot tracking en el antebrazo — `findGreenDot()` detecta un marcador verde físico y calcula el ángulo de muñeca por desplazamiento del punto.
2. **Fallback**: Cálculo 2D desde landmarks — atan2 entre el centro de la palma y la landmark 0 (wrist) de MediaPipe.
Incluye auto-calibración: observa el rango mínimo/máximo de `wrist_raw` en vivo y estira al rango completo [0, 1]. EMA smoothing con alpha 0.15.

## Anti-términos

- ❌ "IK Engine" — Usar **PoseMapper**. No hay cinemática inversa real en el sistema (1 DOF por dedo). El término confunde a desarrolladores que buscan solvers de Jacobianas. (Ver ADR-0001)
- ❌ "posición" o "grados" para referirse a servos — Usar **PWM (µs)**. Los servos se controlan por ancho de pulso, no por ángulos. El firmware del Mega trabaja en µs, no en grados. (Ver ADR-0002)
- ❌ "proxy serial" — Usar **SOCAT**. SOCAT es un bridge bidireccional, no un proxy. El término "proxy" implica caché, filtrado o intermediación que SOCAT no realiza.
- ❌ "el Linux" para referirse al Qualcomm — Usar **Qualcomm** o **UNO Q**. Hay dos procesadores en el UNO Q; "el Linux" es ambiguo.
- ❌ "motor" para referirse a los servos — Usar **Servo** o **MG996R**. Los servos tienen control de posición; los motores no.
- ❌ "mensaje" o "trama" para referirse a comandos seriales — Usar **Command**. El protocolo tiene un formato específico `F<idx> <pwm_us>\n` que no es genérico.
- ❌ "solo dedos" para referirse a los 6 ejes — Usar **6 servos**. El sistema tiene 5 dedos + 1 muñeca. "Dedos" excluye la muñeca y causa confusión en la calibración y el mapeo.

## Communication Path

Camino completo de una señal desde la mano del usuario hasta el movimiento del servo:

```
👋 Mano del usuario + 🟢 Punto verde en antebrazo
    ↓  (cámara web, ~30 FPS)
🖥️  Navegador (MediaPipe.js + findGreenDot)
    ↓  landmarks (21×3D) + wrist_angle + handedness por WebSocket
🐍  Flask (QRB2210 — Debian Linux)
    ↓  PoseMapper: landmarks + wrist_angle + handedness → [800..2200 µs] ×6
    ↓  SerialManager: Command F<idx> <pwm_us>\n
    ↓  TCP:7500
🔗  SOCAT (systemd — Restart=always)
    ↓  /dev/ttyGS0 (USB Gadget Serial)
🔌  STM32U585 (Bridge — reenvía al Mega, procesa LED Matrix)
    ↓  Serial1 (D0/D1 — USART1)
🔌  Arduino Mega 2560
    ↓  Serial1 → firmware → Interpolation trapezoidal → PWM
⚙️  Servo MG996R ×6
    ↓
🤖  Mano robótica se mueve
```

## Relationships

- Un **PoseMapper** produce 6 **PWMs** (5 dedos + 1 muñeca)
- Un **PoseMapper** contiene 5 **FingerMapper** (1 ThumbMapper + 4 RegularFingerMapper) + cálculo directo de **Wrist**
- Un **Command** `F<idx> <pwm_us>\n` controla exactamente 1 **Servo**
- Un **Heartbeat** `H\n` mantiene vivos todos los **Servos** (watchdog global)
- Una secuencia de **Landmarks** (21×3D) + **wrist_angle** + **handedness** produce 6 valores **Angles (PWM)**
- Un **Record** contiene N **Frames** (cada frame = landmarks + angles + timestamp)
- **SOCAT** conecta exactamente 1 puerto TCP (7500) a 1 dispositivo serial (/dev/ttyGS0)
- **STM32** es el bridge entre 1 USB Gadget Serial y 1 UART (Serial1 → Mega Serial1)
- El **Green Dot** es detectado por `findGreenDot()` en el frontend y se convierte en **wrist_angle** para el **PoseMapper**
- La **Handedness** se extrae en el frontend y se usa en el **ThumbMapper** para decidir la dirección de apertura del pulgar
- **SmoothAngles** toma los **Angles** raw y produce **Angles** interpolados para la UI (sin afectar al hardware)

## Example dialogue

> **Dev:** "Cuando el **PoseMapper** recibe landmarks inválidos, ¿qué pasa con los servos?"
> **Domain expert:** "Va a **Safe Pose** — los 6 servos a 1500 µs. El **Mega** también tiene watchdog: si no recibe **Heartbeat** por 2 segundos, también va a safe pose automáticamente."
>
> **Dev:** "¿Y si un dedo se queda temblando cerca de una posición?"
> **Domain expert:** "El **PoseMapper** tiene **deadband** por servo (15 µs para todos). Si la variación es menor que ese umbral, no envía el comando. Además, el **Mega** hace **Interpolation** suave con perfil trapezoidal."
>
> **Dev:** "El **wrist_angle** ¿cómo se calcula exactamente?"
> **Domain expert:** "Dos formas. La principal: un **Green Dot** en el antebrazo — `findGreenDot()` lo detecta en el frontend y calcula el ángulo. Si no hay punto verde, cae en el fallback: atan2 entre el centro de la palma y la landmark 0 (wrist). Además tiene auto-calibración: aprende el rango real de movimiento en vivo y lo estira al rango completo."
>
> **Dev:** "El nombre 'PoseMapper' sugiere que mapea poses, ¿por qué no se llama IK Engine como antes?"
> **Domain expert:** "Porque no hay cinemática inversa real. Cada dedo tiene 1 solo servo. Es un mapeo directo de landmarks a PWM, no una cadena cinemática multi-DOF. Llamarlo IK Engine haría que futuros desarrolladores busquen solvers de Jacobianas donde no existen."

## Flagged ambiguities

- ~~"IK Engine"~~ → resuelto: se renombró a **PoseMapper** (ver ADR-0001)
- ~~"proxy serial"~~ → resuelto: es **SOCAT**, un bridge bidireccional
- ~~"motor"~~ → resuelto: son **Servos MG996R**
- ~~"posición" para servos~~ → resuelto: se usa **PWM (µs)**
- ~~"ángulo" en comandos seriales~~ → resuelto: el protocolo V2 usa microsegundos PWM (ver ADR-0002)
- ~~"Serial3" para comunicación con Mega~~ → resuelto: es **Serial1** (RX1=D19, TX1=D18), conectado físicamente al STM32 Bridge
