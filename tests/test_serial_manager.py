"""
Tests para SerialManager — Comunicación serial TCP con Mega 2560 vía SOCAT.

Estrategia de testing:
  - Usamos pytest mocks para simular el socket TCP (evita depender de hardware).
  - Probamos connect/disconnect, send_command, heartbeat, safe_pose,
    backoff exponencial, timeout watchdog, y thread safety.
  - También probamos la carga de configuración desde YAML (tracking + servo_calibration).
"""

from __future__ import annotations

import json
import socket
import threading
import time
from pathlib import Path
from unittest.mock import MagicMock, patch, call

import pytest

from backend.serial_manager import SerialManager


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def mock_socket() -> MagicMock:
    """Mock de socket.socket para simular la conexión TCP."""
    sock = MagicMock(spec=socket.socket)
    sock.settimeout.return_value = None
    return sock


@pytest.fixture
def manager(mock_socket: MagicMock) -> SerialManager:
    """SerialManager con socket mockeado, conectado y heartbeat iniciado."""
    with patch("backend.serial_manager.socket.socket", return_value=mock_socket):
        mgr = SerialManager()
        mgr.connect()
        mgr.start_heartbeat()
        yield mgr
        mgr.stop_heartbeat()
        mgr.disconnect()


# ---------------------------------------------------------------------------
# Tests de inicialización y configuración
# ---------------------------------------------------------------------------

class TestInit:
    """Verifica que el constructor aplique defaults y carga de YAML."""

    def test_default_values(self):
        """Sin config, debe usar valores por defecto documentados."""
        mgr = SerialManager()
        assert mgr.host == "127.0.0.1"
        assert mgr.port == 7500
        assert mgr.reconnect_attempts == 10
        assert mgr.reconnect_delay_s == 0.5
        assert mgr.heartbeat_interval_ms == 500
        assert mgr.heartbeat_timeout_ms == 2000
        assert mgr.safe_pose_pwm == 1500
        assert mgr.is_connected is False
        assert mgr.last_heartbeat_ack == 0.0

    def test_config_from_yaml(self, tmp_path: Path):
        """Debe cargar serial config desde tracking.yaml."""
        config_file = tmp_path / "tracking.yaml"
        config_file.write_text(json.dumps({
            "serial": {
                "host": "10.0.0.1",
                "port": 9999,
                "reconnect_attempts": 5,
                "reconnect_delay_s": 2.0,
                "heartbeat_interval_ms": 1000,
                "heartbeat_timeout_ms": 5000,
                "debug_log_every_n": 5,
            }
        }))

        mgr = SerialManager(config_path=str(config_file))
        assert mgr.host == "10.0.0.1"
        assert mgr.port == 9999
        assert mgr.reconnect_attempts == 5
        assert mgr.reconnect_delay_s == 2.0
        assert mgr.heartbeat_interval_ms == 1000
        assert mgr.heartbeat_timeout_ms == 5000

    def test_config_file_not_found(self):
        """Si el YAML no existe, debe usar defaults sin error."""
        mgr = SerialManager(config_path="/no/existe.yaml")
        assert mgr.host == "127.0.0.1"
        assert mgr.port == 7500

    def test_config_empty_yaml(self, tmp_path: Path):
        """YAML vacío → defaults."""
        config_file = tmp_path / "empty.yaml"
        config_file.write_text("")
        mgr = SerialManager(config_path=str(config_file))
        assert mgr.host == "127.0.0.1"

    def test_safe_pose_from_servo_calibration(self, tmp_path: Path):
        """safe_pose_pwm debe cargarse desde servo_calibration.yaml."""
        tracking = tmp_path / "tracking.yaml"
        tracking.write_text("serial:\n  host: 127.0.0.1\n  port: 7500\n")

        calib = tmp_path / "servo_calibration.yaml"
        calib.write_text("safe_pose_pwm: 1800\n")

        with patch("backend.serial_manager.os.path.dirname",
                   return_value=str(tmp_path)):
            mgr = SerialManager(config_path=str(tracking))
            assert mgr.safe_pose_pwm == 1800

    def test_invalid_idx_raises(self):
        """send_command debe validar índice 0-4."""
        mgr = SerialManager()
        with pytest.raises(ValueError, match="idx debe estar entre 0 y 4"):
            mgr.send_command(5, 1500)
        with pytest.raises(ValueError, match="idx debe estar entre 0 y 4"):
            mgr.send_command(-1, 1500)

    def test_pwm_clamping(self, mock_socket: MagicMock):
        """PWM debe clamp a [500, 2500] — verificar que no se envían valores fuera de rango."""
        with patch("backend.serial_manager.socket.socket", return_value=mock_socket):
            mgr = SerialManager()
            mgr.connect()
            mgr.send_command(0, -100)   # → 500
            mgr.send_command(0, 3000)   # → 2500
            mgr.send_command(0, 100)    # → 500
            expected_calls = [
                call(b"F0 500\n"),
                call(b"F0 2500\n"),
                call(b"F0 500\n"),
            ]
            mock_socket.sendall.assert_has_calls(expected_calls, any_order=False)
            mgr.disconnect()

    # ------------------------------------------------------------------
    # Tests de conexión
    # ------------------------------------------------------------------

    class TestConnect:
        def test_connect_success(self, mock_socket: MagicMock):
            """Conexión exitosa en primer intento."""
            with patch("backend.serial_manager.socket.socket", return_value=mock_socket):
                mgr = SerialManager(reconnect_attempts=3)
                result = mgr.connect()
                assert result is True
                assert mgr.is_connected is True
                mock_socket.connect.assert_called_once_with(("127.0.0.1", 7500))
                mgr.disconnect()

        def test_connect_retry_then_success(self, mock_socket: MagicMock):
            """Falla 2 veces, luego conecta."""
            mock_socket.connect.side_effect = [
                ConnectionRefusedError,
                ConnectionRefusedError,
                None,
            ]
            with patch("backend.serial_manager.socket.socket", return_value=mock_socket):
                mgr = SerialManager(reconnect_attempts=5, reconnect_delay_s=0.01)
                result = mgr.connect()
                assert result is True
                assert mock_socket.connect.call_count == 3
                mgr.disconnect()

        def test_connect_all_attempts_fail(self, mock_socket: MagicMock):
            """Agota todos los intentos y retorna False."""
            mock_socket.connect.side_effect = ConnectionRefusedError
            with patch("backend.serial_manager.socket.socket", return_value=mock_socket):
                mgr = SerialManager(reconnect_attempts=3, reconnect_delay_s=0.01)
                result = mgr.connect()
                assert result is False
                assert mgr.is_connected is False
                assert mock_socket.connect.call_count == 3

        def test_connect_backoff_delay(self, mock_socket: MagicMock):
            """Los reintentos deben usar backoff exponencial (0.5, 1, 2, 4... max 30)."""
            import math
            mock_socket.connect.side_effect = ConnectionRefusedError
            sleeps = []

            original_sleep = time.sleep

            def tracking_sleep(delay):
                sleeps.append(delay)
                return original_sleep(delay * 0.001)  # acelerar 1000x

            with patch("backend.serial_manager.socket.socket", return_value=mock_socket), \
                    patch("backend.serial_manager.time.sleep", side_effect=tracking_sleep):
                mgr = SerialManager(reconnect_attempts=6, reconnect_delay_s=0.5)
                mgr.connect()
                # Esperado: 0.5, 1, 2, 4, 8 (intento 1-5), attempt 6 no duerme
                assert len(sleeps) >= 5
                expected = [0.5, 1.0, 2.0, 4.0, 8.0]
                for i, (actual, exp) in enumerate(zip(sleeps[:5], expected)):
                    assert actual == pytest.approx(exp, rel=0.1), \
                        f"Sleep {i}: esperado {exp}s, obtuviste {actual}s"

    # ------------------------------------------------------------------
    # Tests de envío de comandos
    # ------------------------------------------------------------------

    class TestSendCommand:
        def test_send_command_format(self, mock_socket: MagicMock, manager: SerialManager):
            """El comando F<idx> <pwm>\\n debe enviarse exactamente."""
            manager.send_command(0, 1500)
            manager._socket.sendall.assert_called_with(b"F0 1500\n")

        def test_send_command_all_indices(self, mock_socket: MagicMock,
                                          manager: SerialManager):
            """Prueba índices 0-4 con valores variados."""
            commands = [(0, 1000), (1, 1500), (2, 2000), (3, 1800), (4, 1200)]
            for idx, pwm in commands:
                manager.send_command(idx, pwm)
            expected_calls = [
                call(b"F0 1000\n"),
                call(b"F1 1500\n"),
                call(b"F2 2000\n"),
                call(b"F3 1800\n"),
                call(b"F4 1200\n"),
            ]
            manager._socket.sendall.assert_has_calls(expected_calls, any_order=False)

        def test_pwm_clamped_to_500_2500(self, mock_socket: MagicMock,
                                         manager: SerialManager):
            """PWM debe recortarse a [500, 2500]."""
            manager.send_command(0, -100)   # → 500
            manager.send_command(0, 3000)   # → 2500
            manager.send_command(0, 100)    # → 500
            expected_calls = [
                call(b"F0 500\n"),
                call(b"F0 2500\n"),
                call(b"F0 500\n"),
            ]
            manager._socket.sendall.assert_has_calls(expected_calls, any_order=False)

        def test_return_false_on_disconnected(self):
            """Sin conexión, send_command debe retornar False."""
            # reconnect_attempts=0 para que falle instantáneo sin intentar TCP real
            mgr = SerialManager(reconnect_attempts=0, reconnect_delay_s=0.01)
            assert mgr.send_command(0, 1500) is False

        def test_reconnect_on_socket_error(self, mock_socket: MagicMock):
            """Si el socket falla al enviar, debe reconectar."""
            mock_socket.sendall.side_effect = BrokenPipeError("Broken pipe")
            with patch("backend.serial_manager.socket.socket", return_value=mock_socket):
                mgr = SerialManager(reconnect_attempts=2, reconnect_delay_s=0.01)
                mgr.connect()  # conecta ok
                mgr._socket = mock_socket  # asegura que use nuestro mock
                mgr._socket.sendall.side_effect = BrokenPipeError  # falla envío
                # Primera vez falla, no reconecta (solo 1 intento porque reconnect_attempts ya se usó)
                result = mgr.send_command(0, 1500)
                assert result is False

    # ------------------------------------------------------------------
    # Tests de Heartbeat
    # ------------------------------------------------------------------

    class TestHeartbeat:
        def test_heartbeat_sends_h(self, mock_socket: MagicMock, manager: SerialManager):
            """El heartbeat debe enviar H\\n periódicamente."""
            time.sleep(1.1)  # ~2 heartbeats con intervalo de 500ms
            manager._socket.sendall.assert_any_call(b"H\n")

        def test_heartbeat_stops(self, mock_socket: MagicMock):
            """stop_heartbeat debe detener el thread."""
            with patch("backend.serial_manager.socket.socket", return_value=mock_socket):
                mgr = SerialManager()
                mgr.connect()
                mgr.start_heartbeat()
                time.sleep(0.6)
                assert mgr._running is True
                mgr.stop_heartbeat()
                assert mgr._running is False

        def test_heartbeat_timeout_triggers_safe_pose(self, mock_socket: MagicMock):
            """Si no hay ACK en heartbeat_timeout_ms, ejecuta safe pose."""
            with patch("backend.serial_manager.socket.socket", return_value=mock_socket):
                mgr = SerialManager(heartbeat_interval_ms=100, heartbeat_timeout_ms=300)
                mgr.connect()
                # No enviamos ACK — _last_ack_time queda en 0
                # Pero queremos que se active el timeout.
                # Forzamos _last_ack_time a un valor viejo
                mgr._last_ack_time = time.time() - 10  # 10s atrás

                mgr.start_heartbeat()
                time.sleep(0.8)  # suficientes ciclos para timeout
                mgr.stop_heartbeat()

                # Debe haber ejecutado safe_pose (F0-F4 1500)
                expected_safe_calls = [
                    call(b"F0 1500\n"),
                    call(b"F1 1500\n"),
                    call(b"F2 1500\n"),
                    call(b"F3 1500\n"),
                    call(b"F4 1500\n"),
                ]
                # Algunos de esos calls pueden ser heartbeats — buscamos los safe pose
                all_calls = mgr._socket.sendall.call_args_list
                for expected in expected_safe_calls:
                    assert expected in all_calls, f"Falta safe pose call: {expected}"

    # ------------------------------------------------------------------
    # Tests de Safe Pose
    # ------------------------------------------------------------------

    class TestSafePose:
        def test_send_safe_pose_all_five(self, mock_socket: MagicMock,
                                         manager: SerialManager):
            """send_safe_pose debe enviar F0..F4 con safe_pose_pwm."""
            result = manager.send_safe_pose()
            assert result is True
            expected = [
                call(b"F0 1500\n"),
                call(b"F1 1500\n"),
                call(b"F2 1500\n"),
                call(b"F3 1500\n"),
                call(b"F4 1500\n"),
            ]
            # Los últimos 5 calls deben ser safe pose (pueden haber heartbeats antes)
            actual_calls = manager._socket.sendall.call_args_list
            for exp in expected:
                assert exp in actual_calls, f"Falta: {exp}"

        def test_safe_pose_custom_pwm(self, mock_socket: MagicMock):
            """safe_pose con pwm distinto."""
            with patch("backend.serial_manager.socket.socket", return_value=mock_socket):
                mgr = SerialManager()
                mgr.safe_pose_pwm = 1800
                mgr.connect()
                mgr.send_safe_pose()
                mgr._socket.sendall.assert_any_call(b"F0 1800\n")

        def test_safe_pose_no_connection(self):
            """Sin conexión, safe_pose retorna False."""
            mgr = SerialManager(reconnect_attempts=0, reconnect_delay_s=0.01)
            result = mgr.send_safe_pose()
            assert result is False

    # ------------------------------------------------------------------
    # Tests de reconexión automática
    # ------------------------------------------------------------------

    class TestAutoReconnect:
        def test_reconnect_after_disconnect(self, mock_socket: MagicMock):
            """_ensure_connected debe reconectar si el socket se perdió."""
            mock_socket.connect.side_effect = [None, None]  # 2 conexiones exitosas
            # Segundo socket para la reconexión
            mock_socket2 = MagicMock(spec=socket.socket)

            with patch("backend.serial_manager.socket.socket",
                       side_effect=[mock_socket, mock_socket2]):
                mgr = SerialManager(reconnect_attempts=3, reconnect_delay_s=0.01)
                mgr.connect()
                old_sock = mgr._socket
                mgr._socket = None  # simulamos pérdida de conexión
                mgr._ensure_connected()
                assert mgr._socket is not None
                # La reconexión debería usar un socket nuevo
                assert mgr._socket is mock_socket2

    # ------------------------------------------------------------------
    # Tests de thread safety
    # ------------------------------------------------------------------

    class TestThreadSafety:
        def test_concurrent_send_does_not_crash(self, mock_socket: MagicMock,
                                                manager: SerialManager):
            """Múltiples hilos enviando comandos no deben causar errores."""
            errors = []

            def send_many():
                try:
                    for i in range(20):
                        manager.send_command(i % 5, 1500 + i * 10)
                        time.sleep(0.001)
                except Exception as e:
                    errors.append(e)

            threads = [threading.Thread(target=send_many) for _ in range(5)]
            for t in threads:
                t.start()
            for t in threads:
                t.join(timeout=5)

            assert len(errors) == 0, f"Errores en threads: {errors}"

    # ------------------------------------------------------------------
    # Tests de context manager
    # ------------------------------------------------------------------

    class TestContextManager:
        def test_context_manager_connect_disconnect(self, mock_socket: MagicMock):
            """Usar 'with SerialManager()' debe conectar y desconectar."""
            with patch("backend.serial_manager.socket.socket", return_value=mock_socket):
                mgr = SerialManager()
                with mgr:
                    assert mgr.is_connected is True
                    mgr.send_command(0, 1500)
                assert mgr.is_connected is False

        def test_context_manager_heartbeat(self, mock_socket: MagicMock):
            """El context manager debe gestionar heartbeat automáticamente."""
            with patch("backend.serial_manager.socket.socket", return_value=mock_socket):
                mgr = SerialManager()
                with mgr:
                    assert mgr._running is True  # heartbeat iniciado
                assert mgr._running is False  # heartbeat detenido

    # ------------------------------------------------------------------
    # Tests de edge cases
    # ------------------------------------------------------------------

    class TestEdgeCases:
        def test_disconnect_when_already_disconnected(self):
            """Disconnect sin conexión no debe lanzar error."""
            mgr = SerialManager()
            mgr.disconnect()  # no debe fallar

        def test_double_connect(self, mock_socket: MagicMock):
            """Conectar dos veces debe cerrar socket anterior."""
            with patch("backend.serial_manager.socket.socket", return_value=mock_socket):
                mgr = SerialManager()
                mgr.connect()
                first_sock = mgr._socket
                mgr.connect()
                assert mgr._socket is not None
                # socket anterior debe haberse cerrado
                first_sock.close.assert_called_once()

        def test_heartbeat_before_connect(self):
            """start_heartbeat sin conexión no debe petar."""
            mgr = SerialManager()
            mgr.start_heartbeat()  # no debe fallar
            time.sleep(0.6)
            mgr.stop_heartbeat()

        def test_last_heartbeat_ack_initially_zero(self):
            """last_heartbeat_ack debe ser 0.0 antes de cualquier ACK."""
            mgr = SerialManager()
            assert mgr.last_heartbeat_ack == 0.0
