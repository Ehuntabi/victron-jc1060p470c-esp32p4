---
name: project-joint-autocaravana
description: ~/joint/ es la carpeta raiz de proyectos relacionados con la autocaravana (marca Joint). Contiene 3 proyectos victron + ficheros FreeCAD soportes
metadata: 
  node_type: memory
  type: project
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

**Carpeta `~/joint/`** agrupa todos los proyectos relacionados con la autocaravana del usuario (marca "Joint"). Estructura:

```
~/joint/
├── victron/                                                  # Pantalla 7" ESP32-P4+C6 (activo, principal)
├── victron_mini/                                             # Variante mini
├── victronsolardisplayesp-multi-device_pantalla_3.5/         # Pantalla 3.5" ESP32-S3 (funciona OK Orion)
├── analizador_logico_centralita/                             # Trabajo con analizador logico centralita
├── control_luz_armario/                                      # Control luz interior
└── soporte/toma/etc *.FCStd                                  # FreeCAD models soportes (bornas, led armario, secador-calefaccion)
```

**Why:** 2026-05-26 el usuario reorganizo. Antes los proyectos victron estaban en `~/victron`, `~/victron_mini`, `~/victronsolardisplayesp-multi-device_pantalla_3.5`. Movidos a `~/joint/<name>/` para tener todo lo de la autocaravana junto. Las copias de respaldo `victron_broken_1858` y `waveshare_7b` fueron borradas (eran snapshots sin git, 827 MB liberados).

**How to apply:**
- El path activo del victron 7" es ahora `~/joint/victron/` (NO `~/victron`)
- CLAUDE.md del proyecto victron tiene `Workspace: ~/victron` que esta DESACTUALIZADO - actualizar si se edita
- Builds ESP-IDF: el `build/` puede tener CMakeCache.txt con paths absolutos viejos `/home/jc/victron/...`. Si CMake falla tras el move -> `rm -rf build/` (NO fullclean!) para regenerar caches manteniendo sdkconfig - ver [[feedback-sdkconfig-known-good-tag]]
- Repos GitHub:
  - victron 7": git@github.com:Ehuntabi/victron-jc1060p470c-esp32p4.git
  - victron_mini: git@github.com:Ehuntabi/victron-mini-c6-esp-now.git
  - pantalla 3.5": git@github.com:Ehuntabi/victron-display-3.5-esp32-s3.git
- Relacionado: [[project-victron-esp-idf]] - ESP-IDF 5.4.4 estricto para todos
