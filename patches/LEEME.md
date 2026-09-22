# Parches al ESP-IDF

Aquí van los arreglos **de ESP-IDF** (no del proyecto) que hacen falta para
publicar, con el motivo y las pruebas. `release.sh` los aplica solo, de forma
idempotente, antes de compilar.

## `idf-5.5.5-spi-null-memcpy.patch`

**Qué es:** el arreglo oficial de Espressif
[`6c1f0e7b59`](https://github.com/espressif/esp-idf/commit/6c1f0e7b59e) —
*"fix(driver_spi): avoid NULL memcpy when private DMA buffer setup fails"*
(28-jul-2026, PR [#18898](https://github.com/espressif/esp-idf/pull/18898)).
Toca 3 líneas de `components/esp_driver_spi` (`spi_master.c`, `spi_slave.c`,
`spi_slave_hd.c`).

**Por qué hace falta aquí:** el IDF fijado es **v5.5.5** (etiquetado el
16-jul-2026), así que el arreglo es **posterior**. Entra en 5.5.6: **cuando se
suba el IDF, este parche sobra y hay que borrarlo** (`release.sh` avisa y no
hace nada si el IDF ya lo lleva).

**Qué pasaba sin él (reproducido en la P4 el 22-sep-2026):** cuando se agota la
RAM interna DMA-capaz —con tráfico Wi-Fi fuerte, p. ej. sacando las capturas por
TCP— el `spi_master` no puede reservar su buffer temporal y entra en su ruta de
error. Allí `uninstall_priv_desc()` hacía `memcpy(destino, NULL, n)` porque
`buffer_to_rcv` nunca llegó a asignarse: **`Load access fault` y reinicio** en
vez de devolver el error. La cadena exacta del log, justo antes de cada panic:

```
E dma_utils: esp_dma_capable_malloc(196): Not enough heap memory
E H_SDIO_DRV: task still writing Rx data to queue!
E spi_master: setup_dma_priv_buffer(1214): Failed to allocate priv RX buffer
Guru Meditation Error: Core 0 panic'ed (Load access fault)
   MEPC: memcpy  <- uninstall_priv_desc (spi_master.c:1182)
   <- setup_priv_desc (clean_up) <- spi_device_polling_start <- sdspi_host_start_command
```

Solo se disparaba al abrir las pantallas que **leen la SD** (6, 7, 8 y a veces
21: históricos de batería/frigo/solar y Victron Keys), porque son las que meten
tráfico de tarjeta en ese momento. En uso normal no se ha visto: hace falta que
el hueco DMA baje a cero, y eso pide mucha transferencia seguida.

**Comprobado en placa (22-sep-2026), con una prueba que fuerza el fallo a
propósito** (comerse la RAM DMA hasta dejar el bloque mayor por debajo de lo que
necesita una transferencia de SD, y pedir la pantalla 6):

| IDF | Resultado |
|---|---|
| v5.5.5 sin parche | `Load access fault` + reinicio (3 veces, sin forzar nada) |
| v5.5.5 + este parche | `Failed to allocate priv TX buffer` ×7, **0 panics, 0 reinicios**, la placa sigue y suelta la memoria |

Con el parche, el fallo se degrada como debe: el driver de la SD ve el error
(`sdspi_host.c` lo comprueba y lo devuelve), la lectura falla y esa pantalla se
queda sin datos. Nada de reiniciar.
