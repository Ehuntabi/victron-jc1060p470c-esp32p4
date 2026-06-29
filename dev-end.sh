#!/usr/bin/env bash
# Cierre de sesion de desarrollo (ejecutar al terminar en CUALQUIER PC).
# Normaliza el lock (portable), commitea TODO tu trabajo y lo sube a GitHub.
#   Uso:  ./dev-end.sh ["mensaje de commit"]
set -euo pipefail
cd "$(dirname "$0")"

REL='path: components/espressif__esp_hosted'
# Re-relativizar la ruta del esp_hosted local (el build la deja absoluta por maquina).
# Asi el lock commiteado es portable entre PCs.
sed -i -E "s#path: .*/components/espressif__esp_hosted#${REL}#" dependencies.lock 2>/dev/null || true

git add -A
if git diff --cached --quiet; then
  echo "[dev-end] Nada nuevo que commitear."
else
  msg="${1:-wip $(date '+%Y-%m-%d %H:%M')}"
  git commit -m "$msg"
  echo "[dev-end] commit: $msg"
fi

echo "[dev-end] git push..."
git push
echo "[dev-end] HECHO. Ya puedes cambiar de PC y hacer ./dev-start.sh alli."
