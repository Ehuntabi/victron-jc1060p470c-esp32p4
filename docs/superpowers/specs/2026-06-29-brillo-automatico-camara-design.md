# Capacidades de camara (SC2336) — Hito 1: camara viva + auto-brillo

Fecha: 2026-06-29
Proyecto: victron-jc1060p470c-esp32p4 (Guition JC1060P470C_I_W, ESP32-P4)
Estado: diseno aprobado, pendiente de revision antes del plan.

## 0. Vision y roadmap

Habilitar la **camara integrada SC2336** (MIPI-CSI) del board. El uso final es una
**camara de vigilancia**: al detectar movimiento, guardar foto y luego video en la SD.
El brillo automatico es el **primer hito** (consumidor sencillo del pipeline) que valida
que la camara funciona bien antes de construir lo demas.

- **Hito 1 (este spec):** levantar el pipeline de camara (servicio reutilizable) +
  **auto-brillo** segun luz ambiente (media de luma del frame).
- **Hito 2 (futuro):** **deteccion de movimiento -> foto JPEG** con fecha/hora a `/sdcard`.
- **Hito 3 (futuro):** **movimiento -> video H.264** a la SD (encoder HW del ESP32-P4).

Por que la camara y no un ALS dedicado: el objetivo real es vigilancia (uso de camara
legitimo) y no se dispone de sensor de luz; levantar la camara una vez sirve a los 3 hitos.

## 1. Arquitectura (Hito 1)

Dos componentes nuevos y aislados, pensados para que H2/H3 reutilicen el pipeline:

### `components/camera/` — servicio de camara (reutilizable)
Encapsula esp_video + esp_cam_sensor (SC2336). Es el unico que conoce el pipeline CSI.
- `esp_err_t camera_init(void)` — inicia el sensor a baja resolucion / pocos fps.
- `esp_err_t camera_deinit(void)`.
- `bool camera_ok(void)`.
- Acceso a frames para consumidores (get-frame/return-frame o callback). En H1 solo se
  usa para calcular luma; H2/H3 usaran los mismos frames para movimiento/codificacion.

### `components/ambient_light/` — consumidor (auto-brillo)
- `int  ambient_light_get_pct(void)` — % de brillo objetivo suavizado (0..100), o -1.
- `bool ambient_light_ok(void)`.
- `int  ambient_light_get_raw_luma(void)` — media de luma 0..255 (Fase 1 / debug).
- Task periodica (~2-3 s): pide frame al servicio `camera`, calcula **media de luma Y**,
  aplica **EMA + banda muerta**, mapea a % con suelo/techo y rampa suave.
- Constantes ajustables (periodo, alfa EMA, BRI_MIN/BRI_MAX, umbral, curva). Afinar con datos reales.

## 2. Comportamiento del auto-brillo (acordado)

Toggle "Brillo automatico" (Settings -> Display, NVS). Precedencia:
`salvapantallas (atenua) > modo nocturno (franja, brillo fijo) > auto (camara) > brillo manual`.

### Integracion (cambio quirurgico en `night_mode_timer_cb`, main/main.c)
```
if (screensaver.active)                 -> brillo atenuado screensaver  (sin cambios)
else if (night_mode en franja)          -> ui->night_mode.brightness     (sin cambios)
else if (auto ON && ambient_light_ok()) -> ambient_light_get_pct()       (NUEVO)
else                                    -> ui->brightness (manual)        (sin cambios)
```

## 3. Robustez y errores

- **Camara KO** (no inicia / deja de dar frames): `ambient_light_ok()` -> false ->
  arbitracion cae a **brillo manual**. La pantalla NUNCA se queda a oscuras ni depende de la camara.
- **Anti-parpadeo:** EMA + banda muerta + rampa; `bsp_display_brightness_set` ya solo aplica si cambia.

## 4. Riesgo principal y mitigacion

Bring-up de la camara sobre firmware fragil: anadir esp_video implica cambios de **sdkconfig**
(habilitar CSI, buffers PSRAM, driver). El proyecto esta pinado a 5.4.4 porque otras versiones
rompian el **MIPI-DSI**; hay que vigilar la convivencia ISR de CSI vs DSI
(`CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE` hoy en n). Existe un issue de Espressif sobre el
bring-up del SC2336 en P4 (pines reset/PWDN/MCLK del board concreto, a confirmar).

Mitigaciones:
- **Tag git known-good** del sdkconfig ANTES de tocar.
- Todo aislado en componentes `camera` / `ambient_light`.
- **Plan por fases** y validacion en la **P4 conectada** (/dev/ttyACM0): la Fase 1 confirma que
  el DSI sigue sano con la camara levantada, ANTES de tocar la logica de brillo. Reflasheable
  al firmware bueno (reversible).

## 5. Plan por fases (Hito 1)

- **Fase 0 — preparacion:** tag known-good sdkconfig; confirmar pines de camara (reset/PWDN/MCLK/CSI)
  del JC1060P470C (esquematico / wiki SpotPear); anadir esp_video/esp_cam_sensor (version compatible 5.4.4).
- **Fase 1 — solo medir:** servicio `camera` + `ambient_light` leyendo y **logueando** la luma
  (opcional: mostrarla en About/debug), SIN tocar el backlight. *Verificar en la P4:* el DSI sigue
  OK; la luma sube/baja coherente al cambiar la luz de la cabina.
- **Fase 2 — controlar:** integrar en la arbitracion + toggle UI + suavizado/mapeo + fallback.
  *Verificar:* brillo suave sin parpadeo; precedencia respetada; camara KO -> manual.

## 6. Criterios de exito (Hito 1)

- Fase 1: la camara arranca **sin romper el DSI**; `ambient_light_get_raw_luma()` responde a la luz real.
- Fase 2: con auto ON el brillo se adapta suave; precedencia respetada; camara KO -> manual sin pantalla negra.

## 7. Compatibilidad

- ESP-IDF **5.4.4** (sin actualizar). `esp_video` v2.2.0 requiere `idf >= 5.4` -> OK (verificado).
  `esp_cam_sensor` (driver SC2336) es dependencia de esp_video.
- `CONFIG_SOC_MIPI_CSI_SUPPORTED=y` ya presente. SD (SDMMC slot 0) y encoder H.264 HW disponibles
  para H2/H3.

## 8. Fuera de alcance de este spec (Hito 1)
Deteccion de movimiento, captura de foto/video, almacenamiento/retencion en SD, avisos. Se
disenaran en los specs de Hito 2 y 3, reutilizando el servicio `camera`.
