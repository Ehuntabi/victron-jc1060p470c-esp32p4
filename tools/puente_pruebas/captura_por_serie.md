# Capturas de pantalla de la P4 por el puerto serie (25-sep-2026)

**Para qué:** bajar las capturas de la P4 **sin red**. El camino normal es
`captura_pantallas.py` (pide `/captura?n=<i>` al portal), pero eso exige que el PC
esté en el AP de la P4: si el dongle Wi-Fi se cae del bus o el puente ESP32 no
está enchufado, no hay forma. La placa sí está enchufada por USB, así que se
manda ella misma las capturas por la consola.

## Lo que se probó (y por qué se quedó en JPEG)

| Intento | Resultado |
|---|---|
| BMP entero en base64 (1,8 MB → 2,4 MB) | funciona pero la consola de la P4 va a **~6 KB/s**: **7 minutos por captura** |
| RLE antes de base64 | **peor**: la interfaz tiene degradados y crecía al **193 %** |
| **JPEG por hardware** (el codificador que ya usa la cámara) | **~100 KB por captura, 13 s cada una** ✓ |

## Receta

1. En `main/main.c`, añadir una tarea **solo de banco** que:
   - espere ~25 s (dejar terminar el arranque),
   - para cada pantalla: `ui_tour_goto_screen(n)` → esperar 2,5 s →
     `screenshot_take_bmp(&bmp, &len)`,
   - convierta el BMP (24 bits, filas de abajo arriba, cabecera de 54 bytes) a
     **RGB565**,
   - lo codifique con `camera_encode_rgb565_jpeg(rgb, 1024, 600, 85, &jpg, &len)`,
   - y vuelque el JPEG en base64 por la consola: líneas de 64 caracteres
     marcadas con `#`, acumulando **4 KB por escritura** (una escritura por línea
     es lo que lo hacía lento), con un `CAPTURA <n> <nombre>` delante y un
     `FINB64 <bytes> <crc32>` detrás.
2. En el PC, leer `/dev/ttyACM0` (abrir el puerto **reinicia** la P4, así que el
   volcado empieza 25 s después), quedarse con las líneas que empiezan por `#`,
   decodificar, comprobar **tamaño y CRC** y guardar el `.jpg`.
3. Al acabar, **quitar la tarea y volver a grabar el firmware publicado**.

Índices del tour (los mismos nombres que el carrusel, ver `capture_carousel.c`):
14 frigo · 15 logs · 16 wifi · 17 display · 18 tarjeta_sd · 19 sonido ·
20 autocaravana · 21 victron_keys · 22 gps · 23 about.

## Trampas

- **Un lector de log en segundo plano deja el puerto cogido** y esptool falla con
  *"the chip stopped responding"* o *"multiple access on port"*. Matar por PID
  (`fuser -k /dev/ttyACM0`) antes de grabar.
- `pkill -f <patrón>` **se mata a sí mismo** si el patrón aparece en la propia
  línea de comandos (pasa con el nombre del script). Usar `fuser -k` o el truco
  del corchete (`patró[n]`).
- La captura de `/captura?n=` navega la pantalla **física**: el tour deja la P4 en
  esa página. Con esto da igual (se acaba grabando el firmware publicado), pero
  conviene saberlo.
