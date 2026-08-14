## v1.4.27

Sin cambios de cara al usuario: sigue la reorganización interna del código
(mismo patrón que v1.4.25/v1.4.26).

- `config_server.c` partido en `config_server_vigilancia.c` (galería
  `/snapshot` y `/vigilancia`) y `config_server_auth.c` (Basic Auth del
  portal + servido de SPIFFS): 1897 → 1336 líneas.
- `app_main()` en `main.c` descompuesto en 10 funciones `init_*()`, una por
  fase del arranque: 294 → 16 líneas.
- Limpieza de auditoría previa a esta Release: 11 declaraciones muertas más
  en `settings_panel.c` (restos de una extracción anterior a
  `settings_display.c`).

## v1.4.26

Sin cambios de cara al usuario: reorganización interna del código de
Ajustes (mismo comportamiento). `settings_panel.c` había vuelto a crecer
hasta 2394 líneas mezclando la página "Acerca de", "Sonido y alertas", el
Trip computer y el modo ausente; se separan a sus propios ficheros
siguiendo el mismo patrón ya usado para Wi-Fi, Pantalla y Victron Keys.

## v1.4.25

Sin cambios de cara al usuario: `ui.c` seguía siendo un fichero grande tras
el recorte anterior; se sacan 3 bloques autocontenidos (seguimiento de
dispositivos BLE, carrusel de capturas por Wi-Fi/SD, y la alarma de SoC
crítico/aviso) a sus propios ficheros.

## v1.4.24

Arreglo de estabilidad: el portal Wi-Fi podía reiniciar la pantalla sola
bajo carga (cámara + BLE a la vez) al recibir peticiones a rutas que no
existen en la memoria SPIFFS, porque cada una escaneaba toda la partición.
Ahora se compara antes contra una lista en RAM, sin tocar flash.

## v1.4.22 / v1.4.23

Arreglo de estabilidad: una actualización por Wi-Fi (OTA) larga podía
reiniciar la pantalla sola a medias, con el firmware nuevo a punto de
terminar de grabarse. La cámara dejaba de avisar al vigilante del sistema
(Task Watchdog) mientras duraba la escritura en flash; ahora se la
desconecta de ese vigilante solo durante la actualización.

## v1.4.21

Arreglo de estabilidad: en redes rápidas, una actualización por Wi-Fi (OTA)
podía monopolizar un núcleo el tiempo suficiente para disparar un reinicio
a medias. Ahora se cede la CPU tras cada trozo recibido.

## v1.4.20

Arreglo interno: varios guardados en memoria (NVS) y arranques de
temporizador fallaban en silencio sin quedar registrados en el log,
dificultando diagnosticar problemas reales si volvían a pasar.

## v1.4.19

Nueva función en la galería de la tarjeta SD: **borrar carpetas de
vigilancia** directamente desde la pantalla.

Al elegir sesión/día, aparece un botón rojo "Borrar carpeta" bien separado
de "Abrir carpeta" (para no confundirlos). Pide confirmación antes de
borrar nada, y el borrado ocurre en segundo plano sin bloquear la pantalla.
