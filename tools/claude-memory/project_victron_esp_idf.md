---
name: project-victron-esp-idf
description: Stack victron y derivados (todos los 7 proyectos ESP32-P4) requieren ESP-IDF 5.4.4 estricto - no cambiar version sin permiso
metadata: 
  node_type: memory
  type: project
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

Todos los proyectos ESP32-P4 del usuario usan **ESP-IDF v5.4.4** (no 5.4.1, no 5.5.x): victron, victron_mini, esp_hosted_slave, waveshare_7b, uart_bridge, victronsolardisplayesp-multi-device_pantalla_3.5.

**Install activo unico (estado 2026-05-25):**
- Path: `/home/jc/.espressif/esp-idf-5.4` (en tag v5.4.4)
- Tools: `/home/jc/.espressif/tools/` (purgados los de 5.3, 5.5 con `idf_tools.py uninstall`)
- Activar: `. ~/.espressif/esp-idf-5.4/export.sh`
- CLAUDE.md de victron menciona `~/esp/esp-idf/export.sh` pero ese path ya no existe - el real es el de .espressif/

**Why:** 5.4.4 introduce capabilities criticas para JD9165BA + DSI 1024x600:
- `CONFIG_SOC_AHB_GDMA_SUPPORT_PSRAM=y` (sin esto, DMA no lee framebuffer de PSRAM rapido)
- `CONFIG_LCD_DSI_OBJ_FORCE_INTERNAL=y`
- `CONFIG_LCD_DSI_ISR_HANDLER_IN_IRAM=y`
- PSRAM speed 200 MHz (vs 20 MHz default que provoca DPI underrun continuo)

En 5.4.1 esas capabilities NO existen en Kconfig -> sdkconfig regenerado pierde optimizaciones -> pantalla azul con error repetido `lcd.dsi.dpi: can't fetch data from external memory fast enough, underrun happens`.

**How to apply:**
- Antes de tocar sdkconfig o IDF version: verificar tag `sdkconfig-known-good-2026-05-22` (en repo victron)
- Si el toolchain warning dice "Expected X, found Y" eso es WARNING no error. NO ejecutar fullclean para "arreglarlo" - ver [[feedback-destructive-commands-ask-first]]
- Para recuperar tras desajuste: `cd ~/.espressif/esp-idf-5.4 && git fetch && git checkout v5.4.4 && git submodule update --init --recursive && ./install.sh esp32p4`
- Despues borrar solo `build/` (NO managed_components/), volver a `. export.sh` y `idf.py build`
