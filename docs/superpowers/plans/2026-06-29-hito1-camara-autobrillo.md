# Hito 1: Camara viva + auto-brillo — Plan de implementacion

> **Para ejecutores agenticos:** SUB-SKILL REQUERIDA: usar superpowers:subagent-driven-development
> (recomendado) o superpowers:executing-plans para implementar tarea a tarea. Los pasos usan
> checkbox (`- [ ]`).

**Goal:** Levantar la camara SC2336 (servicio reutilizable) y usar la luz ambiente medida por
la camara para ajustar automaticamente el brillo de la pantalla.

**Architecture:** Componente `camera` (esp_video + esp_cam_sensor, owner del pipeline CSI) que
entrega frames; componente `ambient_light` que consume frames -> media de luma -> % de brillo,
e integra con la arbitracion de brillo existente. Toggle en Settings->Display.

**Tech Stack:** ESP-IDF 5.4.4, esp_video v2.2.0 + esp_cam_sensor (driver sc2336), LVGL, C.

## NOTA sobre el "test cycle" en este plan (firmware embebido)
No hay framework de unit-test sobre hardware para CSI/DSI/backlight. El "test" de cada tarea es
**verificacion en la P4 conectada**: `idf.py -p /dev/ttyACM1 flash monitor` (o /dev/ttyACM0) y
observar el **log serie** y/o el comportamiento de la pantalla. Cada tarea termina en un estado
verificable en hardware y un commit.

## Global Constraints (copiar verbatim del entorno)
- ESP-IDF **5.4.4** exacto (no actualizar). Install: `~/.espressif/esp-idf-5.4`. `. ~/esp/esp-idf/export.sh` si se pierde el entorno.
- Compilar `idf.py build`; flash `idf.py -p /dev/ttyACM1 flash` (o ttyACM0); monitor `idf.py -p <port> monitor`.
- Textos UI en espanol, sin emojis (salvo LV_SYMBOL_*). Logs sin acentos. NVS por componente (namespace propio).
- NO tocar capabilities DSI del sdkconfig sin diff y tag known-good previo (DSI se ha roto antes).
- Datos de camara del board: SC2336 SCCB **I2C 0x36**, **2 carriles MIPI-CSI**, **MCLK 19.2 MHz**,
  reset/PWDN en **GPIO 43/44** (confirmar cual es cual en Fase 0), chip id 0x5602.

## File structure
- Create `components/camera/camera.h` — API del servicio camara.
- Create `components/camera/camera.c` — bring-up esp_video/SC2336 + acceso a frames.
- Create `components/camera/CMakeLists.txt`, `components/camera/idf_component.yml` (dep esp_video/esp_cam_sensor).
- Create `components/ambient_light/ambient_light.h` / `.c` / `CMakeLists.txt` — luma -> % brillo.
- Modify `main/main.c` — `night_mode_timer_cb`: usar ambient_light cuando auto ON.
- Modify `main/ui/settings_panel.c` (pagina Display) — toggle "Brillo automatico".
- Modify `main/config_backup.c` / `.h` — persistir flag auto-brillo en NVS.
- Modify `sdkconfig.defaults` (o sdkconfig) — habilitar CSI/esp_video (con tag known-good antes).

---

## FASE 0 — Preparacion (sin riesgo para la pantalla)

### Task 0.1: Tag known-good del estado actual
**Files:** ninguno (git).
- [ ] **Step 1:** Confirmar build limpio actual: `idf.py build` -> Expected: build OK.
- [ ] **Step 2:** Crear tag de seguridad del sdkconfig/codigo bueno:
```bash
git tag sdkconfig-known-good-2026-06-29-pre-camara
git push origin sdkconfig-known-good-2026-06-29-pre-camara
```
- [ ] **Step 3:** Flashear y verificar que la pantalla DSI + touch funcionan HOY (linea base):
`idf.py -p /dev/ttyACM1 flash monitor` -> Expected: UI arranca, touch responde, sin panics.

### Task 0.2: Confirmar pinmap de camara del board
**Files:** Create `docs/camara_sc2336_pinmap.md` (anotacion).
- [ ] **Step 1:** Obtener la config de camara del board de la fuente autoritativa (repo
  `sukesh-ak/JC1060P470C_I_W-GUITION-ESP32-P4_ESP32-C6` y/o el board config de esp_video):
  CSI ctlr_id, data_lane_num=2, lane_bit_rate_mbps, MCLK gpio + 19.2 MHz, reset gpio, pwdn gpio,
  SCCB I2C port/pins + addr 0x36.
- [ ] **Step 2:** Anotar los valores confirmados en `docs/camara_sc2336_pinmap.md`.
- [ ] **Step 3:** Commit:
```bash
git add docs/camara_sc2336_pinmap.md
git commit -m "docs(camara): pinmap SC2336 confirmado del board"
```

### Task 0.3: Anadir dependencias esp_video / esp_cam_sensor
**Files:** Create `components/camera/idf_component.yml`.
- [ ] **Step 1:** Declarar dependencias (version compatible con idf 5.4.4):
```yaml
dependencies:
  idf: ">=5.4"
  espressif/esp_video: "^2.2.0"
  espressif/esp_cam_sensor: "*"
```
- [ ] **Step 2:** `idf.py reconfigure` -> Expected: descarga esp_video/esp_cam_sensor sin error de version.
- [ ] **Step 3:** Habilitar SOLO el driver SC2336 y el CSI en menuconfig/sdkconfig.defaults
  (esp_cam_sensor -> SC2336 = y; esp_video MIPI-CSI = y). Diff de sdkconfig revisado: que NO
  cambien las claves DSI (`LCD_DSI_*`).
- [ ] **Step 4:** `idf.py build` -> Expected: compila incluyendo esp_video.
- [ ] **Step 5:** Commit:
```bash
git add components/camera/idf_component.yml sdkconfig.defaults
git commit -m "feat(camara): anadir esp_video + driver sc2336 (sin tocar DSI)"
```

---

## FASE 1 — Solo medir (validar bring-up sin tocar el backlight)

### Task 1.1: Servicio `camera` minimo (init + un frame)
**Files:** Create `components/camera/camera.h`, `camera.c`, `CMakeLists.txt`.
**Interfaces — Produces:**
- `esp_err_t camera_init(void);`
- `bool camera_ok(void);`
- `esp_err_t camera_get_frame(uint8_t **buf, size_t *len, uint32_t *w, uint32_t *h);` (formato con luma accesible)
- `void camera_return_frame(void);`
- [ ] **Step 1:** Implementar `camera_init` abriendo el dispositivo V4L2 de esp_video con el
  sensor SC2336 a la **resolucion mas baja util** y formato con componente Y accesible (p.ej. RAW8/GREY
  o YUV), pocos fps, usando los valores del pinmap de Task 0.2.
- [ ] **Step 2:** Implementar `camera_get_frame`/`camera_return_frame` (dequeue/enqueue de buffer V4L2).
- [ ] **Step 3:** En `app_main` (temporal, tras init de display): `camera_init()` y loguear el
  resultado + chip id detectado.
- [ ] **Step 4 (verificacion HW):** `idf.py -p /dev/ttyACM1 flash monitor`.
  Expected en el log: sensor SC2336 detectado (chip id 0x5602), `camera_ok()==true`, **y la
  pantalla DSI sigue funcionando** (UI visible, sin panic, sin corrupcion). Si el DSI se rompe -> STOP,
  volver al tag known-good y revisar convivencia CSI/DSI ISR antes de seguir.
- [ ] **Step 5:** Commit:
```bash
git add components/camera/
git commit -m "feat(camara): servicio camara minimo (init + get_frame), DSI intacto"
```

### Task 1.2: `ambient_light` — media de luma y log
**Files:** Create `components/ambient_light/ambient_light.h`, `ambient_light.c`, `CMakeLists.txt`.
**Interfaces — Produces:**
- `esp_err_t ambient_light_init(void);`
- `bool ambient_light_ok(void);`
- `int ambient_light_get_raw_luma(void);` (0..255, -1 si no hay lectura)
**Consumes:** `camera_get_frame` / `camera_return_frame` de Task 1.1.
- [ ] **Step 1:** Implementar `ambient_light_init` que arranca una task periodica (cada 2500 ms):
  pide frame a `camera`, calcula **media de la componente Y** sobre el frame (submuestreando 1 de
  cada N pixeles para abaratar), guarda en `s_raw_luma`, libera el frame.
- [ ] **Step 2:** `ambient_light_get_raw_luma` devuelve `s_raw_luma`; `ambient_light_ok` true si la
  ultima lectura es valida y reciente.
- [ ] **Step 3:** La task **loguea** `I (xxx) AMBIENT: luma=NNN` cada muestra. NO toca backlight.
- [ ] **Step 4:** En `app_main`: `ambient_light_init()` tras `camera_init()`.
- [ ] **Step 5 (verificacion HW):** flash + monitor. Tapar la camara con la mano -> `luma` baja;
  acercar a luz -> `luma` sube. Expected: la lectura **sigue la luz real de la cabina** y el DSI sigue OK.
- [ ] **Step 6:** Commit:
```bash
git add components/ambient_light/ main/
git commit -m "feat(ambient_light): media de luma por camara + log (sin tocar brillo)"
```

**>>> CHECKPOINT FASE 1:** la camara arranca sin romper el DSI y la luma responde a la luz.
Aqui se decide seguir a Fase 2.

---

## FASE 2 — Controlar el brillo

### Task 2.1: Mapeo luma -> % de brillo (suavizado)
**Files:** Modify `components/ambient_light/ambient_light.c`, `ambient_light.h`.
**Interfaces — Produces:**
- `int ambient_light_get_pct(void);` (0..100 suavizado; -1 si no valido)
- [ ] **Step 1:** Anadir EMA sobre la luma: `s_ema = a*luma + (1-a)*s_ema` (a=0.2).
- [ ] **Step 2:** Mapear `s_ema` (0..255) a brillo en `[BRI_MIN=15, BRI_MAX=100]` (lineal con clamp),
  con **banda muerta**: solo actualizar `s_pct` si `|nuevo - s_pct| > 3`.
- [ ] **Step 3:** Loguear `pct` junto a `luma`.
- [ ] **Step 4 (verificacion HW):** flash + monitor. Expected: `pct` sube/baja suave con la luz, sin saltos bruscos.
- [ ] **Step 5:** Commit `feat(ambient_light): mapeo luma->pct con EMA y banda muerta`.

### Task 2.2: Flag de auto-brillo en NVS + API
**Files:** Modify `main/config_backup.c`/`.h` (o el storage de display existente).
**Interfaces — Produces:** `bool auto_brightness_get(void);` `void auto_brightness_set(bool);` (persistente NVS).
- [ ] **Step 1:** Anadir clave `auto_bright` (u8) al namespace de display; getters/setters con commit NVS.
- [ ] **Step 2:** Incluir el flag en el export/import JSON de `config_backup` (como brightness).
- [ ] **Step 3 (verificacion HW):** set true, reboot, comprobar que persiste (log).
- [ ] **Step 4:** Commit `feat(display): persistir flag de brillo automatico en NVS`.

### Task 2.3: Integrar en la arbitracion de brillo
**Files:** Modify `main/main.c` (`night_mode_timer_cb`).
**Consumes:** `auto_brightness_get`, `ambient_light_ok`, `ambient_light_get_pct`.
- [ ] **Step 1:** Localizar el calculo de `target` en `night_mode_timer_cb` (tras los chequeos de
  screensaver y franja nocturna). Insertar ANTES del fallback a `ui->brightness`:
```c
/* Auto-brillo por camara: solo si esta activado y la camara da lectura valida.
   El screensaver y la franja nocturna ya tienen precedencia arriba. */
if (auto_brightness_get() && ambient_light_ok()) {
    int p = ambient_light_get_pct();
    if (p >= 0) target = p;
}
```
- [ ] **Step 2 (verificacion HW):** con `auto_bright=true`, fuera de franja nocturna y sin screensaver:
  tapar la camara -> la pantalla **se atenua**; dar luz -> **sube**. En franja nocturna -> manda el
  brillo nocturno (auto NO pisa). Con `auto_bright=false` -> brillo manual de siempre.
- [ ] **Step 3:** Commit `feat(brillo): auto-brillo por camara integrado en la arbitracion`.

### Task 2.4: Toggle en Settings -> Display
**Files:** Modify `main/ui/settings_panel.c` (pagina Display).
**Consumes:** `auto_brightness_get`/`set`.
- [ ] **Step 1:** Anadir un switch "Brillo automatico" en la pagina Display, estilo card como el resto;
  on/off llama a `auto_brightness_set()`. Opcional: deshabilitar/atenuar el slider manual cuando auto ON.
- [ ] **Step 2 (verificacion HW):** desde la pantalla, activar el toggle -> el brillo pasa a seguir la
  camara; desactivar -> vuelve al manual. Persiste tras reboot.
- [ ] **Step 3:** Commit `feat(ui): toggle Brillo automatico en Display`.

### Task 2.5: Fail-safe y limpieza
**Files:** Modify `components/ambient_light/ambient_light.c`, `main/main.c`.
- [ ] **Step 1:** Verificar que si `camera_init`/`ambient_light_init` fallan o la camara se desconecta,
  `ambient_light_ok()` pasa a false y la arbitracion cae a `ui->brightness` (Task 2.3 ya lo cubre) +
  log de aviso una sola vez.
- [ ] **Step 2:** Quitar logs de debug de alta frecuencia (dejar uno cada N o bajo flag), para no saturar.
- [ ] **Step 3 (verificacion HW):** con `auto_bright=true`, desconectar/forzar fallo de camara ->
  la pantalla NO se queda a oscuras, cae a brillo manual. Reactivar -> vuelve auto.
- [ ] **Step 4:** Commit `feat(brillo): fail-safe a manual si la camara falla + limpieza de logs`.

---

## Verificacion final Hito 1
- DSI/touch intactos con la camara levantada.
- Auto-brillo ON: el brillo sigue la luz de la cabina, suave, sin parpadeo.
- Precedencia respetada: screensaver > nocturno > auto > manual.
- Camara KO -> brillo manual, nunca pantalla negra.
- El servicio `camera` queda disponible para Hito 2 (movimiento -> foto).
