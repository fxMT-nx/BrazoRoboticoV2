"""
PoseMapper — Convierte landmarks de MediaPipe (21×3D) a PWM para servos MG996R.

Transforma la detección de mano humana en señales de control para 5 servos
(1 DOF por dedo). Reemplaza el anterior "IK Engine" — es independiente de Flask
y se comunica mediante PWM sobre protocolo serial.

Estrategia por dedo:
  - Pulgar (servo 0): comparación eje X entre THUMB_TIP y THUMB_IP.
    Cuando la punta cruza hacia adentro (tip.x < ip.x) el dedo se cierra;
    caso contrario se abre. Mapeo lineal de la diferencia X.
  - 4 dedos largos (servos 1-4): ángulo 3D en la articulación PIP.
    Se calcula el ángulo entre vectores (TIP → PIP) y (MCP → PIP).
    A menor ángulo (más flexionado), mayor PWM.

Filtros en cadena: EMA sobre landmarks → cálculo PWM → deadband → rate limit.
"""

import math
import logging
from typing import Optional

import yaml

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Constantes de landmarks MediaPipe (índices)
# ---------------------------------------------------------------------------
WRIST = 0

THUMB_TIP = 4
THUMB_IP = 3
THUMB_MCP = 2

INDEX_MCP = 5
INDEX_PIP = 6
INDEX_TIP = 8

MIDDLE_MCP = 9
MIDDLE_PIP = 10
MIDDLE_TIP = 12

RING_MCP = 13
RING_PIP = 14
RING_TIP = 16

PINKY_MCP = 17
PINKY_PIP = 18
PINKY_TIP = 20

# ---------------------------------------------------------------------------
# Defaults (fallback si no existe archivo YAML)
# ---------------------------------------------------------------------------
_DEFAULT_CALIBRATION = {
    "fingers": {
        "thumb": {
            "open_pwm": 1000,
            "closed_pwm": 2000,
            "pwm_min": 800,
            "pwm_max": 2200,
            "deadband_pwm": 40,
            "invert": False,
        },
        "index": {
            "open_pwm": 1000,
            "closed_pwm": 2000,
            "pwm_min": 800,
            "pwm_max": 2200,
            "deadband_pwm": 25,
            "invert": False,
        },
        "middle": {
            "open_pwm": 1000,
            "closed_pwm": 2000,
            "pwm_min": 800,
            "pwm_max": 2200,
            "deadband_pwm": 25,
            "invert": False,
        },
        "ring": {
            "open_pwm": 1000,
            "closed_pwm": 2000,
            "pwm_min": 800,
            "pwm_max": 2200,
            "deadband_pwm": 25,
            "invert": False,
        },
        "pinky": {
            "open_pwm": 1000,
            "closed_pwm": 2000,
            "pwm_min": 800,
            "pwm_max": 2200,
            "deadband_pwm": 25,
            "invert": False,
        },
        "wrist": {
            "open_pwm": 1000,
            "closed_pwm": 2000,
            "pwm_min": 800,
            "pwm_max": 2200,
            "deadband_pwm": 15,
            "invert": False,
        },
    },
    "safe_pose_pwm": 1500,
    "safe_pose_fallback_ms": 2000,
}

_DEFAULT_TRACKING = {
    "pose_mapper": {
        "sensitivity": 0.85,
        "thumb_x_threshold": 0.02,
    },
    "smoothing": {
        "ema_alpha": 0.25,
        "stability_frames": 3,
        "max_change_per_frame": 5,
    },
}

# Mapa de índices de dedo a nombre
_FINGER_NAMES = ["thumb", "index", "middle", "ring", "pinky"]

# Mapa de índices de dedo → landmarks para cálculo PIP
# (mcp, pip, tip)
_FINGER_PIP_LANDMARKS = [
    None,  # thumb — no usa PIP
    (INDEX_MCP, INDEX_PIP, INDEX_TIP),
    (MIDDLE_MCP, MIDDLE_PIP, MIDDLE_TIP),
    (RING_MCP, RING_PIP, RING_TIP),
    (PINKY_MCP, PINKY_PIP, PINKY_TIP),
]

# ---------------------------------------------------------------------------
# Funciones auxiliares matemáticas
# ---------------------------------------------------------------------------


def angle_between_3d(v1: tuple[float, float, float],
                     v2: tuple[float, float, float]) -> float:
    """Ángulo en grados entre dos vectores 3D.

    Usa la fórmula segura: atan2(cross|, dot) para evitar
    inestabilidad numérica con vectores colineales.

    Args:
        v1: Primer vector (x, y, z).
        v2: Segundo vector (x, y, z).

    Returns:
        Ángulo en grados en [0, 180].
    """
    dot = v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2]

    cross_x = v1[1] * v2[2] - v1[2] * v2[1]
    cross_y = v1[2] * v2[0] - v1[0] * v2[2]
    cross_z = v1[0] * v2[1] - v1[1] * v2[0]
    cross_mag = math.sqrt(cross_x * cross_x + cross_y * cross_y + cross_z * cross_z)

    # Clamp para evitar NaN por redondeo
    dot = max(-1.0, min(1.0, dot))

    angle_rad = math.atan2(cross_mag, dot)
    return math.degrees(angle_rad)


def joint_angle_3d(landmarks: list[dict],
                   a: int, b: int, c: int) -> float:
    """Ángulo en la articulación *b* formado por los puntos a→b→c.

    Calcula el ángulo entre los vectores (a - b) y (c - b).
    Útil para medir flexión: un dedo recto da ~180°, flexionado da ~0°.

    Args:
        landmarks: Lista de 21 dicts con claves 'x', 'y', 'z' normalizados.
        a, b, c: Índices de landmarks (punto b es la articulación).

    Returns:
        Ángulo en grados en [0, 180].
    """
    p_a = (landmarks[a]["x"], landmarks[a]["y"], landmarks[a]["z"])
    p_b = (landmarks[b]["x"], landmarks[b]["y"], landmarks[b]["z"])
    p_c = (landmarks[c]["x"], landmarks[c]["y"], landmarks[c]["z"])

    v1 = (p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2])
    v2 = (p_c[0] - p_b[0], p_c[1] - p_b[1], p_c[2] - p_b[2])

    return angle_between_3d(v1, v2)


def lerp(value: float, out_min: float, out_max: float,
         in_min: float = 0.0, in_max: float = 180.0) -> float:
    """Interpolación lineal con clamping.

    Mapea *value* del rango [in_min, in_max] → [out_min, out_max].
    El resultado se clamp (recorta) a [out_min, out_max].

    Args:
        value: Valor de entrada a mapear.
        out_min: Límite inferior de salida.
        out_max: Límite superior de salida.
        in_min: Límite inferior de entrada (default 0.0).
        in_max: Límite superior de entrada (default 180.0).

    Returns:
        Valor interpolado y clamp en [out_min, out_max].
    """
    if abs(in_max - in_min) < 1e-9:
        return (out_min + out_max) / 2.0

    normalized = (value - in_min) / (in_max - in_min)
    normalized = max(0.0, min(1.0, normalized))
    return out_min + normalized * (out_max - out_min)


# ---------------------------------------------------------------------------
# Funciones de filtrado
# ---------------------------------------------------------------------------


def ema_filter(current: list[dict],
               previous: Optional[list[dict]],
               alpha: float) -> list[dict]:
    """Filtro EMA (Exponential Moving Average) sobre landmarks 3D.

    Suaviza variaciones frame a frame en las coordenadas (x, y, z).

    Args:
        current: Landmarks del frame actual (21 dicts con 'x', 'y', 'z').
        previous: Landmarks del frame anterior filtrados, o ``None``.
        alpha: Factor de suavizado (0.0 = máximo suavizado, 1.0 = sin filtro).

    Returns:
        Landmarks filtrados (misma estructura que *current*).
    """
    if previous is None or len(previous) != len(current):
        return [dict(p) for p in current]  # primera vez, copia superficial

    filtered = []
    for i, cur in enumerate(current):
        prev = previous[i]
        filtered.append({
            "x": alpha * cur["x"] + (1.0 - alpha) * prev["x"],
            "y": alpha * cur["y"] + (1.0 - alpha) * prev["y"],
            "z": alpha * cur["z"] + (1.0 - alpha) * prev["z"],
        })
    return filtered


def apply_deadband(current_pwm: int,
                   previous_pwm: int,
                   deadband: int) -> int:
    """Si la variación es menor que *deadband*, retorna el valor anterior.

    Evita micro-oscilaciones cuando el PWM está cerca de un valor estable.

    Args:
        current_pwm: Valor PWM calculado para el frame actual.
        previous_pwm: Valor PWM del frame anterior.
        deadband: Umbral de zona muerta en µs.

    Returns:
        *current_pwm* si |current - previous| ≥ deadband, sino *previous_pwm*.
    """
    if abs(current_pwm - previous_pwm) < deadband:
        return previous_pwm
    return current_pwm


def apply_rate_limit(current_pwm: int,
                     previous_pwm: int,
                     max_change: int) -> int:
    """Limita la variación máxima por frame.

    Previene movimientos bruscos (protección mecánica del servo).

    Args:
        current_pwm: Valor PWM deseado.
        previous_pwm: Valor PWM del frame anterior.
        max_change: Máxima variación permitida en µs.

    Returns:
        PWM limitado, como máximo ± *max_change* respecto a *previous_pwm*.
    """
    delta = current_pwm - previous_pwm
    if abs(delta) > max_change:
        return previous_pwm + (max_change if delta > 0 else -max_change)
    return current_pwm


# ---------------------------------------------------------------------------
# Carga de configuración
# ---------------------------------------------------------------------------


def _load_calibration(config_path: str) -> dict:
    """Carga calibración de servos desde archivo YAML.

    Si el archivo no existe o tiene errores, usa valores por defecto
    y emite advertencia por logging.

    Args:
        config_path: Ruta al archivo YAML de calibración.

    Returns:
        Diccionario de configuración con estructura normalizada.
    """
    try:
        with open(config_path, "r") as f:
            cfg = yaml.safe_load(f)
        if cfg is None or "fingers" not in cfg:
            logger.warning("YAML vacío o sin 'fingers'. Usando defaults.")
            return _DEFAULT_CALIBRATION
        logger.info("Calibración cargada desde %s", config_path)
        return cfg
    except FileNotFoundError:
        logger.warning(
            "Archivo de calibración %s no encontrado. Usando defaults.", config_path
        )
        return _DEFAULT_CALIBRATION
    except yaml.YAMLError as e:
        logger.error("Error al parsear YAML %s: %s. Usando defaults.", config_path, e)
        return _DEFAULT_CALIBRATION
    except Exception as e:
        logger.exception("Error inesperado al cargar %s: %s", config_path, e)
        return _DEFAULT_CALIBRATION


def _load_tracking_config(config_path: str) -> dict:
    """Carga configuración de tracking desde YAML.

    Args:
        config_path: Ruta al archivo YAML de tracking.

    Returns:
        Diccionario con keys 'pose_mapper' y 'smoothing'.
    """
    try:
        with open(config_path, "r") as f:
            cfg = yaml.safe_load(f)
        if cfg is None:
            return dict(_DEFAULT_TRACKING)
        return cfg
    except (FileNotFoundError, yaml.YAMLError, Exception):
        logger.warning("Tracking config no disponible. Usando defaults.", exc_info=True)
        return dict(_DEFAULT_TRACKING)


# ---------------------------------------------------------------------------
# PoseMapper
# ---------------------------------------------------------------------------


class PoseMapper:
    """Convierte landmarks de MediaPipe (21×3D) a PWM para 6 servos MG996R.

    Modo de uso::

        mapper = PoseMapper()
        pwm = mapper.landmarks_to_pwm(landmarks)
        # → [thumb, index, middle, ring, pinky, wrist] en µs
    """

    def __init__(self,
                 calibration_path: str = "config/servo_calibration.yaml",
                 tracking_path: str = "config/tracking.yaml"):
        """Inicializa el mapper cargando configuraciones.

        Args:
            calibration_path: Ruta al YAML de calibración de servos.
            tracking_path: Ruta al YAML de configuración de tracking.
        """
        # Cargar configuraciones
        calib = _load_calibration(calibration_path)
        track = _load_tracking_config(tracking_path)

        # Extraer calibración por dedo
        finger_cfgs = calib.get("fingers", {})
        self.finger_configs: list[dict] = []
        for name in _FINGER_NAMES:
            cfg = finger_cfgs.get(name, {})
            self.finger_configs.append({
                "open_pwm": cfg.get("open_pwm", 1000),
                "closed_pwm": cfg.get("closed_pwm", 2000),
                "pwm_min": cfg.get("pwm_min", 800),
                "pwm_max": cfg.get("pwm_max", 2200),
                "deadband_pwm": cfg.get("deadband_pwm", 25),
                "invert": cfg.get("invert", False),
            })

        self.thumb_config = self.finger_configs[0]  # alias para el pulgar

        self.safe_pose_pwm: int = calib.get("safe_pose_pwm", 1500)

        # Cargar calibración de muñeca (servo 5)
        wrist_cfg = finger_cfgs.get("wrist", _DEFAULT_CALIBRATION["fingers"]["wrist"])
        self.wrist_config = {
            "open_pwm": wrist_cfg.get("open_pwm", 1000),
            "closed_pwm": wrist_cfg.get("closed_pwm", 2000),
            "pwm_min": wrist_cfg.get("pwm_min", 800),
            "pwm_max": wrist_cfg.get("pwm_max", 2200),
            "deadband_pwm": wrist_cfg.get("deadband_pwm", 15),
            "invert": wrist_cfg.get("invert", False),
        }

        self.PWM_CENTER = 1500

        # Configuración de tracking
        pm_cfg = track.get("pose_mapper", {})
        self.sensitivity: float = pm_cfg.get("sensitivity", 0.85)
        self.thumb_x_threshold: float = pm_cfg.get("thumb_x_threshold", 0.02)

        sm_cfg = track.get("smoothing", {})
        self.ema_alpha: float = sm_cfg.get("ema_alpha", 0.25)
        self.max_change_pwm: int = sm_cfg.get("max_change_per_frame", 5) * 6
        # aprox: 5° × ~5.56 µs/° ≈ 28 µs → lo redondeamos a 30

        # Estado interno (persistente entre frames)
        self._prev_filtered_landmarks: Optional[list[dict]] = None
        self._prev_pwm: Optional[list[int]] = None
        # Estado para suavizado de muñeca (raw 0-1, EMA α=0.05)
        self._prev_wrist_raw: Optional[float] = None

        # Auto-calibración de muñeca (observa rango real y lo estira)
        self._wrist_min: float = 1.0
        self._wrist_max: float = 0.0
        self._wrist_calibrated: bool = False

        logger.info(
            "PoseMapper inicializado. Sensibilidad=%.2f, EMA α=%.2f, "
            "thumb_threshold=%.3f",
            self.sensitivity, self.ema_alpha, self.thumb_x_threshold,
        )

    # ------------------------------------------------------------------
    # API pública
    # ------------------------------------------------------------------

    def landmarks_to_pwm(self, landmarks: Optional[list[dict]],
                         wrist_angle: float | None = None,
                         handedness: str | None = None) -> list[int]:
        """Convierte landmarks de MediaPipe a PWM para 6 servos.

        El pipeline completo por frame es:

        1. Validación de entrada
        2. Cálculo de PWM por dedo y muñeca (sin EMA — ya viene suavizado del frontend)
        3. Deadband y rate limit por servo
        4. Actualización de estado interno

        Args:
            landmarks: Lista de 21 dicts con claves ``x``, ``y``, ``z``
                       en rango normalizado [0.0, 1.0].
                       Si es ``None``, vacío o < 21, retorna *safe pose*
                       (``[1500, 1500, 1500, 1500, 1500, 1500]``).
            wrist_angle: (Opcional) Ángulo de muñeca en grados desde Holistic.
                         -90° = flexión, 0° = neutro, +90° = extensión.
                         Si es None, usa el cálculo interno mejorado.
            handedness: (Opcional) 'Left' o 'Right' desde MediaPipe Hands.
                        Si es None, se infiere por la posición del pulgar.

        Returns:
            Lista de 6 enteros: ``[thumb, index, middle, ring, pinky, wrist]``
            en microsegundos (rango típico 800–2200 µs).
        """
        # --- Paso 1: Validación ---
        if not self._validate_landmarks(landmarks):
            safe = [self.safe_pose_pwm] * 6
            logger.debug("Landmarks inválidos. Retornando safe pose: %s", safe)
            # Resetear estado para evitar propagación de datos corruptos
            self._prev_filtered_landmarks = None
            self._prev_pwm = None
            self._prev_wrist_raw = None
            self._wrist_min = 1.0
            self._wrist_max = 0.0
            self._wrist_calibrated = False
            return safe

        # --- Paso 2: Sin EMA (ya viene suavizado del frontend) ---
        smoothed = landmarks

        # --- Paso 3: Calcular PWM por dedo ---
        try:
            pwm_values = self._compute_all_pwm(smoothed, wrist_angle, handedness)
        except Exception:
            logger.exception("Error en cómputo de PWM. Retornando safe pose.")
            return [self.safe_pose_pwm] * 6

        # --- Paso 4: Deadband + Rate limit ---
        final_pwm = []
        for i in range(6):
            pwm = pwm_values[i]

            if self._prev_pwm is not None:
                cfg = self.finger_configs[i] if i < 5 else self.wrist_config
                pwm = apply_deadband(pwm, self._prev_pwm[i], cfg["deadband_pwm"])
                pwm = apply_rate_limit(pwm, self._prev_pwm[i], self.max_change_pwm)

            pwm = int(round(pwm))
            final_pwm.append(pwm)

        # --- Paso 5: Actualizar estado ---
        self._prev_filtered_landmarks = smoothed
        self._prev_pwm = final_pwm

        return final_pwm

    # ------------------------------------------------------------------
    # Métodos internos
    # ------------------------------------------------------------------

    @staticmethod
    def _validate_landmarks(landmarks: Optional[list[dict]]) -> bool:
        """Valida que los landmarks sean una lista de 21 dicts con x, y, z."""
        if landmarks is None:
            return False
        if not isinstance(landmarks, list):
            return False
        if len(landmarks) < 21:
            return False
        for i, lm in enumerate(landmarks[:21]):
            if not isinstance(lm, dict):
                logger.debug("Landmark %d no es dict: %s", i, type(lm))
                return False
            for key in ("x", "y", "z"):
                if key not in lm:
                    logger.debug("Landmark %d falta key '%s'", i, key)
                    return False
                val = lm[key]
                if not isinstance(val, (int, float)):
                    logger.debug("Landmark %d.%s no es numérico: %s", i, key, type(val))
                    return False
        return True

    def _compute_all_pwm(self, landmarks: list[dict],
                         wrist_angle: float | None = None,
                         handedness: str | None = None) -> list[float]:
        """Calcula PWM para los 5 dedos + muñeca.

        Args:
            landmarks: 21 puntos 3D de MediaPipe Hands.
            wrist_angle: (Opcional) Ángulo de muñeca en grados desde Holistic.
                -90° = flexión, 0° = neutro, +90° = extensión.
                Si es None, usa el cálculo interno mejorado.
            handedness: (Opcional) 'Left' o 'Right' desde MediaPipe Hands.

        Returns:
            Lista de 6 floats (sin deadband/rate-limit aún).
        """
        result = []
        for i in range(5):
            pwm = self._compute_finger_pwm(i, landmarks, handedness)
            # Clamp a límites seguros
            cfg = self.finger_configs[i]
            pwm = max(cfg["pwm_min"], min(cfg["pwm_max"], pwm))
            result.append(pwm)

        # ── Servo 5: Wrist (rotación izquierda/derecha) ──
        try:
            # Detección de rotación por diferencia de profundidad Z
            # entre el índice (MCP) y el meñique (MCP)
            idx_z = landmarks[INDEX_MCP]["z"]
            pky_z = landmarks[PINKY_MCP]["z"]
            z_diff = idx_z - pky_z  # <0 = rotado un lado, >0 = rotado otro
            # Mapear rango típico [-0.04, 0.04] a [0, 1]
            wrist_raw = max(0.0, min(1.0, (z_diff + 0.04) / 0.08))
            
            # ── AUTO-CALIBRACIÓN: estirar rango observado ──
            # Observamos el mínimo y máximo real de wrist_raw
            self._wrist_min = min(self._wrist_min, wrist_raw)
            self._wrist_max = max(self._wrist_max, wrist_raw)
            
            # Calcular rango observado
            observed_range = self._wrist_max - self._wrist_min
            
            if observed_range >= 0.03:
                self._wrist_calibrated = True
                # Estirar raw al rango completo 0-1
                wrist_raw = (wrist_raw - self._wrist_min) / observed_range
                wrist_raw = max(0.0, min(1.0, wrist_raw))
            
            # ── EMA smoothing ──
            WRIST_EMA_ALPHA = 0.15  # Más rápido que antes (0.2)
            if self._prev_wrist_raw is not None:
                wrist_raw = (self._prev_wrist_raw * (1.0 - WRIST_EMA_ALPHA) +
                             wrist_raw * WRIST_EMA_ALPHA)
            self._prev_wrist_raw = wrist_raw
            
            # Mapear a PWM
            wrist_pwm = (self.wrist_config["closed_pwm"] +
                         (self.wrist_config["open_pwm"] -
                          self.wrist_config["closed_pwm"]) * wrist_raw)
            
            wrist_pwm = max(self.wrist_config["pwm_min"],
                            min(self.wrist_config["pwm_max"], wrist_pwm))
        except Exception:
            logger.warning("Error calculando PWM de muñeca. Usando centro.", exc_info=True)
            wrist_pwm = float(self.PWM_CENTER)

        result.append(wrist_pwm)
        return result

    def _compute_finger_pwm(self, finger_idx: int,
                            landmarks: list[dict],
                            handedness: str | None = None) -> float:
        """Calcula PWM para un dedo específico.

        Args:
            finger_idx: Índice del dedo (0=thumb … 4=pinky).
            landmarks: Landmarks filtrados (21).
            handedness: (Opcional) 'Left' o 'Right' desde MediaPipe Hands.

        Returns:
            PWM en µs (sin clamp).
        """
        cfg = self.finger_configs[finger_idx]

        if finger_idx == 0:
            return self._map_thumb(landmarks, cfg, handedness)
        else:
            return self._map_long_finger(finger_idx, landmarks, cfg)

    # ------------------------------------------------------------------
    # Pulgar (servo 0)
    # ------------------------------------------------------------------

    def _map_thumb(self, landmarks: list[dict], cfg: dict,
                   handedness: str | None = None) -> float:
        """Mapea el pulgar a PWM con transición suave.

        Usa la diferencia en el eje X entre THUMB_TIP(4) y THUMB_IP(3).
        Si `handedness` es 'Left' o 'Right' (desde MediaPipe), se usa directamente.
        Si es None, se infiere comparando thumb_x < pinky_x.

        Returns:
            PWM continuo entre open_pwm y closed_pwm.
        """
        try:
            # Diferencia en X entre tip e IP
            x_diff = landmarks[THUMB_TIP]["x"] - landmarks[THUMB_IP]["x"]

            # Determinar handedness
            if handedness == 'Right':
                # Mano derecha: x_diff < 0 = cerrado, x_diff > 0 = abierto
                raw = x_diff
            elif handedness == 'Left':
                # Mano izquierda: invertido (MediaPipe refleja X)
                raw = -x_diff
            else:
                # Fallback: inferir por posición
                thumb_x = landmarks[THUMB_TIP]["x"]
                pinky_x = landmarks[PINKY_MCP]["x"]
                is_right_hand = thumb_x < pinky_x
                raw = x_diff if is_right_hand else -x_diff

            # raw suele estar en rango [-0.05, 0.05]. Mapear a [0, 1]
            # -0.05 → 0 (cerrado), +0.05 → 1 (abierto)
            normalized = max(0.0, min(1.0, (raw + 0.01) / 0.02))

            # Mapear a PWM: 0 = closed_pwm, 1 = open_pwm
            pwm = cfg["closed_pwm"] + (cfg["open_pwm"] - cfg["closed_pwm"]) * normalized

            return float(pwm)
        except Exception:
            logger.warning("Error en _map_thumb. Usando centro.", exc_info=True)
            return float(self.PWM_CENTER)

    # ------------------------------------------------------------------
    # Dedos largos (servos 1-4)
    # ------------------------------------------------------------------

    def _map_long_finger(self, finger_idx: int,
                         landmarks: list[dict],
                         cfg: dict) -> float:
        """Mapea un dedo largo (index, middle, ring, pinky) a PWM.

        Algoritmo:
          1. Calcular ángulo 3D en la articulación PIP.
          2. ``flexion = joint_angle_3d(landmarks, mcp, pip, tip)``
             — dedo recto ≈ 180°, flexionado ≈ 0°.
          3. ``raw = (180 - flexion) * sensitivity``
          4. Mapear raw (0–180) a PWM (open_pwm–closed_pwm).
        """
        mcp, pip, tip = _FINGER_PIP_LANDMARKS[finger_idx]

        flexion = joint_angle_3d(landmarks, mcp, pip, tip)

        # Normalizar: a mayor flexión (menor ángulo), mayor raw
        raw = (180.0 - flexion) * self.sensitivity * 1.5
        raw = max(0.0, min(180.0, raw))

        return lerp(raw, float(cfg["open_pwm"]), float(cfg["closed_pwm"]))
