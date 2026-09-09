# Origen y parche local

**Upstream**: `espressif/esp_lcd_jd9165`, paquete oficial del ESP Component
Registry (https://components.espressif.com/components/espressif/esp_lcd_jd9165).
Vendorizado en **v1.0.4** — ver el `CHANGELOG.md` de este mismo componente
para el historial completo de versiones upstream.

## Parche local

`esp_lcd_jd9165.c`, dentro de `panel_jd9165_init()` (comentario "FORK
LOCAL", ~línea 150): se **deshabilita la lectura del Display ID** (cmd
0x04). El upstream la hace con `rx_param`, que internamente llama a
`mipi_dsi_hal_host_gen_read_short_packet` — un busy-wait SIN timeout
esperando el FIFO. Si el panel no responde a tiempo (pasa en el Guition
JC1060P470C, variación entre lotes), el host se cuelga 5s y dispara el WDT
IDLE0. Referencia: esp-idf issue #15137. El ID solo se usaba para un log
de debug, no afecta a la operación real del panel.

## Procedimiento de re-sync (al subir de versión del paquete o de IDF)

1. Mirar si `esp-idf#15137` (o el equivalente en la versión nueva) sigue
   abierto: `gh issue view 15137 -R espressif/esp-idf`. Si Espressif lo
   arregló, probar SIN el bypass antes de asumir que sigue haciendo falta.
2. Si se sube la versión del paquete (`idf_component.yml` fija
   `esp_lcd_jd9165: "*"` hoy, sin pin — comprobar qué versión trae
   realmente el build con `idf.py build` y mirar el log del component
   manager), diff `panel_jd9165_init()` contra la `esp_lcd_jd9165.c` nueva
   del registry (bajarla aparte, NO reinstalar encima del fork) para ver
   si el código de lectura de ID cambió de sitio o de forma.
3. Actualizar la versión anotada arriba tras cualquier resincronización.

Relacionado: el workaround BTA de `main/esp_bsp.c:33-53` (deshabilita
`cmd_ack` del DBI IO porque este panel no responde con BTA) no es parte de
este componente — vive en el bring-up de la placa — pero es el mismo
panel y hay que revisarlo en el mismo momento. Ya se autoprotege con
`_Static_assert` contra un cambio de layout privado de IDF (falla la
compilación sola si hace falta mirarlo).
