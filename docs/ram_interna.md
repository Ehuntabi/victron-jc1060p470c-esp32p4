# RAM interna de la P4: dónde se va y qué se puede hacer

Lo que va justo no es la flash (quedan 868 KB en la partición de la app), es la
**RAM interna**: de los 576 KB que el enlazador deja para la aplicación, 233 KB se
quedan fijos al arrancar y con todo funcionando quedaban **61 KB libres** (medido en
la pantalla *Acerca de* el 22-sep-2026, v2.43). La PSRAM, en cambio, va al 15 %
(27,2 MB libres de 32).

## El reparto, medido

Del informe de tamaño de la compilación (`idf.py size`, v3.35):

| Sección | Tamaño | Qué es | ¿Se toca? |
|---|---|---|---|
| `.iram0.text` | 135 KB | Código que **tiene que** correr desde RAM interna: init de PSRAM y caché, el asignador TLSF, manejadores de interrupción | No |
| `.dram1.bss` + `.dram0.bss` | 74 KB | Variables estáticas | Sí, en parte |
| `.dram0.data` | 11 KB | Variables inicializadas | Poco margen |
| `.flash.text` / `.flash.rodata` | 2,7 MB | Código y constantes, en flash | No ocupan RAM |

Lo grande que es nuestro, símbolo a símbolo:

| Buffer | Tamaño | Fichero | Para qué |
|---|---|---|---|
| `s_flush_snapshot` | 14,0 KB | `datalogger.c` | Volcado del registro a la SD |
| `s_dias` | 7,8 KB | `solar_daily.c` | Historial diario del solar |
| `s_buf` | 7,8 KB | `datalogger.c` | Línea del CSV |
| `s_files` | 6,0 KB | `gallery.c` | Lista de fotos |
| `buf.0` | 4,0 KB | `config_server_vigilancia.c` | JSON de vigilancia |
| `s_buf` | 2,5 KB | `ne185_vlog.c` | Registro del NE185 |
| `ses.2` + `sd_names` | 3,5 KB | `config_server_vigilancia.c` | Sesiones y nombres |
| `s_page_ctxs` | 1,5 KB | `settings_panel.c` | Contextos de la pantalla |
| `s_crudo` | 1,5 KB | `gps.c` | Últimas tramas del GPS |

Suma: **unos 48 KB movibles** a PSRAM sin que se note.

El resto de la RAM interna no son datos, es **heap en marcha**: las pilas de las
tareas (entre todas piden unos 175 KB en los `xTaskCreate`), los buffers de Wi-Fi,
Bluetooth y del enlace con el C6, y los de TLS cuando hay HTTPS.

## Qué se puede hacer, por orden de riesgo

1. **Diagnóstico (ya hecho, v3.36).** *Acerca de* enseña ahora `int X KB (min Y KB)`:
   el mínimo desde que arrancó es el número que importa, porque el libre de ahora
   mismo puede estar alto por no haber nada abierto. El log periódico
   (`heap_log_cb`) lleva lo mismo. **Sin esto no se puede medir nada de lo que sigue.**

2. **mbedTLS a PSRAM** — `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`. Los buffers de TLS
   (16 KB de entrada y 4 KB de salida por conexión) se van a PSRAM: unos **20 KB**
   mientras hay una sesión HTTPS (portal, OTA), que es justo cuando aprieta.
   Riesgo bajo; comprobar que el portal y la OTA siguen funcionando.

3. **Buffers estáticos a PSRAM** — unos **48 KB**. Hace falta activar
   `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` y poner `EXT_RAM_BSS_ATTR` en cada
   variable de la tabla de arriba. **Cuidado con dos cosas**: ninguna puede usarse
   dentro de una interrupción, y si el DMA lee o escribe directamente en ellas (el
   datalogger y la galería escriben en la SD) hay que comprobar que el driver hace
   el mantenimiento de caché que toca. Probar a grabar en la SD después del cambio.

4. **Pilas de tareas a PSRAM** — hasta **100 KB**, el cambio que más libera. Ya está
   permitido en la configuración (`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`):
   basta cambiar `xTaskCreate` por `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)` en
   las tareas normales. **No** en las de Wi-Fi y Bluetooth ni en las que puedan
   correr con la caché apagada. Cuesta un poco más de latencia al cambiar de tarea.

5. **`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`: de 16 KB a 4 KB.** Todo lo que se pide al
   heap por debajo de ese tamaño cae hoy en RAM interna; bajándolo, lo mediano (los
   objetos de LVGL, los JSON del portal) se va a PSRAM y el heap interno respira.
   Es el cambio que más se nota y el que más hay que probar: algunos drivers
   esperan memoria interna sin decirselo.

6. **Recortar `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN`** (16 KB → 8 KB): unos 8 KB más por
   conexión, pero puede romper el apretón de manos si el otro extremo manda
   certificados grandes. Solo si con todo lo anterior no llega.

## Cómo se mide

- **En la pantalla**: *Ajustes → Acerca de*, el `min` de la RAM interna.
- **En el log**: la línea periódica del `heap_log_cb`, que ya lleva interno libre y
  mínimo, y se puede leer desde el portal o desde el fichero de la SD.
- **Cuándo**: con la **cámara abierta y la galería cargando**, que es el momento de
  más consumo. Un dato tomado con todo cerrado no vale.
