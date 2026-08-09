## v1.4.8

Tanda centrada en cerrar huecos reales de INT WDT y en el histórico de
Frigo, sobre datos de placa (coredumps y fotos de vigilancia reales, no
suposiciones).

**Tres focos más de INT WDT**, todos con la misma raíz de fondo (una
sección crítica o un cerrojo de la SD retenido demasiado tiempo, contra el
GDMA de la cámara o contra el otro core):

- `esp_bsp.c` usaba un buffer de LVGL a pantalla completa para un
  `avoid_tearing` que ya estaba desactivado; cada flush hacía un
  `esp_cache_msync` de ~1,2 MB en sección crítica. Ahora el buffer es de 50
  líneas (`BSP_LCD_DRAW_BUFF_SIZE`).
- La miniatura PPA de vigilancia procesaba el frame completo (1920x1080,
  4,1 MB) de una tacada; el troceo anterior soltaba y volvía a coger el
  spinlock sin ceder CPU de verdad. Ahora son 4 franjas con
  `vTaskDelay(1)` real entre cada una. Confirmado con 3 coredumps reales
  (`httpd`, `nimble_host`) el 9 de agosto.
- `read_file_to_buf` (gráficas y CSV de Frigo/Batería/NE185 en el portal)
  leía el fichero entero — hasta 512 KB — en un solo `fread()` reteniendo
  `camera_sd_bus_lock` todo el rato. Ahora lee en trozos de 4 KB soltando
  el cerrojo entre cada uno, igual que ya hacía `tar_stream_dir`.

**Histórico de Frigo — "HOY" ya no se parte en dos.** Tras un reinicio,
"HOY" leía solo el anillo en RAM (vacío desde el arranque) y la fecha de
hoy como día navegable solo traía el CSV (hasta el último volcado antes de
reiniciar): el mismo día aparecía partido en dos vistas incompletas. Ahora
se fusionan CSV + cola del anillo, igual que ya hacía Batería. De paso,
las cabeceras de fecha de Frigo y Batería pasan de año-mes-día a
día-mes-año.

**Vigilancia — menos falsos positivos por ruido en poca luz.** Calibrado
con 300 fotos reales de una sesión en escena vacía y a oscuras: el ruido
de sensor cambiaba hasta 19 celdas de 576 (picos aislados, no un cambio de
luz global), mientras que una persona real delante de la cámara cambiaba
296-381. El umbral de disparo sube de 4 a 30 celdas.

**Portal cautivo (Android).** `/generate_204`, `/hotspot-detect.html` y
`/ncsi.txt` redirigían siempre a `/index.html` (claves AES Victron),
saltándose la página configurada en Ajustes. Ahora redirigen a `/` para
que el destino sea consistente con el resto del portal.
