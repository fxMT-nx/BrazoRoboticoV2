#!/usr/bin/env bash
# =============================================================================
# Configura el UNO Q como Punto de Acceso WiFi
#
# Convierte el UNO Q en un AP WiFi al que conectarse directamente.
# Útil cuando no hay red WiFi doméstica disponible (campo, transporte,
# demostraciones).
#
# SSID:     RobotHand
# Pass:     robot2026
# IP AP:    192.168.4.1
# DHCP:     192.168.4.10 — 192.168.4.100
#
# Uso:
#   sudo ./setup.sh                    # Iniciar AP
#   sudo ./setup.sh --stop             # Detener AP y restaurar cliente
#   sudo ./setup.sh --status           # Ver estado del AP
# =============================================================================

set -euo pipefail

# --- Constantes ---------------------------------------------------------------
SSID="${WIFI_SSID:-RobotHand}"
PASS="${WIFI_PASS:-robot2026}"
CHANNEL="${WIFI_CHANNEL:-6}"
AP_IFACE="${WIFI_AP_IFACE:-wlan0}"
AP_IP="192.168.4.1"
DHCP_RANGE_START="192.168.4.10"
DHCP_RANGE_END="192.168.4.100"
DHCP_NETMASK="255.255.255.0"
DHCP_LEASE_TIME="12h"

HOSTAPD_CONF="/etc/hostapd/hostapd.conf"
DNSMASQ_CONF="/etc/dnsmasq.d/robot-hand.conf"

# --- Funciones ----------------------------------------------------------------

usage() {
    echo "Uso: $0 [--stop|--status|--help]"
    echo ""
    echo "  (sin argumentos)  Inicia el Punto de Acceso WiFi"
    echo "  --stop            Detiene el AP y limpia la configuración"
    echo "  --status          Muestra el estado actual del AP"
    echo "  --help            Esta ayuda"
    exit 0
}

check_root() {
    if [[ $EUID -ne 0 ]]; then
        echo "[ERROR] Este script debe ejecutarse como root (sudo)."
        exit 1
    fi
}

check_packages() {
    local missing=()
    for pkg in hostapd dnsmasq iw; do
        if ! dpkg -l "$pkg" &>/dev/null 2>&1; then
            missing+=("$pkg")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "[INFO] Instalando paquetes faltantes: ${missing[*]}..."
        apt-get update -qq
        apt-get install -y -qq "${missing[@]}"
        echo "[OK] Paquetes instalados."
    else
        echo "[OK] Todos los paquetes necesarios están instalados."
    fi
}

stop_conflicting_services() {
    echo "[INFO] Deteniendo servicios que puedan interferir..."

    # Detener NetworkManager si está gestionando la interfaz
    if systemctl is-active NetworkManager &>/dev/null; then
        nmcli device set "$AP_IFACE" managed no 2>/dev/null || true
    fi

    # Matar procesos existentes de hostapd y dnsmasq
    systemctl stop hostapd 2>/dev/null || true
    systemctl stop dnsmasq 2>/dev/null || true
    killall hostapd 2>/dev/null || true
    killall dnsmasq 2>/dev/null || true
    sleep 1

    echo "[OK] Servicios detenidos."
}

configure_interface() {
    echo "[INFO] Configurando interfaz $AP_IFACE..."

    # Asignar IP estática al AP
    ip addr flush dev "$AP_IFACE" 2>/dev/null || true
    ip addr add "$AP_IP/24" dev "$AP_IFACE"
    ip link set "$AP_IFACE" up

    echo "[OK] Interfaz $AP_IFACE configurada con IP $AP_IP."
}

write_hostapd_conf() {
    echo "[INFO] Escribiendo configuración de hostapd..."

    cat > "$HOSTAPD_CONF" <<EOF
# Configuración generada por setup.sh — BrazoRoboticoV2
interface=$AP_IFACE
driver=nl80211
ssid=$SSID
hw_mode=g
channel=$CHANNEL
wmm_enabled=1
macaddr_acl=0
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=$PASS
wpa_key_mgmt=WPA-PSK
wpa_pairwise=TKIP
rsn_pairwise=CCMP
EOF

    chmod 600 "$HOSTAPD_CONF"
    echo "[OK] hostapd configurado en $HOSTAPD_CONF."
}

write_dnsmasq_conf() {
    echo "[INFO] Escribiendo configuración de dnsmasq..."

    mkdir -p /etc/dnsmasq.d/
    cat > "$DNSMASQ_CONF" <<EOF
# Configuración generada por setup.sh — BrazoRoboticoV2
interface=$AP_IFACE
dhcp-range=$DHCP_RANGE_START,$DHCP_RANGE_END,$DHCP_NETMASK,$DHCP_LEASE_TIME
dhcp-option=3,$AP_IP
dhcp-option=6,$AP_IP
server=8.8.8.8
server=8.8.4.4
no-resolv
log-queries
log-dhcp
EOF

    echo "[OK] dnsmasq configurado en $DNSMASQ_CONF."
}

start_services() {
    echo "[INFO] Iniciando hostapd..."
    hostapd -B "$HOSTAPD_CONF" 2>&1
    sleep 2

    echo "[INFO] Iniciando dnsmasq..."
    dnsmasq -C "$DNSMASQ_CONF" 2>&1 || dnsmasq --conf-file=/dev/null -C "$DNSMASQ_CONF" 2>&1
    sleep 1

    echo "[OK] Servicios iniciados."
}

show_summary() {
    echo ""
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║        Punto de Acceso WiFi — BrazoRoboticoV2               ║"
    echo "╠══════════════════════════════════════════════════════════════╣"
    printf "║  SSID:         %-40s ║\n" "$SSID"
    printf "║  Password:     %-40s ║\n" "$PASS"
    printf "║  IP del AP:    %-40s ║\n" "$AP_IP"
    printf "║  Interfaz:     %-40s ║\n" "$AP_IFACE"
    printf "║  DHCP:         %s - %-29s ║\n" "$DHCP_RANGE_START" "$DHCP_RANGE_END"
    echo "╠══════════════════════════════════════════════════════════════╣"
    echo "║  Conéctate al WiFi y abre:                                  ║"
    printf "║  https://%s:3000                                          ║\n" "$AP_IP"
    echo "║                                                              ║"
    echo "║  Para detener el AP: sudo $0 --stop          ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
}

stop_ap() {
    echo "[INFO] Deteniendo Punto de Acceso WiFi..."

    systemctl stop hostapd 2>/dev/null || true
    systemctl stop dnsmasq 2>/dev/null || true
    killall hostapd 2>/dev/null || true
    killall dnsmasq 2>/dev/null || true

    # Restaurar gestión de NetworkManager
    if systemctl is-active NetworkManager &>/dev/null; then
        nmcli device set "$AP_IFACE" managed yes 2>/dev/null || true
    fi

    # Liberar IP
    ip addr flush dev "$AP_IFACE" 2>/dev/null || true

    rm -f "$HOSTAPD_CONF" "$DNSMASQ_CONF"

    echo "[OK] AP detenido. La interfaz $AP_IFACE vuelve al modo cliente."
}

show_status() {
    echo "=== Estado del AP WiFi ==="
    echo ""

    if pgrep -x hostapd &>/dev/null; then
        echo "  hostapd:  ACTIVO (PID $(pgrep -x hostapd))"
    else
        echo "  hostapd:  INACTIVO"
    fi

    if pgrep -x dnsmasq &>/dev/null; then
        echo "  dnsmasq:  ACTIVO (PID $(pgrep -x dnsmasq))"
    else
        echo "  dnsmasq:  INACTIVO"
    fi

    echo ""
    echo "  Interfaz $AP_IFACE:"
    ip addr show "$AP_IFACE" 2>/dev/null | grep -E "inet|state" || echo "    (no disponible)"
    echo ""

    if iw dev "$AP_IFACE" info &>/dev/null; then
        echo "  Modo: $(iw dev "$AP_IFACE" info | grep type | awk '{print $2}')"
    fi
}

# --- Main --------------------------------------------------------------------

case "${1:-}" in
    --help|-h)
        usage
        ;;
    --stop)
        check_root
        stop_ap
        exit 0
        ;;
    --status)
        show_status
        exit 0
        ;;
    "")
        # Modo normal: iniciar AP
        check_root
        echo "=== Configuración de Punto de Acceso WiFi ==="
        echo ""
        check_packages
        stop_conflicting_services
        configure_interface
        write_hostapd_conf
        write_dnsmasq_conf
        start_services
        show_summary
        ;;
    *)
        echo "[ERROR] Opción desconocida: $1"
        usage
        ;;
esac
