## v1.4.9

Un solo arreglo, motivado por la app móvil: `/data/bateria.csv` (y las
gráficas de Batería del portal) salían vacías la mayor parte del día.

`read_file_to_buf` (compartida por Frigo/Batería/NE185) limitaba la lectura
a 512 KB reservados en RAM interna. El CSV de Frigo nunca lo toca (~288
líneas/día), pero el de Batería registra 4 fuentes cada ~10 s — unas 34.500
líneas/día, ~1,5 MB. A partir de aproximadamente un tercio del día el
fichero ya superaba los 512 KB y el endpoint devolvía vacío el resto,
sistemáticamente, hasta el día siguiente.

Límite subido a 3 MB, reservados en PSRAM (la RAM interna no tiene margen
para un buffer de ese tamaño, compartida con LVGL/WiFi/cámara). Esto es lo
que necesitaba la app del móvil para poder mostrar las 4 fuentes de
batería (BatteryMonitor, SolarCharger, OrionTR, ACCharger) del día
completo en vez de solo el primer tercio.
