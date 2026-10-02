v4.02 — la SD lee 6,6x más rápido (el búfer de stdio pedía sector a sector)

## Qué cambia

- **`CONFIG_FATFS_VFS_FSTAT_BLKSIZE=16384`**. newlib decide el tamaño del búfer
  de `stdio` con el `st_blksize` que le da el VFS de FatFS. Con el valor por
  defecto (0 → 128 B, y en la práctica 512) cada `fread`/`fgets` le pedía a FatFS
  **un sector por vez**: una transacción SDMMC de 512 B cada una (~2,5 ms de
  sobrecoste por transacción) → 0,30 MB/s. Con 16 KB (el tamaño de clúster de la
  tarjeta, que es el máximo que cabe en una transacción) cada recarga del búfer
  es UNA transacción de 16 KB.
- Los `fclose` que no se miraban (**datalogger**, **battery_history**,
  **ne185_vlog**) ahora se miran: con el búfer grande parte de la escritura
  ocurre al cerrar, así que un fallo ahí era pérdida silenciosa de muestras.

## Medido (banco, mismo fichero, solo lectura, CSV de batería de 1,26 MB)

| | antes | ahora | mejora |
|---|---|---|---|
| `fread` en trozos de 16 KB | 0,301 MB/s | **2,000 MB/s** | **6,6x** |
| `fgets` (pantallas de históricos) | 0,264 MB/s | **0,623 MB/s** | **2,4x** |
| `read()` POSIX (control) | 2,623 MB/s | 4,770 MB/s | varía con la tarjeta |

Abrir los históricos del frigo/batería/solar y el visor de logs se nota: un CSV
de 1 MB pasa de ~3,5 s a ~0,5 s.

## Cómo se encontró (por si hay que repetirlo)

1. Contadores temporales en el `diskio` del IDF: leer 2 MB por `fread` hacía
   **4098 lecturas de 1 sector** (512 B) mientras escribir agrupaba 16 sectores.
2. La misma lectura con `read()` POSIX: **130 llamadas de 16 KB** y 5,8 MB/s
   (9x). Con `setvbuf(_IONBF)` empeoraba (0,04 MB/s).
3. El `st_blksize` del VFS (512) era el que fijaba el búfer de stdio. La ayuda
   del propio Kconfig lo dice: *"Increasing this value improves fread() speed"*.

## Pendiente

- El recorrido línea a línea (`fgets`) sigue en 0,62 MB/s: si algún día molesta,
  los lectores pueden pasar a `read()` y trocear en RAM (medido: 4,8 MB/s).
- La tarjeta sin marca del banco sigue siendo el techo (y a veces falla el
  montaje: el aviso rojo de la barra y el texto de Ajustes lo dicen).
