#!/usr/bin/env bash
# =============================================================================
# Instalador de BrazoRoboticoV2 para Arduino UNO Q (QRB2210)
#
# Este script despliega todo el stack en el UNO Q:
#   1. Verifica usuario y permisos
#   2. Instala dependencias del sistema (python3-pip, hostapd, dnsmasq, socat)
#   3. Instala dependencias Python (Flask, Flask-Sock, pyserial, etc.)
#   4. Crea la estructura de directorios
#   5. Copia backend y frontend
#   6. Instala servicios systemd
#   7. Recarga y habilita servicios
#   8. Configura AP WiFi (opcional)
#   9. Muestra resumen final
#
# Uso:
#   ./install.sh                           # Instalación completa
#   ./install.sh --dry-run                 # Simular sin cambios
#   ./install.sh --skip-wifi               # Saltar configuración WiFi
#   ./install.sh --no-python-deps          # Saltar instalación de pip
#   ./install.sh --uninstall               # Revertir instalación
#
# Ejecutar como arduino o root en el UNO Q.
# =============================================================================

set -euo pipefail

# --- Constantes ---------------------------------------------------------------

# Directorios de origen (en la máquina de desarrollo)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BACKEND_SRC="$PROJECT_DIR/backend"
FRONTEND_SRC="$PROJECT_DIR/frontend"
DEPLOY_SRC="$PROJECT_DIR/deploy"

# Directorios de destino (en el UNO Q)
DEST_DIR="/home/arduino/brazorobotico"
BACKEND_DEST="$DEST_DIR/backend"
FRONTEND_DEST="$DEST_DIR/frontend"
SYSTEMD_DIR="/etc/systemd/system"
UDEV_DIR="/etc/udev/rules.d"

# Colores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Contador de pasos
STEP=0
TOTAL_STEPS=10

# Flags
DRY_RUN=false
SKIP_WIFI=false
NO_PYTHON_DEPS=false

# --- Funciones de utilería ---------------------------------------------------

log_info()  { echo -e "${CYAN}[INFO]${NC}  $1"; }
log_ok()    { echo -e "${GREEN}[OK]${NC}    $1"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

step() {
    STEP=$((STEP + 1))
    echo ""
    echo -e "${CYAN}══════════════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Paso $STEP/$TOTAL_STEPS — $1${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════════════════${NC}"
    echo ""
}

run_cmd() {
    if $DRY_RUN; then
        echo -e "${YELLOW}[DRY-RUN]${NC} $*"
    else
        "$@"
    fi
}

check_user() {
    local user_ok=false

    if [[ $EUID -eq 0 ]]; then
        log_info "Ejecutando como root — se respetará el usuario 'arduino' para los servicios."
        user_ok=true
    elif [[ "$(whoami)" == "arduino" ]]; then
        log_info "Ejecutando como usuario 'arduino'."
        user_ok=true
    else
        log_warn "Ejecutando como '$(whoami)'. Se recomienda 'arduino' o root."
        echo -n "¿Continuar de todas formas? [s/N]: "
        read -r response
        if [[ ! "$response" =~ ^[sS]$ ]]; then
            log_error "Instalación cancelada."
            exit 1
        fi
    fi
}

confirm_step() {
    echo ""
    echo -n "¿Continuar? [S/n]: "
    read -r response
    if [[ "$response" =~ ^[nN]$ ]]; then
        log_warn "Instalación cancelada por el usuario."
        exit 0
    fi
}

# --- Pasos de instalación ----------------------------------------------------

step_verify_system() {
    step "Verificación del sistema"

    log_info "Sistema operativo: $(uname -a 2>/dev/null || echo 'desconocido')"
    log_info "Arquitectura: $(uname -m)"
    log_info "Usuario actual: $(whoami)"

    # Verificar que es un UNO Q (Debian)
    if grep -qi "debian" /etc/os-release 2>/dev/null; then
        log_ok "Sistema Debian detectado (Arduino UNO Q compatible)."
    else
        log_warn "No se detectó Debian Linux. ¿Estás en un UNO Q?"
        echo -n "¿Continuar de todas formas? [s/N]: "
        read -r response
        if [[ ! "$response" =~ ^[sS]$ ]]; then
            exit 1
        fi
    fi
}

step_install_system_deps() {
    step "Instalación de dependencias del sistema"

    local packages=(
        python3-pip
        python3-venv
        hostapd
        dnsmasq
        socat
        iw
        curl
        git
    )

    log_info "Paquetes a instalar: ${packages[*]}"

    if $DRY_RUN; then
        log_info "[DRY-RUN] apt-get install -y ${packages[*]}"
    else
        apt-get update -qq
        apt-get install -y -qq "${packages[@]}"
        log_ok "Dependencias del sistema instaladas."

        # Verificar que socat está disponible
        if command -v socat &>/dev/null; then
            log_ok "socat disponible: $(socat -V | head -1)"
        else
            log_error "socat no se instaló correctamente."
            exit 1
        fi
    fi
}

step_install_python_deps() {
    step "Instalación de dependencias Python"

    if $NO_PYTHON_DEPS; then
        log_info "Omitido (--no-python-deps activo)."
        return
    fi

    local req_file="$SCRIPT_DIR/requirements.txt"

    if [[ ! -f "$req_file" ]]; then
        log_error "No se encuentra $req_file"
        exit 1
    fi

    log_info "Instalando desde $req_file..."

    if $DRY_RUN; then
        log_info "[DRY-RUN] pip install -r $req_file"
    else
        pip3 install --break-system-packages -r "$req_file" 2>/dev/null \
            || pip3 install -r "$req_file"

        log_ok "Dependencias Python instaladas."

        # Verificar instalación
        python3 -c "
import flask, flask_sock, serial, numpy, waitress, yaml
print(f'Flask:      {flask.__version__}')
print(f'Flask-Sock: {flask_sock.__version__}')
print(f'PySerial:   {serial.__version__}')
print(f'NumPy:      {numpy.__version__}')
print(f'Waitress:   {waitress.__version__}')
print(f'PyYAML:     {yaml.__version__}')
" 2>&1 | while IFS= read -r line; do log_info "  $line"; done

        log_ok "Todas las librerías Python se importan correctamente."
    fi
}

step_create_directories() {
    step "Creación de directorios en $DEST_DIR"

    local dirs=(
        "$DEST_DIR"
        "$BACKEND_DEST"
        "$BACKEND_DEST/config"
        "$FRONTEND_DEST"
    )

    for dir in "${dirs[@]}"; do
        if $DRY_RUN; then
            log_info "[DRY-RUN] mkdir -p $dir"
        else
            mkdir -p "$dir"
            log_info "Directorio: $dir"
        fi
    done

    log_ok "Estructura de directorios creada."
}

step_copy_backend() {
    step "Copia del backend"

    if [[ ! -d "$BACKEND_SRC" ]]; then
        log_warn "No se encuentra $BACKEND_SRC — saltando."
        return
    fi

    log_info "Origen: $BACKEND_SRC"
    log_info "Destino: $BACKEND_DEST"

    if $DRY_RUN; then
        log_info "[DRY-RUN] rsync -av --delete $BACKEND_SRC/ $BACKEND_DEST/"
    else
        rsync -av --delete "$BACKEND_SRC/" "$BACKEND_DEST/" --exclude='__pycache__' --exclude='*.pyc' --exclude='.venv'
        log_ok "Backend copiado."
    fi
}

step_copy_frontend() {
    step "Copia del frontend (build)"
    #
    # El frontend se construye con:
    #   cd frontend && npm run build
    #
    # Vite está configurado (vite.config.ts) para volcar el build en
    # backend/static/. Si existe backend/static/ se copia directamente.
    #

    local static_dir="$BACKEND_SRC/static"

    if [[ -d "$static_dir" ]] && [[ "$(ls -A "$static_dir" 2>/dev/null)" ]]; then
        log_info "Build estático encontrado en $static_dir"

        if $DRY_RUN; then
            log_info "[DRY-RUN] rsync -av --delete $static_dir/ $BACKEND_DEST/static/"
        else
            rsync -av --delete "$static_dir/" "$BACKEND_DEST/static/"
            log_ok "Frontend build copiado a backend/static/."
        fi
    elif [[ -d "$FRONTEND_SRC" ]]; then
        log_warn "No hay build de frontend en $static_dir"
        log_info "Para construirlo: cd $FRONTEND_SRC && npm install && npm run build"
        echo ""
        echo -n "¿Construir frontend ahora? (requiere Node.js) [s/N]: "
        read -r response
        if [[ "$response" =~ ^[sS]$ ]]; then
            if command -v node &>/dev/null; then
                (cd "$FRONTEND_SRC" && npm install && npm run build)
                if $DRY_RUN; then
                    log_info "[DRY-RUN] rsync -av --delete $static_dir/ $BACKEND_DEST/static/"
                else
                    rsync -av --delete "$static_dir/" "$BACKEND_DEST/static/"
                    log_ok "Frontend construido y copiado."
                fi
            else
                log_error "Node.js no está instalado. No se puede construir el frontend."
            fi
        else
            log_warn "Frontend no copiado. El servidor Flask funcionará sin UI."
        fi
    else
        log_warn "No se encuentra frontend en $FRONTEND_SRC."
    fi
}

step_install_systemd_services() {
    step "Instalación de servicios systemd"

    local services=(
        "socat.service"
        "robot-hand.service"
    )

    for svc in "${services[@]}"; do
        local src="$SCRIPT_DIR/systemd/$svc"
        local dst="$SYSTEMD_DIR/$svc"

        if [[ ! -f "$src" ]]; then
            log_warn "No se encuentra $src — saltando."
            continue
        fi

        log_info "Instalando $svc..."

        if $DRY_RUN; then
            log_info "[DRY-RUN] cp $src $dst"
        else
            cp "$src" "$dst"
            chmod 644 "$dst"
            log_ok "$svc instalado."
        fi
    done

    # Instalar drop-in para arduino-router.service si existe
    local dropin_src="$SCRIPT_DIR/systemd/arduino-router-dropin.conf"
    local dropin_dst="/etc/systemd/system/arduino-router.service.d/20-no-reset.conf"

    if [[ -f "$dropin_src" ]]; then
        log_info "Instalando drop-in para arduino-router.service..."
        if $DRY_RUN; then
            log_info "[DRY-RUN] mkdir -p $(dirname "$dropin_dst") && cp $dropin_src $dropin_dst"
        else
            mkdir -p "$(dirname "$dropin_dst")"
            cp "$dropin_src" "$dropin_dst"
            chmod 644 "$dropin_dst"
            log_ok "Drop-in instalado (previene reset del STM32)."
        fi
    else
        log_info "Drop-in no encontrado — saltando."
    fi
}

step_install_udev_rules() {
    step "Instalación de reglas udev"

    local udev_src="$SCRIPT_DIR/udev/99-arduino-uno-q.rules"
    local udev_dst="$UDEV_DIR/99-arduino-uno-q.rules"

    if [[ ! -f "$udev_src" ]]; then
        log_warn "No se encuentra $udev_src — saltando."
        return
    fi

    log_info "Instalando reglas udev para Arduino UNO Q..."

    if $DRY_RUN; then
        log_info "[DRY-RUN] cp $udev_src $udev_dst"
        log_info "[DRY-RUN] udevadm control --reload-rules && udevadm trigger"
    else
        cp "$udev_src" "$udev_dst"
        chmod 644 "$udev_dst"
        udevadm control --reload-rules 2>/dev/null || true
        udevadm trigger 2>/dev/null || true
        log_ok "Reglas udev instaladas y recargadas."
    fi
}

step_reload_and_enable() {
    step "Recarga de systemd y habilitación de servicios"

    if $DRY_RUN; then
        log_info "[DRY-RUN] systemctl daemon-reload"
        log_info "[DRY-RUN] systemctl enable socat.service robot-hand.service"
    else
        log_info "Recargando systemd..."
        systemctl daemon-reload
        log_ok "systemd recargado."

        log_info "Habilitando socat.service..."
        systemctl enable socat.service
        log_ok "socat.service habilitado."

        log_info "Habilitando robot-hand.service..."
        systemctl enable robot-hand.service
        log_ok "robot-hand.service habilitado."

        # Mostrar estado
        echo ""
        log_info "Estado de los servicios:"
        systemctl is-enabled socat.service robot-hand.service 2>/dev/null | while IFS= read -r line; do
            log_info "  $line"
        done
    fi
}

step_configure_wifi() {
    step "Configuración de WiFi AP (opcional)"

    if $SKIP_WIFI; then
        log_info "Omitido (--skip-wifi activo)."
        return
    fi

    echo ""
    echo "¿Deseas configurar el UNO Q como punto de acceso WiFi?"
    echo "  SSID: RobotHand"
    echo "  Pass: robot2026"
    echo "  IP:   192.168.4.1"
    echo ""
    echo -n "Configurar AP WiFi ahora? [s/N]: "
    read -r response

    if [[ "$response" =~ ^[sS]$ ]]; then
        local setup_script="$SCRIPT_DIR/wifi-ap/setup.sh"

        if [[ ! -f "$setup_script" ]]; then
            log_error "No se encuentra $setup_script"
            return
        fi

        if $DRY_RUN; then
            log_info "[DRY-RUN] sudo bash $setup_script"
        else
            chmod +x "$setup_script"
            bash "$setup_script"
            log_ok "AP WiFi configurado."
        fi
    else
        log_info "Omitiendo configuración WiFi AP."
        log_info "Puedes configurarlo después con: sudo ./deploy/wifi-ap/setup.sh"
    fi
}

step_show_summary() {
    step "Resumen de instalación"

    echo ""
    echo "╔══════════════════════════════════════════════════════════════════╗"
    echo "║        Instalación completada — BrazoRoboticoV2                  ║"
    echo "╠══════════════════════════════════════════════════════════════════╣"
    echo "║                                                                    ║"
    printf "║  Directorio:       %-47s ║\n" "$DEST_DIR"
    printf "║  Backend:          %-47s ║\n" "$BACKEND_DEST"
    printf "║  Frontend build:   %-47s ║\n" "$BACKEND_DEST/static"
    echo "║                                                                    ║"
    echo "║  Servicios instalados:                                            ║"
    echo "║    • socat.service      (TCP:7500 ↔ USB Gadget Serial)            ║"
    echo "║    • robot-hand.service (Flask + WebSocket :3000)                 ║"
    echo "║                                                                    ║"

    if systemctl is-active robot-hand.service &>/dev/null 2>&1; then
        echo "║  ✅ robot-hand.service: ACTIVO                                  ║"
    else
        echo "║  ⚠️  robot-hand.service: INACTIVO (iniciar con systemctl start)   ║"
    fi
    if systemctl is-active socat.service &>/dev/null 2>&1; then
        echo "║  ✅ socat.service:      ACTIVO                                  ║"
    else
        echo "║  ⚠️  socat.service:      INACTIVO (iniciar con systemctl start)   ║"
    fi

    echo "║                                                                    ║"
    echo "║  Acceso:                                                          ║"
    printf "║    • Red local:    https://192.168.31.12:3000                    ║\n"
    printf "║    • WiFi AP:      https://192.168.4.1:3000                      ║\n"
    echo "║                                                                    ║"
    echo "║  Comandos útiles:                                                  ║"
    echo "║    sudo systemctl start robot-hand.service    Iniciar servidor     ║"
    echo "║    sudo systemctl stop robot-hand.service     Detener servidor     ║"
    echo "║    sudo journalctl -fu robot-hand.service     Ver logs             ║"
    echo "║    sudo systemctl status socat.service        Estado del bridge    ║"
    echo "║                                                                    ║"
    echo "╚══════════════════════════════════════════════════════════════════╝"
    echo ""
}

# --- Uninstall ----------------------------------------------------------------

uninstall() {
    echo ""
    echo -e "${RED}╔══════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}║  DESINSTALACIÓN — Esto eliminará todos los archivos         ║${NC}"
    echo -e "${RED}║  y servicios instalados por este script.                    ║${NC}"
    echo -e "${RED}╚══════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo -n "¿Estás seguro? Escribe 'BORRAR' para confirmar: "
    read -r response
    if [[ "$response" != "BORRAR" ]]; then
        echo "Cancelado."
        exit 0
    fi

    echo ""
    log_info "Deteniendo servicios..."
    systemctl stop robot-hand.service 2>/dev/null || true
    systemctl stop socat.service 2>/dev/null || true

    log_info "Deshabilitando servicios..."
    systemctl disable robot-hand.service 2>/dev/null || true
    systemctl disable socat.service 2>/dev/null || true

    log_info "Eliminando archivos de servicio..."
    rm -f "$SYSTEMD_DIR/socat.service"
    rm -f "$SYSTEMD_DIR/robot-hand.service"
    rm -rf "/etc/systemd/system/arduino-router.service.d/"

    log_info "Eliminando reglas udev..."
    rm -f "$UDEV_DIR/99-arduino-uno-q.rules"

    log_info "Eliminando directorio del proyecto..."
    rm -rf "$DEST_DIR"

    log_info "Recargando systemd..."
    systemctl daemon-reload
    udevadm control --reload-rules 2>/dev/null || true

    echo ""
    log_ok "Desinstalación completada."
    log_info "Las dependencias Python y del sistema no se eliminaron."
    log_info "Para removerlas manualmente:"
    log_info "  pip uninstall Flask Flask-Sock pyserial numpy waitress pyyaml"
    log_info "  apt remove python3-pip hostapd dnsmasq socat"
}

# --- Main --------------------------------------------------------------------

# Procesar argumentos
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --skip-wifi)
            SKIP_WIFI=true
            shift
            ;;
        --no-python-deps)
            NO_PYTHON_DEPS=true
            shift
            ;;
        --uninstall)
            check_user
            uninstall
            exit 0
            ;;
        --help|-h)
            echo "Uso: $0 [--dry-run] [--skip-wifi] [--no-python-deps] [--uninstall]"
            echo ""
            echo "  --dry-run         Simular instalación sin hacer cambios"
            echo "  --skip-wifi       Omitir configuración de WiFi AP"
            echo "  --no-python-deps  Omitir instalación de dependencias pip"
            echo "  --uninstall       Revertir completamente la instalación"
            exit 0
            ;;
        *)
            log_error "Opción desconocida: $1"
            echo "Usa --help para ver las opciones disponibles."
            exit 1
            ;;
    esac
done

# Banner
echo ""
echo -e "${CYAN}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║    Instalador de BrazoRoboticoV2 para Arduino UNO Q         ║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════════╝${NC}"
echo ""

if $DRY_RUN; then
    log_warn "MODO DRY-RUN — No se realizarán cambios en el sistema."
    echo ""
fi

# Ejecutar pasos
check_user
step_verify_system
step_install_system_deps
step_install_python_deps
step_create_directories
step_copy_backend
step_copy_frontend
step_install_systemd_services
step_install_udev_rules
step_reload_and_enable
step_configure_wifi
step_show_summary

log_ok "Instalación completada exitosamente."
echo ""
echo "Recomendación:"
echo "  1. Verifica la configuración en $BACKEND_DEST/config/"
echo "  2. Inicia los servicios: sudo systemctl start socat.service robot-hand.service"
echo "  3. Revisa los logs: sudo journalctl -fu robot-hand.service"
echo ""
