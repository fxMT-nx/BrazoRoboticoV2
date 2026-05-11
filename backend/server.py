"""
BrazoRoboticoV2 — Servidor Flask + WebSocket
Arduino UNO Q (Qualcomm Linux)

Orquestador central del sistema:
  1. Sirve el frontend React (build estático en static/)
  2. Recibe landmarks por WebSocket desde el navegador
  3. Los pasa al PoseMapper para convertirlos a PWM
  4. Envía los comandos PWM al Mega via SerialManager (TCP → SOCAT → STM32 → Mega)
  5. Soporta Record/Replay (desde frontend)
  6. Healthcheck + Config endpoints

IP UNO Q: 192.168.31.12
Puerto: 3000 (HTTPS)
"""

from __future__ import annotations

import json
import logging
import os
import time

from flask import Flask, jsonify, send_from_directory
from flask_sock import Sock

from pose_mapper import PoseMapper

# ── Import protegido de SerialManager ──────────────────────────────────
# El servidor debe arrancar aunque serial_manager.py no exista (modo offline).
_serial_manager_available = False
try:
    from serial_manager import SerialManager
    _serial_manager_available = True
except ImportError:
    SerialManager = None  # type: ignore[assignment]

# ── Logging estructurado ──────────────────────────────────────────────

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger(__name__)

# ── App Flask ─────────────────────────────────────────────────────────

app = Flask(__name__, static_folder="static", static_url_path="/static")
sock = Sock(app)

# ── Constantes / Rutas de configuración ───────────────────────────────

CONFIG_PATHS = {
    "calibration": "config/servo_calibration.yaml",
    "tracking": "config/tracking.yaml",
    "network": "config/network.yaml",
}

# ── Estado global ─────────────────────────────────────────────────────

pose_mapper: PoseMapper | None = None
serial_mgr: SerialManager | None = None

tracking_active = False
current_angles: list[int] = [1500, 1500, 1500, 1500, 1500, 1500]
frame_count = 0

# ── Inicialización ────────────────────────────────────────────────────


def init_system() -> None:
    """Inicializa PoseMapper y SerialManager.

    El servidor arranca en modo offline si SerialManager no está disponible
    o no puede conectar. PoseMapper siempre se inicializa (con fallback a
    defaults si falta YAML).
    """
    global pose_mapper, serial_mgr

    # ── PoseMapper ────────────────────────────────────────────────
    try:
        pose_mapper = PoseMapper(
            calibration_path=CONFIG_PATHS["calibration"],
            tracking_path=CONFIG_PATHS["tracking"],
        )
        logger.info("PoseMapper inicializado desde calibración+tracking")
    except Exception as exc:
        logger.error("Error iniciando PoseMapper: %s", exc)
        pose_mapper = PoseMapper()  # fallback a defaults
        logger.info("PoseMapper inicializado con valores por defecto")

    # ── SerialManager ─────────────────────────────────────────────
    if not _serial_manager_available:
        logger.warning(
            "SerialManager no disponible (archivo no encontrado). "
            "Modo offline permanente."
        )
        serial_mgr = None  # type: ignore[assignment]
        return

    try:
        serial_mgr = SerialManager(CONFIG_PATHS["tracking"])
        if serial_mgr.connect():
            serial_mgr.start_heartbeat()
            logger.info("SerialManager conectado y heartbeat activo")
        else:
            logger.warning("SerialManager no pudo conectar — modo offline")
    except Exception as exc:
        logger.error("Error iniciando SerialManager: %s", exc)
        serial_mgr = None  # type: ignore[assignment]

    logger.info("Sistema inicializado")


# ── Utilidades ────────────────────────────────────────────────────────


def _send_safe_pose() -> None:
    """Envía safe pose a todos los servos si SerialManager está conectado."""
    global serial_mgr
    if serial_mgr is not None and hasattr(serial_mgr, "is_connected") and serial_mgr.is_connected:
        try:
            serial_mgr.send_safe_pose()
        except Exception as exc:
            logger.error("Error enviando safe pose: %s", exc)


# ── Rutas HTTP ────────────────────────────────────────────────────────


@app.route("/")
def index():
    """Sirve el frontend React."""
    return send_from_directory("static", "index.html")


@app.route("/api/health")
def health():
    """Healthcheck endpoint — estado del sistema en vivo."""
    return jsonify(
        {
            "status": "ok",
            "connected": serial_mgr.is_connected
            if serial_mgr is not None and hasattr(serial_mgr, "is_connected")
            else False,
            "tracking": tracking_active,
            "angles": current_angles,
            "frame_count": frame_count,
            "uptime_s": round(time.time() - app.start_time, 2),
        }
    )


@app.route("/api/tunnel")
def tunnel():
    """Devuelve la URL del Cloudflare Tunnel activo."""
    try:
        with open('/tmp/quick-tunnel-url.txt') as f:
            url = f.read().strip()
        return jsonify({'url': url, 'found': True})
    except Exception:
        return jsonify({'url': None, 'found': False})


def _get_real_ip() -> str:
    """Obtiene la IP real del servidor (no loopback, no docker).

    Orden:
      1. `ip -4 addr show` filtrando loopback y docker
      2. Fallback a network.yaml (client_wifi.ip)
      3. Fallback final 0.0.0.0
    """
    import subprocess
    try:
        result = subprocess.run(
            ['ip', '-4', 'addr', 'show'],
            capture_output=True, text=True, timeout=5
        )
        for line in result.stdout.split('\n'):
            if 'inet ' in line:
                parts = line.strip().split()
                if len(parts) >= 2:
                    ip = parts[1].split('/')[0]
                    # Saltar loopback y docker bridge (172.x.x.x)
                    if not ip.startswith('127.') and not ip.startswith('172.'):
                        return ip
        # Fallback: primera IP no-loopback (ej. solo docker)
        for line in result.stdout.split('\n'):
            if 'inet ' in line:
                parts = line.strip().split()
                if len(parts) >= 2:
                    ip = parts[1].split('/')[0]
                    if not ip.startswith('127.'):
                        return ip
    except Exception:
        pass
    # Fallback: leer de network.yaml
    try:
        import yaml
        with open(CONFIG_PATHS["network"]) as f:
            cfg = yaml.safe_load(f) or {}
        ip = cfg.get('client_wifi', {}).get('ip', '0.0.0.0')
        if ip:
            return ip
    except Exception:
        pass
    return '0.0.0.0'


@app.route("/api/network")
def network():
    """Devuelve información de red del servidor.

    Obtiene la IP real de la interfaz de red (wlan0/eth0),
    leyendo directamente del sistema. No usa gethostbyname
    porque puede devolver 127.0.0.1 en entornos con hostname
    no resoluble.
    """
    import socket
    hostname = socket.gethostname()
    ip = _get_real_ip()
    return jsonify({
        'hostname': hostname,
        'ip': ip,
        'port': 3000,
        'url_local': f'https://{ip}:3000',
    })


@app.route("/api/config")
def get_config():
    """Devuelve la configuración actual (solo lectura)."""
    import yaml

    configs: dict[str, object] = {}
    for name, path in CONFIG_PATHS.items():
        try:
            with open(path) as f:
                configs[name] = yaml.safe_load(f)
        except FileNotFoundError:
            configs[name] = {"error": f"Archivo no encontrado: {path}"}
        except yaml.YAMLError as exc:
            configs[name] = {"error": f"Error YAML en {path}: {exc}"}
        except Exception as exc:
            configs[name] = {"error": f"Error leyendo {path}: {exc}"}

    return jsonify(configs)


# ── WebSocket Handler ─────────────────────────────────────────────────


@sock.route("/ws")
def ws_handler(ws):
    """Maneja conexiones WebSocket desde el frontend.

    Pipeline por frame:
      1. Recibe landmarks desde el navegador
      2. PoseMapper: landmarks → 6 PWM values
      3. SerialManager: envía cada PWM al Mega
      4. Responde al frontend con los ángulos calculados

    En caso de error → safe pose.
    Al desconectar cliente → safe pose.
    """
    global tracking_active, current_angles, frame_count

    logger.info("Cliente WebSocket conectado")
    tracking_active = True

    # ── Reintentar conexión serial si está caída ────────────────
    if (
        serial_mgr is not None
        and hasattr(serial_mgr, "is_connected")
        and not serial_mgr.is_connected
    ):
        logger.info("Reintentando conexión serial...")
        try:
            if serial_mgr.connect():
                serial_mgr.start_heartbeat()
                logger.info("SerialManager reconectado")
        except Exception as exc:
            logger.warning("No se pudo reconectar SerialManager: %s", exc)

    try:
        while True:
            message = ws.receive()
            if message is None:
                # Cliente cerró la conexión limpiamente
                break

            # ── Parsear mensaje ──────────────────────────────────
            try:
                data = json.loads(message)
            except json.JSONDecodeError as exc:
                logger.warning("JSON inválido desde WebSocket: %s", exc)
                continue

            landmarks = data.get("landmarks")
            wrist_angle = data.get("wrist_angle")
            handedness = data.get("handedness")  # ← NUEVO: 'Left' o 'Right' o None

            if not landmarks:
                continue

            # ── Procesar frame ───────────────────────────────
            try:
                # Verificar que pose_mapper esté disponible
                if pose_mapper is None:
                    raise RuntimeError("PoseMapper no inicializado")

                # Pasar wrist_angle al PoseMapper
                pwm_values = pose_mapper.landmarks_to_pwm(
                    landmarks, wrist_angle=wrist_angle, handedness=handedness
                )
                current_angles = pwm_values

                # Enviar comandos al Mega (uno por servo)
                if (
                    serial_mgr is not None
                    and hasattr(serial_mgr, "is_connected")
                    and serial_mgr.is_connected
                ):
                    for i, pwm in enumerate(pwm_values):
                        serial_mgr.send_command(i, pwm)

                # Responder al frontend
                ws.send(
                    json.dumps(
                        {
                            "angles": pwm_values,
                            "timestamp": data.get("timestamp", time.time()),
                        }
                    )
                )

                frame_count += 1

            except Exception as exc:
                logger.error("Error procesando frame: %s", exc, exc_info=True)
                _send_safe_pose()
                ws.send(
                    json.dumps(
                        {
                            "angles": [1500, 1500, 1500, 1500, 1500, 1500],
                            "error": str(exc),
                            "timestamp": data.get("timestamp", time.time()),
                        }
                    )
                )

    except Exception as exc:
        logger.error("Error en bucle WebSocket: %s", exc, exc_info=True)

    finally:
        tracking_active = False
        logger.info("Cliente WebSocket desconectado")
        _send_safe_pose()


# ── Main ──────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import yaml

    app.start_time = time.time()
    init_system()

    # ── Enviar IP local al STM32 (LED Matrix) ──────────────────
    try:
        local_ip = _get_real_ip()
        if serial_mgr and serial_mgr.is_connected:
            cmd = f"I{local_ip}:3000\n"
            serial_mgr._send_raw(cmd.encode())
            logger.info("IP enviada al STM32: %s:3000", local_ip)
    except Exception as e:
        logger.warning("No se pudo enviar IP al STM32: %s", e)

    # ── Cargar configuración de red ──────────────────────────
    try:
        with open(CONFIG_PATHS["network"]) as f:
            net_cfg = yaml.safe_load(f) or {}
    except Exception as exc:
        logger.warning("No se pudo cargar network config: %s", exc)
        net_cfg = {}

    server_cfg = net_cfg.get("server", {})
    host = server_cfg.get("host", "0.0.0.0")
    port = server_cfg.get("port", 3000)
    ssl_cert = server_cfg.get("ssl_cert", "cert.pem")
    ssl_key = server_cfg.get("ssl_key", "key.pem")
    debug = server_cfg.get("debug", False)

    logger.info("Arrancando BrazoRoboticoV2 en %s:%s", host, port)

    # ── Verificar certificados SSL ───────────────────────────
    if os.path.exists(ssl_cert) and os.path.exists(ssl_key):
        logger.info("Modo HTTPS con certificados (%s, %s)", ssl_cert, ssl_key)
        app.run(
            host=host,
            port=port,
            ssl_context=(ssl_cert, ssl_key),
            debug=debug,
        )
    else:
        logger.warning(
            "Certificados SSL no encontrados (%s, %s) — usando HTTP",
            ssl_cert,
            ssl_key,
        )
        logger.warning(
            "Para generar: openssl req -x509 -newkey rsa:4096 "
            "-keyout key.pem -out cert.pem -days 365 -nodes"
        )
        app.run(host=host, port=port, debug=debug)
