#!/bin/bash
# Instala Claude Code (binario nativo) en el portatil y migra memorias del repo
# Uso desde el portatil:
#   cd ~/joint/victron && git pull && bash tools/install-claude.sh
# Sin sudo - todo en ~/.local/ y ~/.claude/

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MEM_SRC="$SCRIPT_DIR/claude-memory"
CLAUDE_MD_SRC="$MEM_SRC/CLAUDE-global.md"

RED='\033[0;31m'
GRN='\033[0;32m'
YEL='\033[0;33m'
BLU='\033[0;34m'
NC='\033[0m'

step() { echo -e "\n${BLU}=== $1 ===${NC}"; }
ok()   { echo -e "${GRN}OK${NC}: $1"; }
warn() { echo -e "${YEL}WARN${NC}: $1"; }
err()  { echo -e "${RED}ERROR${NC}: $1"; }

# -----------------------------------------------------------------------------
step "1. Verificar contenido del repo"

if [ ! -d "$MEM_SRC" ]; then
  err "No existe $MEM_SRC. Haz git pull primero"
  exit 1
fi
ok "Carpeta claude-memory presente con $(ls "$MEM_SRC" | wc -l) archivos"

if ! command -v curl >/dev/null 2>&1; then
  err "curl no instalado. Instalarlo con: sudo apt install curl"
  exit 1
fi
ok "curl disponible"

# -----------------------------------------------------------------------------
step "2. Instalar Claude Code (binario nativo)"

if command -v claude >/dev/null 2>&1; then
  cver=$(claude --version 2>/dev/null | head -1 || echo "?")
  ok "Claude Code ya instalado: $cver"
  read -r -p "Actualizar a la ultima version? (y/N): " ans
  if [ "${ans,,}" = "y" ]; then
    REINSTALL=1
  fi
else
  REINSTALL=1
fi

if [ "${REINSTALL:-0}" = "1" ]; then
  echo "Descargando e instalando Claude Code..."
  curl -fsSL https://claude.ai/install.sh | bash 2>&1 | tail -10
  if [ -x "$HOME/.local/bin/claude" ]; then
    export PATH="$HOME/.local/bin:$PATH"
    ok "Claude Code instalado: $($HOME/.local/bin/claude --version 2>/dev/null | head -1)"
  else
    err "Claude Code install fallo. Pasos manuales:"
    err "  1. https://docs.claude.com/en/docs/claude-code/setup"
    exit 1
  fi
  # Anadir PATH al .bashrc si no esta
  if ! grep -q "\.local/bin" "$HOME/.bashrc" 2>/dev/null; then
    echo '' >> "$HOME/.bashrc"
    echo '# Claude Code en ~/.local/bin' >> "$HOME/.bashrc"
    echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$HOME/.bashrc"
    ok "PATH anadido a ~/.bashrc"
  fi
fi

# -----------------------------------------------------------------------------
step "3. Migrar CLAUDE.md global"

mkdir -p "$HOME/.claude"

if [ -f "$HOME/.claude/CLAUDE.md" ]; then
  bak="$HOME/.claude/CLAUDE.md.bak.$(date +%Y%m%d-%H%M%S)"
  cp "$HOME/.claude/CLAUDE.md" "$bak"
  warn "CLAUDE.md existia, backup en $bak"
fi
cp "$CLAUDE_MD_SRC" "$HOME/.claude/CLAUDE.md"
ok "CLAUDE.md global copiado"

# -----------------------------------------------------------------------------
step "4. Migrar memorias persistentes"

# Path dinamico: -home-<user> (no hardcoded -home-jc)
USERNAME=$(whoami)
for d in "$HOME/.claude/projects/-home-$USERNAME/memory" "$HOME/.claude/projects/-home-$USERNAME-joint-victron/memory"; do
  mkdir -p "$d"
  if [ -n "$(ls -A "$d" 2>/dev/null)" ]; then
    bak="$d.bak.$(date +%Y%m%d-%H%M%S)"
    mv "$d" "$bak"
    mkdir -p "$d"
    warn "Memorias previas backed up en $bak"
  fi
  for f in "$MEM_SRC"/*.md; do
    [ "$(basename "$f")" = "CLAUDE-global.md" ] && continue
    cp "$f" "$d/"
  done
  ok "$(ls "$d" | wc -l) memorias copiadas a $d"
done

# -----------------------------------------------------------------------------
step "5. RESUMEN"

echo ""
echo "Listo. Para empezar:"
echo ""
echo "  source ~/.bashrc          # recargar PATH si se anadio"
echo "  cd ~/joint/victron"
echo "  claude"
echo ""
echo "La primera vez te pedira auth (OAuth web o API key)."
echo "Pidele:"
echo "  > Lee VICTRON-CONTEXT.md y AUTOCARAVANA-SETUP.md para contexto"
echo "  > Empezamos con el sniff CHECK button NE187"
echo ""
echo "Las memorias persistentes estan en:"
echo "  ~/.claude/projects/-home-jc/memory/"
echo "  ~/.claude/projects/-home-jc-joint-victron/memory/"
