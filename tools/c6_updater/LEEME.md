# Actualizar la radio (C6) de una placa que todavía tiene la vieja

**Para qué:** una placa que aún lleva el firmware de radio anterior al
23-sep-2026 no puede pasar directamente al firmware nuevo de la P4. Si se le
graba solo el firmware nuevo, el punto de acceso funciona pero **el BLE no
arranca** y se pierden las lecturas de los Victron. Medido en el banco, no es
teoría:

```
W (19947) victron_ble: esp_hosted_bt_controller_init fallo: ESP_FAIL (0xffffffff)
W (24957) victron_ble: esp_hosted_bt_controller_enable fallo: ESP_FAIL (0xffffffff)
```

El motivo es que host y esclavo van **emparejados**: la 2.5.2 movió al host la
tarea de encender el controlador Bluetooth del C6, la 2.6.0 cambió la API de OTA
del esclavo (el `end` ya no activa, hay un `activate` aparte) y la 2.12.13 añade
eventos que el esclavo viejo no manda. Además, el firmware nuevo **ya no sabe
grabar** un C6 viejo (le falta el `activate`), así que la grabación hay que
hacerla **con el firmware viejo puesto**.

De ahí el orden obligatorio: **primero la radio, después la P4**.

## Lo que hay en esta carpeta

| Fichero | Qué es |
|---|---|
| `aplicar.sh` | Prepara el firmware «transportador» (compila y, si se le pide, lo graba por USB). |
| `slave_ota.c` / `slave_ota.h` | El módulo que graba el C6 leyendo la imagen de la tarjeta SD. Se inyecta en el árbol del firmware viejo; **no va en el firmware de publicación**. |

## Procedimiento

```bash
# 1. Copia la imagen nueva de la radio a la RAIZ de la tarjeta SD de la placa
cp ~/joint/firmware_radio/network_adapter_2.12.13.bin /media/<tu-lector>/network_adapter.bin

# 2. Compila y graba el transportador (la placa por USB)
cd ~/joint/victron/tools/c6_updater
./aplicar.sh flash

# 3. Enciende la placa con la SD dentro. A los 30 s graba el C6 sola (~1 minuto).
#    También se puede lanzar a mano: Ajustes -> Wi-Fi -> "Actualizar radio C6".
#    En el log se ve el avance: 10% ... 100%, y luego el reinicio de los dos.

# 4. Graba ahora el firmware nuevo de la P4 (el de publicación), por USB o por
#    la web del portal.

# 5. Comprueba en el log del arranque:
#       Radio C6: firmware 2.12.13
#    Si sale "no dice su version (firmware anterior a la 2.12.13)", el C6 sigue
#    viejo: repite los pasos 1-3.
```

Ojo con el paso 4: mientras la placa lleva el transportador y el C6 ya está
grabado, el AP funciona pero el BLE no (host viejo + C6 nuevo). Es normal y se
arregla con el paso 4.

## Detalles que importan

- **La imagen va en la SD y no empotrada en el firmware** porque no cabe: ocupa
  1,2 MB y la partición de aplicación es de 4 MB, con la app actual (3 MB) la
  compilación falla con `All app partitions are too small`.
- **Se comprueba el fichero antes de empezar**: si no está, o si no parece una
  imagen (entre 900 KB y 2 MB), no se toca nada y se dice por el log.
- **La escritura va a la partición inactiva del C6.** Un corte a medias deja el
  firmware actual intacto y la placa sigue arrancando.
- **Lo que no se puede deshacer** es una imagen que arranque pero no hable: para
  eso haría falta el UART del C6, que va soldado. Por eso la imagen que se graba
  se compila con la configuración ya probada (interfaz SDIO y los pines de esta
  placa) y está guardada en `~/joint/firmware_radio/` con su md5.
- **Vuelta atrás:** la imagen vieja está en `~/joint/firmware_radio/`. Para
  volver habría que usar el mismo transportador con esa imagen y luego un
  firmware de P4 con el host viejo (v3.14 o anterior); con el host nuevo el C6
  viejo no da BLE.

## Cómo funciona por dentro

- El árbol lo saca `aplicar.sh` con `git worktree` del tag **v3.14**, el último
  con esp_hosted 0.0.27 (el único host que puede hablar con un C6 viejo).
- El módulo usa las primitivas de OTA de esa versión (`rpc_ota_begin`,
  `rpc_ota_write`, `rpc_ota_end`), que en la 0.0.27 sí activan la partición
  nueva y reinician el C6.
- La imagen se lee **de golpe a PSRAM** y se suelta el cerrojo de la SD antes de
  empezar a grabar: la grabación tarda ~1 minuto y el bus de la SD lo comparten
  el datalogger y la cámara.
- `aplicar.sh` inyecta tres cosas en el árbol viejo: el módulo en
  `main/portal/`, una llamada `slave_ota_start_diferido(30)` en `app_main` y el
  botón rojo en Ajustes → Wi-Fi.
