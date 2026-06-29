# Brillo automatico de la pantalla segun luz ambiente (via camara SC2336)

Fecha: 2026-06-29
Proyecto: victron-jc1060p470c-esp32p4 (Guition JC1060P470C_I_W, ESP32-P4)
Estado: diseno aprobado, pendiente de revision del usuario antes del plan.

## 1. Objetivo

Ajustar automaticamente el brillo del backlight de la pantalla en funcion de la
luz ambiente de la cabina, usando la **camara integrada SC2336** (MIPI-CSI) como
sensor de luz. Es el primer uso de la camara en este proyecto.

El board **no tiene sensor de luz dedicado** (verificado en el esquematico), por lo
que la camara es la unica via para medir luz ambiente.

## 2. Alcance

**En v1:**
- Medir luz ambiente con la camara (media de luminancia de un frame).
- Toggle "Brillo automatico" en Settings -> Display (persistido en NVS).
- Cuando esta ON, la camara fija el brillo durante el dia.
- Integracion con la arbitracion de brillo existente.

**Fuera de alcance (YAGNI):**
- Streaming, foto, grabacion, vista de camara en UI.
- Zonificacion/ROI de la imagen (de momento media global del frame).
- Curva de mapeo configurable por el usuario (constantes en codigo, ajustables).
- Enfoque alternativo por registros AEC del sensor (posible optimizacion futura).

## 3. Decisiones de diseno (acordadas con el usuario)

1. **Comportamiento vs lo existente:** toggle ON -> de dia manda la camara; en la
   franja nocturna configurada **el modo nocturno sigue mandando** (brillo fijo bajo);
   el **salvapantallas siempre atenua**. Precedencia final:
   `salvapantallas > modo nocturno (franja) > auto/camara > brillo manual`.
2. **Medicion:** Enfoque A = capturar frame a baja resolucion y promediar la luma (Y).
   Enfoque B (leer exposicion/ganancia del sensor por I2C) descartado para v1.
3. **Camara:** ve la luz de la cabina con vista despejada -> la media de luma es
   representativa.
4. **IDF:** todo sobre **ESP-IDF 5.4.4** sin actualizar. `esp_video` v2.2.0 requiere
   `idf >= 5.4` (verificado en su idf_component.yml) -> 5.4.4 es compatible.

## 4. Arquitectura

### Componente nuevo y aislado: `components/ambient_light/`

Encapsula toda la dependencia de la camara. El resto del firmware NO se entera de
esp_video; solo consume una API minima.

API publica (`ambient_light.h`):
- `esp_err_t ambient_light_init(void)` — inicializa esp_video + esp_cam_sensor (SC2336)
  a la resolucion mas baja util y pocos fps; arranca la task de muestreo.
- `esp_err_t ambient_light_deinit(void)`.
- `int  ambient_light_get_pct(void)` — % de brillo objetivo ya suavizado (0..100), o
  `-1` si no hay lectura valida todavia.
- `bool ambient_light_ok(void)` — true si la camara esta dando frames validos.
- `int  ambient_light_get_raw_luma(void)` — media de luma cruda 0..255 (para Fase 1 / debug).

Internamente:
- Task periodica (cada ~2-3 s) que captura 1 frame, calcula media de luma Y,
  aplica EMA (suavizado), banda muerta y mapeo a %.
- Constantes ajustables: periodo de muestreo, alfa EMA, BRI_MIN/BRI_MAX, banda muerta,
  curva (lineal en v1; gamma a afinar con datos reales).

### Integracion (cambio quirurgico)

En la arbitracion de brillo existente (`night_mode_timer_cb` en `main/main.c`), el
calculo de `target` pasa a:

```
if (screensaver.active)            -> brillo atenuado del screensaver   (sin cambios)
else if (night_mode en franja)     -> ui->night_mode.brightness          (sin cambios)
else if (auto ON && ambient_light_ok()) -> ambient_light_get_pct()       (NUEVO)
else                               -> ui->brightness (manual)            (sin cambios)
```

### UI

Toggle "Brillo automatico" en Settings -> Display, persistido en NVS (namespace del
config existente, integrado en config_backup como el resto). El slider de brillo manual
sigue siendo el valor de fallback (cuando auto OFF o camara KO).

## 5. Flujo de datos

`SC2336 (MIPI-CSI) -> esp_video (V4L2 capture) -> frame -> media de luma Y ->
EMA + banda muerta -> mapeo a % [BRI_MIN..BRI_MAX] -> ambient_light_get_pct() ->
arbitracion de brillo -> bsp_display_brightness_set(%) -> LEDC duty (GPIO 23)`

## 6. Robustez y manejo de errores

- **Camara KO** (no inicializa o deja de dar frames): `ambient_light_ok()` -> false ->
  la arbitracion cae a **brillo manual** (la pantalla NUNCA se queda a oscuras). Log de aviso.
- **Anti-parpadeo:** EMA sobre la luma + banda muerta (solo se cambia el brillo si el
  delta supera un umbral) + se reutiliza que `bsp_display_brightness_set` solo aplica si cambia.
- **Auto ON pero camara no arranca:** fallback a manual + aviso (jingle/log).

## 7. Riesgos y mitigacion

**Riesgo principal: bring-up de la camara sobre firmware fragil.**
- Anadir `esp_video` + `esp_cam_sensor` implica cambios de **sdkconfig** (habilitar CSI,
  buffers en PSRAM, driver). El proyecto esta pinado a 5.4.4 porque otras versiones
  rompian el **MIPI-DSI**. Convivencia de la **ISR de CSI con la ISR de DSI**
  (cache-safety): `CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE` esta hoy en `n` -> revisar.
- Existe un issue de Espressif sobre bring-up de **SC2336 en ESP32-P4** (pines reset/clock
  especificos del board) -> hay que confirmar el pinout de la camara (reset/PWDN/MCLK)
  del JC1060P470C (esquematico / wiki SpotPear).

**Mitigaciones:**
- **Tag git known-good** del sdkconfig ANTES de tocar nada; diff de sdkconfig revisado.
- Todo aislado en el componente `ambient_light`.
- **Plan por fases:** la Fase 1 valida el bring-up (que el DSI sigue OK) sin tocar la
  logica de brillo.

## 8. Plan por fases

- **Fase 0 — preparacion:** tag known-good sdkconfig; confirmar pines de camara
  (reset/PWDN/MCLK/CSI) del board; anadir esp_video/esp_cam_sensor (version compatible 5.4.4).
- **Fase 1 — solo medir:** componente `ambient_light` que inicia la camara y **loguea**
  la luma media (y opcional: mostrarla en About/debug), SIN tocar el backlight.
  *Verificar:* la pantalla DSI sigue funcionando; la luma sube/baja coherente al cambiar
  la luz de la cabina.
- **Fase 2 — controlar:** integrar en la arbitracion + toggle UI + suavizado/mapeo +
  fallback. *Verificar:* el brillo sube con luz y baja en penumbra sin parpadeo; nocturno
  y salvapantallas siguen mandando; si la camara falla, cae a manual.

## 9. Criterios de exito / verificacion

- Fase 1: la camara arranca **sin romper el DSI**; `ambient_light_get_raw_luma()` responde
  coherentemente a cambios de luz reales.
- Fase 2: con auto ON, el brillo se adapta de forma suave (sin parpadeo); la precedencia
  (salvapantallas > nocturno > auto > manual) se respeta; con la camara desconectada/KO el
  brillo cae a manual sin dejar la pantalla a oscuras.

## 10. Compatibilidad confirmada

- ESP-IDF: **5.4.4** (sin actualizar).
- `esp_video` v2.2.0 requiere `idf >= 5.4` -> OK. `esp_cam_sensor` (driver SC2336) es
  dependencia de esp_video.
- SoC: `CONFIG_SOC_MIPI_CSI_SUPPORTED=y` ya presente en el sdkconfig.
