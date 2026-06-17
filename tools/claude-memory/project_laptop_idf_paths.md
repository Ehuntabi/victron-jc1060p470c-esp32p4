---
name: project_laptop_idf_paths
description: Portatil autocaravana - rutas ESP-IDF y puerto difieren del CLAUDE.md
metadata: 
  node_type: memory
  type: project
  originSessionId: 5c9e46a5-ed43-43c5-b968-9a33b9e8eb45
---

En el portatil de la autocaravana (usuario `db3`, repo en `/home/db3/joint/victron`)
el entorno difiere de lo documentado en CLAUDE.md:

- ESP-IDF v5.4.4 esta en `~/.espressif/esp-idf-5.4/export.sh` (NO en `~/esp/esp-idf`).
  Antes de sourcear export.sh hay que exportar `IDF_TOOLS_PATH=/home/db3/.espressif`.
- El puerto del ESP32-P4 es `/dev/ttyACM0` (el CLAUDE.md menciona ACM1).

Setup que hubo que reparar una vez (2026-06-17, primer flash en este portatil):
- Faltaba el paquete de sistema `python3.12-venv` (lo instala el usuario con sudo);
  sin el, `install.sh` no puede crear el venv de IDF.
- Faltaban `cmake` y `ninja`: se instalan SIN sudo con
  `python $IDF_PATH/tools/idf_tools.py install cmake ninja` (los baja a ~/.espressif/tools).

Comando de flash que funciono: `idf.py -p /dev/ttyACM0 flash`.
Relacionado: [[project_joint_autocaravana]], [[project_victron_esp_idf]].
