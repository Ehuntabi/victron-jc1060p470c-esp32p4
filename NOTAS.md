v4.5 — el tick del sistema a 1000 Hz: la SD lee 4x más rápido (y esp_hosted deja de avisar)

## Qué cambia

`CONFIG_FREERTOS_HZ`: de 100 a 1000 (un tick cada 1 ms en vez de cada 10 ms).

## Por qué (medido, no supuesto)

esp_hosted avisa en **cada arranque**: *"CONFIG_FREERTOS_HZ is 100, ESP-Hosted
recommended 1000, to avoid bus level jitters"*. Se midió el efecto en el banco,
misma tarjeta y con la radio ya levantada:

| | 100 Hz | 1000 Hz |
|---|---|---|
| lectura por FATFS, 8 KB | 0,345 MB/s | **1,354 MB/s** |
| lectura por FATFS, 64 KB | 0,330 MB/s | **1,333 MB/s** |
| lectura por FATFS, 256 KB | 0,361 MB/s | **1,224 MB/s** |
| escritura por FATFS | 0,49 - 0,53 MB/s | 0,52 - 0,55 MB/s |
| lectura cruda (64 KB) | 1,3 - 3,9 MB/s | 4,2 MB/s |

El motivo es la granularidad: cada espera de una transacción SDMMC se mide en
ticks, así que con 10 ms por tick una operación podía esperar hasta 10 ms de más.
La escritura apenas cambia porque agrupa bloques grandes.

## Comprobado antes de aplicarlo

- **Perfil de errores del arranque idéntico** al de 100 Hz: los mismos tres avisos
  de banco (sonda 1-wire sin conectar, SELFTEST del NE185 y el `swap_xy` del
  panel), ni uno nuevo.
- **Cero esperas en ticks literales** en el código propio (todo con
  `pdMS_TO_TICKS`), o sea que ninguna espera cambia de significado al mover el
  tick. Sin esto, el cambio sería peligroso.
- Regla nueva en `test/auditar.sh` (2c): el tick tiene que ser ≥ 1000, con este
  motivo escrito al lado.
