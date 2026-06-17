---
name: feedback-sdkconfig-known-good-tag
description: "Antes de tocar sdkconfig en victron, verificar diff contra tag sdkconfig-known-good-YYYY-MM-DD - es el cinturon de seguridad"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

El proyecto victron mantiene tags `sdkconfig-known-good-YYYY-MM-DD` en git que apuntan a estados de sdkconfig validados como funcionales en el HW del usuario (JD9165BA DSI + ESP32-P4 + PSRAM 200M). Antes de cualquier accion que pueda regenerar sdkconfig (build con IDF distinta, fullclean, menuconfig, etc.) hacer `git diff sdkconfig-known-good-<ultimo> -- sdkconfig` y revisar capabilities criticas.

**Why:** El 2026-05-25 perdi 1+ hora reconstruyendo el setup tras un `fullclean` que regenero sdkconfig con valores incompatibles con el HW. El tag ya existia (`sdkconfig-known-good-2026-05-22`) y resolvio el problema cuando finalmente lo use. Habria evitado todo el desastre comparar PRIMERO.

**How to apply:**
- Verificar tags disponibles: `git -C ~/victron tag | grep sdkconfig-known-good`
- Diff antes de tocar nada: `git -C ~/victron diff sdkconfig-known-good-<fecha> -- sdkconfig`
- Capabilities clave a vigilar (todas necesarias para DSI sin underrun):
  - `CONFIG_SOC_AHB_GDMA_SUPPORT_PSRAM=y`
  - `CONFIG_SPIRAM_SPEED_200M=y` (NUNCA 20M)
  - `CONFIG_LCD_DSI_OBJ_FORCE_INTERNAL=y`
  - `CONFIG_LCD_DSI_ISR_HANDLER_IN_IRAM=y`
  - `CONFIG_ESP_LDO_VOLTAGE_PSRAM_DOMAIN=1800` (NO 1900)
- Si fueron borradas o reset: `git checkout sdkconfig-known-good-<fecha> -- sdkconfig` y rebuild
- Relacionado: [[project-victron-esp-idf]] (el tag asume IDF 5.4.4), [[feedback-destructive-commands-ask-first]]
- Tras una sesion exitosa de cambios en sdkconfig + flash funcional, el usuario puede crear un nuevo tag: `git tag sdkconfig-known-good-$(date +%Y-%m-%d)`
