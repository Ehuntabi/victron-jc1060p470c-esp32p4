#!/bin/bash
# Setup automatico de los 3 proyectos Victron en el portatil de la autocaravana
# Disenado para Linux Mint sin sudo (todo en home del usuario)
#
# Uso desde el portatil:
#   curl -fsSL https://raw.githubusercontent.com/Ehuntabi/victron-jc1060p470c-esp32p4/main/setup-autocaravana.sh -o /tmp/setup.sh
#   (o copiar este archivo manualmente)
#   bash /tmp/setup.sh
#
# Si algun paso falla por falta de paquete, el script lo dice claro
# y sigue con lo que puede. Anota los fallos para resolver con sudo cuando vuelvas.

set -uo pipefail

JOINT_DIR="$HOME/joint"
IDF_DIR="$HOME/.espressif/esp-idf-5.4"
IDF_TARGETS="esp32p4,esp32c6,esp32s3"
IDF_BRANCH="v5.4.4"

# Colores
RED='\033[0;31m'
GRN='\033[0;32m'
YEL='\033[0;33m'
BLU='\033[0;34m'
NC='\033[0m'

step() { echo -e "\n${BLU}=== $1 ===${NC}"; }
ok()   { echo -e "${GRN}OK${NC}: $1"; }
warn() { echo -e "${YEL}WARN${NC}: $1"; }
err()  { echo -e "${RED}ERROR${NC}: $1"; }

FAILS=()

# -----------------------------------------------------------------------------
step "1. Verificar dependencias basicas (sin sudo)"

needed_cmds=(git curl wget python3 cmake ninja make gcc dfu-util)
missing=()
for c in "${needed_cmds[@]}"; do
  if command -v "$c" >/dev/null 2>&1; then
    ok "$c"
  else
    missing+=("$c")
    warn "$c NO instalado"
  fi
done

if [ ${#missing[@]} -gt 0 ]; then
  err "Faltan paquetes. Instalalos con sudo cuando puedas:"
  echo "  sudo apt install ${missing[*]} python3-pip python3-venv libusb-1.0-0 libffi-dev libssl-dev"
  FAILS+=("Dependencias apt: ${missing[*]}")
fi

# Verificar python version >= 3.10
if command -v python3 >/dev/null 2>&1; then
  pyver=$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
  ok "python3 = $pyver"
fi

# -----------------------------------------------------------------------------
step "2. Grupo dialout (acceso a /dev/ttyUSB*, /dev/ttyACM*)"

if id -nG | grep -qw dialout; then
  ok "usuario en dialout (NO necesita sudo para puertos serie)"
else
  warn "usuario NO en dialout. Sin esto, flash dara permission denied"
  echo "  Solucion (cuando tengas password): sudo usermod -aG dialout \$USER && logout"
  FAILS+=("Usuario no en grupo dialout")
fi

# -----------------------------------------------------------------------------
step "3. Clonar/actualizar los 3 repos victron"

mkdir -p "$JOINT_DIR"
cd "$JOINT_DIR"

declare -A REPOS_SSH=(
  ["victron"]="git@github.com:Ehuntabi/victron-jc1060p470c-esp32p4.git"
  ["victron_mini"]="git@github.com:Ehuntabi/victron-mini-c6-esp-now.git"
  ["victronsolardisplayesp-multi-device_pantalla_3.5"]="git@github.com:Ehuntabi/victron-display-3.5-esp32-s3.git"
)
declare -A REPOS_HTTPS=(
  ["victron"]="https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4.git"
  ["victron_mini"]="https://github.com/Ehuntabi/victron-mini-c6-esp-now.git"
  ["victronsolardisplayesp-multi-device_pantalla_3.5"]="https://github.com/Ehuntabi/victron-display-3.5-esp32-s3.git"
)

for name in "${!REPOS_SSH[@]}"; do
  ssh_url="${REPOS_SSH[$name]}"
  https_url="${REPOS_HTTPS[$name]}"
  if [ -d "$name/.git" ]; then
    cd "$name"
    git fetch origin 2>&1 | head -3
    branch=$(git branch --show-current)
    if [ -z "$(git status --porcelain)" ]; then
      git pull --ff-only 2>&1 | tail -3
      ok "$name actualizado (branch $branch)"
    else
      warn "$name tiene cambios locales, NO se hace pull. Resuelve manualmente"
      git status --short
    fi
    cd ..
  else
    # Intentar SSH primero, fallback HTTPS si no tiene clave configurada
    echo "  Clonando $name (intento SSH)..."
    if git clone --recursive "$ssh_url" "$name" 2>&1 | tail -3; then
      ok "$name clonado (SSH)"
    else
      warn "SSH fallo (sin clave en GitHub?), intentando HTTPS..."
      if git clone --recursive "$https_url" "$name" 2>&1 | tail -3; then
        ok "$name clonado (HTTPS, lectura. Para push, configurar SSH key)"
      else
        err "$name fallo en clone (sin red? sin acceso GitHub?)"
        FAILS+=("Clone $name fallo")
      fi
    fi
  fi
done

# -----------------------------------------------------------------------------
step "4. Instalar ESP-IDF 5.4.4 (sin sudo, en \$HOME/.espressif/)"

if [ -d "$IDF_DIR" ] && [ -f "$IDF_DIR/export.sh" ]; then
  ok "ESP-IDF ya existe en $IDF_DIR"
  cd "$IDF_DIR"
  current_tag=$(git describe --tags 2>/dev/null || echo "?")
  echo "  Version actual: $current_tag"
else
  mkdir -p "$HOME/.espressif"
  echo "  Clonando ESP-IDF $IDF_BRANCH (sin --depth para tener tags)..."
  git clone -b "$IDF_BRANCH" --recursive https://github.com/espressif/esp-idf.git "$IDF_DIR" 2>&1 | tail -5
  if [ -d "$IDF_DIR" ]; then
    ok "ESP-IDF clonado"
  else
    err "ESP-IDF clone fallo"
    FAILS+=("ESP-IDF clone")
  fi
fi

if [ -f "$IDF_DIR/install.sh" ]; then
  echo "  Ejecutando install.sh para targets $IDF_TARGETS (tarda 5-10 min)..."
  cd "$IDF_DIR"
  ./install.sh "$IDF_TARGETS" 2>&1 | tail -10
  if [ $? -eq 0 ]; then
    ok "ESP-IDF instalado para $IDF_TARGETS"
  else
    err "ESP-IDF install.sh fallo (revisa output)"
    FAILS+=("ESP-IDF install")
  fi
fi

# -----------------------------------------------------------------------------
step "5. Verificar puertos USB conectados"

if command -v lsusb >/dev/null 2>&1; then
  echo "USB devices:"
  lsusb | grep -iE "silicon labs|qinheng|cp210|ch340|ftdi|esp32|espressif" || echo "  (ningun ESP detectado ahora)"
fi
echo "Serial ports:"
ls -la /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo "  (ningun puerto serie - conecta el ESP32 con USB)"

# -----------------------------------------------------------------------------
step "6. Anadir alias util a ~/.bashrc"

if ! grep -q "alias get_idf" ~/.bashrc; then
  cat >> ~/.bashrc <<'EOF'

# === Victron autocaravana setup (Claude 2026-06-16) ===
alias get_idf='. $HOME/.espressif/esp-idf-5.4/export.sh'
alias victron='cd ~/joint/victron && get_idf'
alias victron_mini='cd ~/joint/victron_mini && get_idf'
alias pantalla='cd ~/joint/victronsolardisplayesp-multi-device_pantalla_3.5 && get_idf'
EOF
  ok "aliases anadidos a ~/.bashrc (get_idf, victron, victron_mini, pantalla)"
  warn "Recargar bash: source ~/.bashrc o abrir terminal nueva"
fi

# -----------------------------------------------------------------------------
step "7. RESUMEN"

if [ ${#FAILS[@]} -eq 0 ]; then
  echo -e "${GRN}TODO OK. Listo para usar.${NC}"
  echo ""
  echo "Empezar a trabajar:"
  echo "  source ~/.bashrc       # cargar aliases"
  echo "  victron                # cd al proyecto + setup ESP-IDF"
  echo "  idf.py build           # build"
  echo "  idf.py -p /dev/ttyUSB0 flash monitor  # flash + serial monitor"
else
  echo -e "${RED}Hubo ${#FAILS[@]} problemas:${NC}"
  for f in "${FAILS[@]}"; do
    echo "  - $f"
  done
  echo ""
  echo "Resuelvelos cuando tengas la password de sudo, o sigue con lo que funcione."
fi

echo ""
echo "Documentacion completa: ~/joint/AUTOCARAVANA-SETUP.md"
