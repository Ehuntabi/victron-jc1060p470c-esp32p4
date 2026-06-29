# Flujo de desarrollo multi-PC (PC sobremesa <-> portatil)

Para que dé igual el equipo que uses y NO tengas que acordarte de comandos.

## Regla de oro
- **Compartido (en git):** `dependencies.lock` (versiones de componentes) y `sdkconfig` (config).
- **Local de cada PC (gitignored, se regenera):** `build/` y `managed_components/`. Nunca se commitean.

## Al EMPEZAR a trabajar (en cualquier PC)
```bash
./dev-start.sh
```
Hace: `git pull` + (si cambiaron deps/sdkconfig) borra `managed_components/`+`build/` para
regenerarlos desde el lock compartido. Te deja listo para `idf.py build`.

## Al TERMINAR (en cualquier PC)
```bash
./dev-end.sh "lo que hice"     # el mensaje es opcional
```
Hace: normaliza la ruta del componente local `esp_hosted` (portable) + `git add -A` + commit + push.

## ¿Por qué hace falta normalizar la ruta?
`components/espressif__esp_hosted/` es un fork local (viaja por git). El gestor de componentes
escribe su ruta ABSOLUTA (`/home/<usuario>/...`) en `dependencies.lock` en cada build, lo que rompe
en el otro PC. Por eso el lock se **commitea con ruta relativa** (`components/espressif__esp_hosted`)
y los scripts la re-relativizan automaticamente al cerrar. Tu no tienes que hacer nada.

## Si algo falla (raro)
- Conflicto en `git pull`: resuelvelo y reintenta `./dev-start.sh`.
- Build con error de Kconfig/componentes: `rm -rf managed_components build && idf.py reconfigure`.
- Volver al ultimo estado bueno: `git tag` lista los `sdkconfig-known-good-*`.
