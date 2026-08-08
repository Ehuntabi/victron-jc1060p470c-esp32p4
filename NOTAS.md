## v1.4.4

Fix: contención SDMMC (INT_WDT) en los flushes periódicos de `datalogger`,
`battery_history` y `ne185_vlog`.

Los tres módulos comprobaban la cabecera del CSV con `stat()` **antes** de
tomar `camera_sd_bus_lock()`, saltándose la protección contra contención
SDMMC (cámara ↔ SD) que el resto del firmware aplica sin excepción. Los tres
flushes corren cada 60 s en segundo plano, todo el día. Movido el `stat()`
dentro del cerrojo en los tres.

De paso, `dashboard_state.c` (la caché que alimenta `/api/state`, usada por
la app) deja de usar `portMAX_DELAY` en su mutex — ahora acotado a 100 ms,
consistente con el resto del código, para no poder bloquearse nunca de forma
indefinida.
