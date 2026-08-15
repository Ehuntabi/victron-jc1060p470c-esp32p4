## v1.5.3

Nueva función: la card "DC/DC" del mini (segundo display ESP32-C6 de 1,47")
pasa a llamarse "BATERIA MOTOR" y muestra el canal auxiliar del SmartShunt
(tensión de arranque/motor, punto medio o temperatura, según cómo esté
configurado el propio shunt) en vez de la tensión de entrada del Orion
DC/DC. Mismo dato que ya se veía en el 7" (Ajustes → Batería), ahora
también en el mini. Confirmado en la placa con el motor arrancado y
parado: la tensión sigue correctamente los dos estados.

Requiere flashear también el mini (protocolo UDP entre los dos display
sube de versión); si el mini se queda en v1.5.2 o anterior, descarta los
paquetes del 7" por versión distinta y no actualiza esa card.

## v1.5.2

Arreglo real: la actualización de firmware por Wi-Fi (OTA) se reiniciaba a
medias (a un % distinto cada vez, sin completar nunca), con cualquier
versión. Causa real: al escribir en flash durante la OTA, ESP-IDF bloquea
temporalmente al núcleo que no está escribiendo (para proteger la caché
compartida) — si ese bloqueo se alarga por la carga concurrente del resto
del sistema (cámara, Bluetooth, tarjeta SD), el vigilante de tareas de
FreeRTOS lo detecta como colgado y reinicia la placa. Con
`CONFIG_SPIRAM_XIP_FROM_PSRAM` el código deja de necesitar la caché de
flash durante ese bloqueo (se ejecuta desde PSRAM en su lugar), y se sube
también el margen del vigilante de 5 a 10 segundos por seguridad extra.
Confirmado en la placa: OTA completa sin reiniciar.

También: aviso fijo en pantalla ("Actualizando firmware, no apagues la
pantalla") durante toda la subida, con el táctil bloqueado para que no se
pueda tocar nada mientras dura.

## v1.5.1

Sin cambios de cara al usuario: continuación del mismo pase de
reorganización de v1.5.0.

- `main/` (fuera de `ui/`) agrupado por dominio: `main/portal/`
  (`config_server*.c/h`, `charts_svg.c`, `data_export_tar.c`,
  `ota_update.c`) y `main/data/` (`dashboard_state.c`, `energy_today.c`,
  `solar_daily.c`, `trip_computer.c`). Solo movimiento de ficheros.
- Los `.md` de la raíz agrupados: `INSTALACION.md`/`DESARROLLO.md`
  movidos a `docs/`, índice nuevo en el README.
- CI: build automático (`idf.py build`) en cada push/PR via GitHub
  Actions, para detectar errores de compilación antes de llegar a la
  placa.

## v1.5.0

Sin cambios de cara al usuario: auditoría completa pre-release (funcional,
seguridad, refactor y documentación) más continuación de la reorganización
interna del código.

- `config_server.c` partido en handlers HTTP + `config_server_ap.c` (ciclo
  de vida del AP Wi-Fi: radio, timers de auto-off, cola de trabajos):
  1336 → 748 líneas.
- `main/ui/` (49 ficheros sueltos) agrupado por tema en subcarpetas:
  `devices/`, `history/`, `settings/`, `views/`, `vigilancia/`, `widgets/`.
  Solo movimiento de ficheros, sin cambios de lógica.
- Documentación desactualizada corregida: número de versión en README/
  CLAUDE.md, y datos de hardware erróneos en VICTRON-CONTEXT.md (ventilador
  listado en GPIO21 cuando es GPIO5, RTC listado como RX8130 cuando es
  RX8025T).

## v1.4.28

Arreglo real: las miniaturas de la galería de vigilancia (Ajustes → Tarjeta
SD → Ver galería, o `/vigilancia` en el portal) salían siempre rotas
(icono roto en vez de la foto), aunque el listado con fechas se veía bien.
Llevaba así desde el 10-ago (migración al esquema de carpetas por sesión):
un filtro de seguridad interno rechazaba cualquier nombre de fichero que
llevara letras, y **todos** llevan ".jpg" al final.

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
