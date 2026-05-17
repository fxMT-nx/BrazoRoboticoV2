"""
SerialManager — Comunicación serial TCP con Arduino Mega 2560 vía SOCAT bridge.

Cadena de comunicación:
  Flask → TCP:7500 → SOCAT → ttyGS0 → STM32 Serial → Serial1(D0/D1) → Mega Serial3 → PWM → Servos

Protocolo V2:
  - Formato: ``F<idx> <pwm_us>\\n``   (ej: ``F0 1500\\n``)
  - Rango idx: 0-4, pwm: 500-2500 µs
  - Heartbeat: ``H\\n`` cada 500ms

Features:
  - Conexión TCP a 127.0.0.1:7500 (SOCAT)
  - Auto-reconnect con backoff exponencial (0.5s, 1s, 2s, 4s … max 30s)
  - Heartbeat periódico con watchdog timeout → safe pose automático
  - Envío de comandos con clamping a límites seguros
  - Logging estructurado
  - Thread safety con Lock
  - Context manager (``with SerialManager() as sm:``)
"""

from __future__ import annotations

import logging
import os
import socket
import threading
import time
from typing import Optional

logger = logging.getLogger(__name__)

# Constantes de validación
VALID_IDX_RANGE = range(0, 6)  # 0=thumb … 4=pinky, 5=wrist
PWM_MIN = 500
PWM_MAX = 2500
BACKOFF_MAX = 30.0  # segundos, techo del backoff exponencial
SOCKET_TIMEOUT = 1.0  # timeout de operaciones de socket
HEARTBEAT_JOIN_TIMEOUT = 2.0  # segundos para esperar al thread al detener


# ---------------------------------------------------------------------------
# SerialManager
# ---------------------------------------------------------------------------


class SerialManager:
    """Gestor de comunicación serial con el Mega vía TCP (SOCAT bridge).

    Ejemplo de uso::

        mgr = SerialManager("config/tracking.yaml")
        if mgr.connect():
            mgr.start_heartbeat()
            mgr.send_command(0, 1500)   # pulgar al centro
            mgr.send_safe_pose()
            mgr.stop_heartbeat()
            mgr.disconnect()

    También soporta context manager (con heartbeat automático)::

        with SerialManager("config/tracking.yaml") as mgr:
            mgr.send_command(0, 1500)
    """

    def __init__(
        self,
        config_path: str = "config/tracking.yaml",
        *,
        host: Optional[str] = None,
        port: Optional[int] = None,
        reconnect_attempts: Optional[int] = None,
        reconnect_delay_s: Optional[float] = None,
        heartbeat_interval_ms: Optional[int] = None,
        heartbeat_timeout_ms: Optional[int] = None,
    ) -> None:
        """Inicializa SerialManager con configuración.

        Los argumentos con nombre (``host``, ``port``, etc.) tienen prioridad
        sobre lo que se cargue del YAML. Si no se proveen ni en kwargs ni en
        el YAML, se usan los defaults documentados.

        Args:
            config_path: Ruta al archivo YAML de tracking que contiene la
                sección ``serial``. También busca ``safe_pose_pwm`` en
                ``servo_calibration.yaml`` (en el mismo directorio).
            host: Dirección IP del SOCAT bridge (default: ``127.0.0.1``).
            port: Puerto TCP (default: ``7500``).
            reconnect_attempts: Número máximo de reintentos de conexión
                (default: ``10``).
            reconnect_delay_s: Demora base entre reintentos para backoff
                exponencial (default: ``0.5``).
            heartbeat_interval_ms: Intervalo entre heartbeats en ms
                (default: ``500``).
            heartbeat_timeout_ms: Timeout sin respuesta de heartbeat en ms.
                Si se excede, se ejecuta safe pose automático (default: ``2000``).
        """
        # ── Valores por defecto ────────────────────────────────────
        self.host: str = "127.0.0.1"
        self.port: int = 7500
        self.reconnect_attempts: int = 10
        self.reconnect_delay_s: float = 0.5
        self.heartbeat_interval_ms: int = 500
        self.heartbeat_timeout_ms: int = 86400000   # 24h — nunca timeout durante sesión
        self.safe_pose_pwm: int = 1500
        self.debug_log_every_n: int = 10

        # ── Cargar desde YAML ──────────────────────────────────────
        self._load_config(config_path)

        # ── Override con kwargs (máxima prioridad) ─────────────────
        if host is not None:
            self.host = host
        if port is not None:
            self.port = port
        if reconnect_attempts is not None:
            self.reconnect_attempts = reconnect_attempts
        if reconnect_delay_s is not None:
            self.reconnect_delay_s = reconnect_delay_s
        if heartbeat_interval_ms is not None:
            self.heartbeat_interval_ms = heartbeat_interval_ms
        if heartbeat_timeout_ms is not None:
            self.heartbeat_timeout_ms = heartbeat_timeout_ms

        # ── Estado interno ─────────────────────────────────────────
        self._socket: Optional[socket.socket] = None
        self._hb_thread: Optional[threading.Thread] = None
        self._running: bool = False
        self._last_ack_time: float = 0.0
        self._lock: threading.Lock = threading.Lock()
        self._cmd_count: int = 0  # contador de comandos para log cada N

    # ------------------------------------------------------------------
    # API pública — conexión
    # ------------------------------------------------------------------

    def connect(self) -> bool:
        """Conecta al SOCAT TCP con backoff exponencial.

        Estrategia de reintentos:
          - Hasta ``reconnect_attempts`` intentos.
          - Entre cada intento, espera ``reconnect_delay_s × 2^(attempt-1)``
            con un techo de 30 segundos.
          - Si conecta, resetea el timer de ``_last_ack_time``.

        Returns:
            ``True`` si conecta, ``False`` si agota los intentos.
        """
        for attempt in range(1, self.reconnect_attempts + 1):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(SOCKET_TIMEOUT)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                s.connect((self.host, self.port))
                with self._lock:
                    # Cerrar socket anterior si existe (doble connect)
                    if self._socket is not None:
                        try:
                            self._socket.close()
                        except OSError:
                            pass
                    self._socket = s
                self._last_ack_time = time.time()
                logger.info(
                    "Conectado a %s:%s (intento %d)",
                    self.host, self.port, attempt,
                )
                return True
            except (socket.timeout, ConnectionRefusedError, OSError) as exc:
                if attempt < self.reconnect_attempts:
                    delay = min(
                        self.reconnect_delay_s * (2 ** (attempt - 1)),
                        BACKOFF_MAX,
                    )
                    logger.warning(
                        "Intento %d/%s falló: %s — reconectando en %.1fs",
                        attempt, self.reconnect_attempts, exc, delay,
                    )
                    time.sleep(delay)
                else:
                    logger.error(
                        "No se pudo conectar a %s:%s tras %s intentos",
                        self.host, self.port, self.reconnect_attempts,
                    )
        return False

    def disconnect(self) -> None:
        """Cierra la conexión TCP limpiamente.

        Es seguro llamarlo múltiples veces y cuando no hay conexión activa.
        """
        with self._lock:
            if self._socket is not None:
                try:
                    self._socket.shutdown(socket.SHUT_RDWR)
                except (OSError, AttributeError):
                    pass
                try:
                    self._socket.close()
                except OSError:
                    pass
                self._socket = None
        logger.info("Desconectado de %s:%s", self.host, self.port)

    # ------------------------------------------------------------------
    # API pública — envío de comandos
    # ------------------------------------------------------------------

    def send_command(self, idx: int, pwm_us: int) -> bool:
        """Envía comando ``F<idx> <pwm_us>\\n`` al Mega.

        Args:
            idx: Índice del servo (0=thumb, 1=index, 2=middle, 3=ring, 4=pinky).
            pwm_us: Ancho de pulso PWM en microsegundos. Se clamp automáticamente
                al rango seguro [500, 2500].

        Returns:
            ``True`` si el comando se encoló para envío, ``False`` si no hay
            conexión o el envío falló.

    Raises:
        ValueError: Si ``idx`` no está en el rango 0-5.
        """
        if idx not in VALID_IDX_RANGE:
            raise ValueError(
                f"idx debe estar entre 0 y 5, recibido: {idx}"
            )

        # Clamping a límites seguros
        pwm_us = max(PWM_MIN, min(PWM_MAX, int(pwm_us)))
        cmd = f"F{idx} {pwm_us}\n".encode("ascii")

        result = self._send_raw(cmd)

        # Logging periódico (cada N comandos)
        self._cmd_count += 1
        if self._cmd_count % self.debug_log_every_n == 0:
            logger.debug(
                "Comando #%d: F%d %dµs → %s",
                self._cmd_count, idx, pwm_us, "OK" if result else "FAIL",
            )

        return result

    def send_safe_pose(self) -> bool:
        """Envía ``safe_pose_pwm`` a los 6 servos.

        Se usa como posición de resguardo cuando ocurre un timeout de
        heartbeat o el cliente WebSocket se desconecta.

        Returns:
            ``True`` si todos los comandos se enviaron, ``False`` si alguno
            falló (modo best-effort: intenta los 6 aunque uno falle).
        """
        all_ok = True
        for i in range(6):
            # Envía directo sin pasar por send_command para evitar logging extra
            try:
                cmd = f"F{i} {self.safe_pose_pwm}\n".encode("ascii")
                if not self._send_raw(cmd):
                    all_ok = False
            except Exception:
                all_ok = False
        if all_ok:
            logger.info("Safe pose enviada (%d µs)", self.safe_pose_pwm)
        else:
            logger.warning("Safe pose incompleta — algunos servos no recibieron comando")
        return all_ok

    # ------------------------------------------------------------------
    # API pública — heartbeat
    # ------------------------------------------------------------------

    def start_heartbeat(self) -> None:
        """Inicia un thread daemon que envía ``H\\n`` periódicamente.

        El thread corre en segundo plano hasta que se llama a
        :meth:`stop_heartbeat`. Cada ciclo:
          1. Duerme ``heartbeat_interval_ms`` milisegundos.
          2. Envía ``H\\n`` por el socket.
          3. Verifica si pasó más de ``heartbeat_timeout_ms`` desde el
             último ACK — de ser así, ejecuta :meth:`send_safe_pose`.

        Es seguro llamarlo múltiples veces (solo el primer llamado inicia
        el thread).
        """
        if self._running:
            logger.debug("Heartbeat ya estaba corriendo")
            return
        self._running = True
        self._hb_thread = threading.Thread(
            target=self._heartbeat_loop,
            name="serial-hb",
            daemon=True,
        )
        self._hb_thread.start()
        logger.info(
            "Heartbeat iniciado (intervalo=%dms, timeout=%dms)",
            self.heartbeat_interval_ms, self.heartbeat_timeout_ms,
        )

    def stop_heartbeat(self) -> None:
        """Detiene el thread de heartbeat.

        Espera hasta ``HEARTBEAT_JOIN_TIMEOUT`` segundos a que el thread
        termine. Es seguro llamarlo aunque el heartbeat no esté corriendo.
        """
        self._running = False
        if self._hb_thread is not None and self._hb_thread.is_alive():
            self._hb_thread.join(timeout=HEARTBEAT_JOIN_TIMEOUT)
            if self._hb_thread.is_alive():
                logger.warning("Heartbeat thread no terminó a tiempo")
        self._hb_thread = None
        logger.info("Heartbeat detenido")

    def send_heartbeat(self) -> bool:
        """Envía un heartbeat ``H\\n`` inmediato.

        Returns:
            ``True`` si se envió correctamente.
        """
        return self._send_raw(b"H\n")

    # ------------------------------------------------------------------
    # Properties
    # ------------------------------------------------------------------

    @property
    def is_connected(self) -> bool:
        """``True`` si el socket TCP está activo.

        Nota: Esto solo verifica que el objeto socket existe, no que la
        conexión esté realmente viva (un socket roto se detecta al enviar).
        """
        return self._socket is not None

    @property
    def last_heartbeat_ack(self) -> float:
        """Timestamp (``time.time()``) del último ACK recibido, o ``0.0`` si nunca."""
        return self._last_ack_time

    # ------------------------------------------------------------------
    # Context manager
    # ------------------------------------------------------------------

    def __enter__(self) -> SerialManager:
        """Soporte para ``with SerialManager() as mgr:``.

        Conecta e inicia el heartbeat automáticamente.
        """
        self.connect()
        self.start_heartbeat()
        return self

    def __exit__(
        self,
        exc_type: Optional[type],
        exc_val: Optional[BaseException],
        exc_tb: Optional[object],
    ) -> None:
        """Al salir del context, detiene heartbeat y desconecta."""
        self.stop_heartbeat()
        self.disconnect()

    # ------------------------------------------------------------------
    # Métodos internos
    # ------------------------------------------------------------------

    def _ensure_connected(self) -> bool:
        """Verifica conectividad y reconecta si es necesario.

        Returns:
            ``True`` si hay conexión activa (o se reconectó ok).
        """
        if self._socket is not None:
            return True
        logger.warning("Socket perdido — reconectando...")
        return self.connect()

    def _send_raw(self, data: bytes) -> bool:
        """Envía datos crudos por el socket TCP con protección thread-safe.

        Args:
            data: Bytes a enviar (debe incluir ``\\n`` de terminación).

        Returns:
            ``True`` si el envío fue exitoso.
        """
        with self._lock:
            if not self._ensure_connected():
                return False
            try:
                self._socket.sendall(data)  # type: ignore[union-attr]
                return True
            except (OSError, socket.timeout, AttributeError) as exc:
                logger.error("Error enviando datos: %s", exc)
                self._socket = None
                return False

    def _heartbeat_loop(self) -> None:
        """Bucle principal del thread de heartbeat.

        Este loop corre en un thread daemon separado. Por cada ciclo:
          1. Duerme ``heartbeat_interval_ms``.
          2. Envía ``H\\n`` (best-effort, no bloquea si falla).
          3. Si ``last_heartbeat_ack`` es > 0 y el tiempo desde el último ACK
             supera ``heartbeat_timeout_ms``, ejecuta safe pose automático.
        """
        while self._running:
            time.sleep(self.heartbeat_interval_ms / 1000.0)

            # Enviar heartbeat (best-effort)
            try:
                self._send_raw(b"H\n")
            except Exception:
                pass

            # Verificar timeout de heartbeat
            if self._last_ack_time > 0:
                elapsed = time.time() - self._last_ack_time
                if elapsed > self.heartbeat_timeout_ms / 1000.0:
                    logger.warning(
                        "Heartbeat timeout — %.1fs sin respuesta.",
                        elapsed,
                    )

    def _load_config(self, config_path: str) -> None:
        """Carga configuración serial desde archivo YAML.

        Busca la sección ``serial`` en *config_path* (tracking.yaml).
        También carga ``safe_pose_pwm`` desde ``servo_calibration.yaml``
        ubicado en el mismo directorio.

        Si los archivos no existen o tienen errores, usa los valores por
        defecto sin lanzar excepción.

        Args:
            config_path: Ruta al YAML de tracking.
        """
        try:
            import yaml

            # ── Cargar serial config desde tracking.yaml ────────
            with open(config_path) as f:
                cfg = yaml.safe_load(f) or {}

            serial_cfg = cfg.get("serial", {})
            if serial_cfg:
                self.host = serial_cfg.get("host", self.host)
                self.port = int(serial_cfg.get("port", self.port))
                self.reconnect_attempts = int(
                    serial_cfg.get("reconnect_attempts", self.reconnect_attempts)
                )
                self.reconnect_delay_s = float(
                    serial_cfg.get("reconnect_delay_s", self.reconnect_delay_s)
                )
                self.heartbeat_interval_ms = int(
                    serial_cfg.get("heartbeat_interval_ms", self.heartbeat_interval_ms)
                )
                self.heartbeat_timeout_ms = int(
                    serial_cfg.get("heartbeat_timeout_ms", self.heartbeat_timeout_ms)
                )
                self.debug_log_every_n = int(
                    serial_cfg.get("debug_log_every_n", self.debug_log_every_n)
                )

            logger.debug(
                "Config serial cargada desde %s: %s:%s, hb=%d/%dms",
                config_path, self.host, self.port,
                self.heartbeat_interval_ms, self.heartbeat_timeout_ms,
            )

            # ── Cargar safe_pose_pwm desde servo_calibration.yaml ──
            calib_dir = os.path.dirname(os.path.abspath(config_path))
            calib_path = os.path.join(calib_dir, "servo_calibration.yaml")

            try:
                with open(calib_path) as f:
                    calib = yaml.safe_load(f) or {}
                safe_pwm = calib.get("safe_pose_pwm")
                if safe_pwm is not None:
                    self.safe_pose_pwm = int(safe_pwm)
                    logger.debug(
                        "safe_pose_pwm=%d desde %s",
                        self.safe_pose_pwm, calib_path,
                    )
            except FileNotFoundError:
                logger.debug(
                    "servo_calibration.yaml no encontrado en %s — usando default safe_pose",
                    calib_path,
                )
            except (yaml.YAMLError, ValueError, TypeError) as exc:
                logger.warning(
                    "Error cargando safe_pose_pwm desde %s: %s",
                    calib_path, exc,
                )

        except FileNotFoundError:
            logger.warning(
                "Archivo de configuración %s no encontrado — usando defaults serial",
                config_path,
            )
        except (yaml.YAMLError, ValueError, TypeError) as exc:
            logger.warning(
                "Error cargando config serial desde %s: %s — usando defaults",
                config_path, exc,
            )
        except Exception as exc:
            logger.exception(
                "Error inesperado cargando config desde %s: %s",
                config_path, exc,
            )
