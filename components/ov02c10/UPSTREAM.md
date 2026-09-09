# Origen y parches locales

**Upstream**: PR abierto (sin mergear a fecha 09-sep-2026) en
`espressif/esp-video-components`:

- PR: https://github.com/espressif/esp-video-components/pull/46
  ("feat(esp_cam_sensor): Add OV02C10 MIPI camera sensor driver (AEGHB-1263)")
- Rama origen: `csvke:feat/add-ov02c10-sensor` (fork de `csvke`)
- Commit HEAD portado: `ed9f3ddc8bff764b02a0b7c22dd7179b99bdb6a3`
- Verificado con `gh api repos/espressif/esp-video-components/pulls/46` el
  09-sep-2026 — el PR seguía abierto, sin mergear.

No es un paquete del Component Registry (a diferencia de
`components/esp_lcd_jd9165`): se copiaron los ficheros del PR directamente
al repo (commit `a15cdde`, 29-jun-2026, "driver OV02C10 portado del kernel
Linux" — el propio PR de Espressif es a su vez un port del driver del
kernel Linux, de ahí el mensaje).

## Parches locales sobre lo que trae el PR

- `ov02c10.c` líneas 963, 978 y 993: tres valores de `tline_ns` (tiempo por
  línea = `1e9/(fps*lineas)`) que **faltan en el PR original** — sin ellos
  el timing de exposición sale mal. Confirmado ausentes al comparar contra
  el diff del PR (`gh api .../pulls/46` con `Accept: application/vnd.github.diff`,
  o `https://github.com/espressif/esp-video-components/pull/46.diff`).

## Procedimiento de re-sync

1. Comprobar si el PR #46 ya se mergeó (`gh pr view 46 -R espressif/esp-video-components`).
   Si se mergeó: valorar migrar a la versión oficial del Component Registry
   en vez de seguir manteniendo este port a mano.
2. Si sigue abierto pero con commits nuevos: `gh pr diff 46 -R espressif/esp-video-components`
   contra el HEAD anotado arriba, para ver qué cambió upstream desde
   `ed9f3dd`.
3. Revisar si los tres `tline_ns` siguen faltando en la versión nueva del
   PR — si Espressif los añadió, se puede quitar el parche local.
4. Actualizar el hash de HEAD portado en este fichero tras cualquier
   resincronización.

Relacionado: el workaround BTA de `main/esp_bsp.c:33-53` (deshabilita
`cmd_ack` del DBI IO para el panel JD9165) no viene de este PR — es un
problema aparte del panel, no de la cámara — pero vive en el mismo fichero
de bring-up (`esp_bsp.c`) y también hay que revalidarlo en cada bump de
IDF (tiene sus propios `_Static_assert` que fallan la compilación solos si
el layout privado de IDF cambia).
