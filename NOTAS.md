v4.00 — la microSD vuelve a SDMMC: 4 líneas a 40 MHz

## Qué cambia

La tarjeta deja el modo SPI (20 MHz, 1 línea) y vuelve al controlador **SDMMC:
slot 0, bus de 4 bits, 40 MHz**. El motivo es la velocidad de escritura: por SPI
se medían **0,13 MB/s**, que hace inviable guardar vídeo o ráfagas de fotos.

## Por qué 40 MHz exactos (y no 20)

En el P4 hay **un solo controlador SDMMC** y lo comparten dos cosas: la tarjeta
(slot 0) y el **enlace SDIO con el C6**, que es la radio (slot 1, a 40 MHz según
`CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=40000`). El divisor de reloj del
periférico se programa con `sdmmc_host_set_card_clk()`, que toca el registro
**global**: 40 MHz ⇒ divisor 4, 20 MHz ⇒ divisor 8.

Por eso 40 MHz no es solo "el doble de rápido": es **la frecuencia que coincide
con la del enlace del C6**, así que al montar la tarjeta el reloj de la radio no
se mueve. Con 20 MHz (lo que hacía el código de julio) el enlace del C6 se
quedaba a la mitad mientras la tarjeta estuviera montada. La única ventana en
que el reloj baja es la identificación de la tarjeta, a 400 kHz (unos
milisegundos), y este montaje ocurre **antes** de que la radio arranque.

## Fiabilidad

El montaje se hace **una vez por arranque**, antes de `init_network()`, con tres
intentos (dos a 4 bits y el tercero a 1 bit, mismo reloj) y un corte de corriente
real a la tarjeta entre intentos.

Medido en el banco (P4 + SD32G de 32 GB):

- **6/6 arranques** montando a 4 bits y 40 MHz, con la radio arrancando después
  en los 6.
- **23 arranques consecutivos** montando sin un solo fallo (los 6 últimos, ya con
  el C6 en reset durante la identificación).

Además, mientras se identifica la tarjeta se **mantiene el C6 en reset**
(GPIO54 = `C6_CHIP_PU`, activo a nivel bajo). Esa es la causa de un fallo real
medido: arrancando justo después de flashear, con el C6 todavía en modo
streaming de la sesión anterior (un reinicio de la P4 no lo resetea), la tarjeta
contestaba CMD0/CMD8 y se quedaba muda en ACMD41: **3 intentos de 3 con
`ESP_ERR_TIMEOUT`**. Con el C6 en reset durante la identificación, monta a la
primera. esp_hosted lo suelta él solo cuando conecta.

## Velocidad medida (tarjeta SD32G, con la radio en marcha)

| Camino | Escritura | Lectura |
|---|---|---|
| SPI 20 MHz (lo anterior) | 0,134 MB/s | 0,280 MB/s |
| SDMMC, sectores crudos | 0,6 - 0,85 MB/s | **3,1 - 4,0 MB/s** |
| SDMMC, por FATFS (fopen/fwrite) | **0,49 - 0,53 MB/s** | 0,32 - 0,36 MB/s |

La lectura cruda es **14× la de SPI**; la escritura por fichero, **4×**.

**El techo lo pone la tarjeta, no el driver**: la misma lectura cruda a 20 MHz
da 3,29 MB/s y a 40 MHz 3,91 MB/s. Si el límite fuera el bus, bajar el reloj a
la mitad habría bajado la velocidad a la mitad; no baja. Con una tarjeta
decente (Clase 10/A1) estos números deberían subir bastante: **es la siguiente
prueba pendiente**.

Con lo medido, grabar vídeo MJPEG VGA a 5 fps (~200 KB/s) entra en el
presupuesto de escritura de esta tarjeta (≈40%), pero sin mucho margen.

## Pendiente / siguiente

- Probar con una tarjeta Clase 10/A1 para ver cuánto era la tarjeta y cuánto el
  camino.
- La lectura **por FATFS** apenas mejora (0,35 MB/s frente a 0,28 por SPI) aunque
  los sectores crudos vayan a 4 MB/s: hay 11× de sobrecoste en la capa de
  ficheros que no está explicado. Afecta a leer históricos/CSV desde la UI, no a
  escribir.
- El cerrojo `camera_sd_bus_lock` se queda puesto: se midió cuando la SD iba por
  SPI y con la SD en SDMMC no se ha vuelto a medir si sigue haciendo falta.
