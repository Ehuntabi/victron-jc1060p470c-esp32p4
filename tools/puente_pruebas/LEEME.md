# Puente de pruebas (WiFi y BLE) entre el PC y la P4

**Para qué:** probar la P4 desde este PC sin sacar el vehículo del garaje y sin
pelearse con permisos del PC. Se graba en una **placa ESP32 aparte** (vale
cualquiera con WiFi y BLE; se ha usado un ESP32-C6) y desde el PC se le mandan
órdenes por USB. La placa hace de:

- **Victron (BLE)**: anuncia una trama de monitor de batería con el formato real
  de Victron, cifrada con la clave del equipo simulado. La P4 la recibe, la
  descifra y la enseña como si fuera el aparato de verdad.
- **Satélite (WiFi)**: se conecta al AP de la P4, escanea, pide el portal por
  HTTP y manda datagramas UDP.

Con eso quedan cubiertas **las dos conexiones** de la P4 en el banco.

## Cómo se usa

```bash
# 1. Compilar y grabar en la placa ESP32 (NO en la P4)
cd ~/joint/victron/tools/puente_pruebas
source ~/.espressif/esp-idf-5.5/export.sh
idf.py set-target esp32c6      # o el chip que sea
idf.py -p /dev/ttyACM1 flash   # ¡ojo al puerto, que la P4 es otro!

# 2. Mandarle órdenes desde el PC
python3 puente.py status
python3 puente.py "wifiscan"
python3 puente.py "wifista VictronConfig <clave del AP>"
python3 puente.py "httpget http://192.168.4.1/"
```

`puente.py` abre el puerto, espera al aviso `puente>` (al abrir el puerto la
placa se reinicia, es cosa del USB) y lee hasta la línea `OK` o `ERR`.

## Órdenes

| Orden | Qué hace |
|---|---|
| `status` | Estado del puente (BLE listo/emitiendo/escaneando, WiFi y IP). |
| `blescan on\|off` | Escanear y volcar los anuncios que ve (`ADV <mac> rssi=... vid=...`). |
| `blevictron <mac> <clave32hex> <V_centi> <I_milli> <soc_deci> <ttg_min>` | Emular un SmartShunt. Ejemplo: `blevictron F9:EB:87:DB:E7:59 <clave> 1234 -5678 777 321`. |
| `bleadv <mac> <hex...>` | Anunciar datos de fabricante crudos (para otros formatos). |
| `bleoff` | Parar la emisión BLE. |
| `wifiscan` | Listar redes (`AP rssi=... canal=... ssid="..."`). |
| `wifista <ssid> <clave>` | Conectarse a un AP (el de la P4). |
| `wifioff` / `wifiip` | Desconectar / ver la IP. |
| `httpget <url>` | Petición HTTP y volcado del cuerpo (primeros 200 bytes). |
| `udp <ip> <puerto> <hex> [cada_ms] [veces]` | Mandar datagramas UDP. |

## El formato de Victron, que costó un rato

La trama que espera la P4 (`victron_ble.c`) es:

```
E1 02 | 10 | largo | producto(2) | tipo(1) | nonce(2) | clave[0] | 15 bytes cifrados
```

y esos 15 bytes en claro son: `TTG(2) V(2) alarma(2) aux(2)` y luego, **empaquetado
a bits** (esto es lo que no es evidente):

| Bits | Campo |
|---|---|
| 0-1 | entrada auxiliar |
| 2-23 | corriente, 22 bits con signo (0,001 A) |
| 24-43 | consumo, 20 bits con signo (0,1 Ah) |
| 44-53 | SOC, 10 bits (0,1 %) |

El cifrado es AES-128-CTR con la clave del equipo y el contador
`{nonce_lo, nonce_hi, 0…}` (16 bytes). La P4 comprueba además que el byte
`clave[0]` del anuncio coincida con el primero de la clave guardada, y **filtra
por MAC**: la dirección del anuncio tiene que ser una de las que tiene
configuradas (por eso el puente emite con dirección aleatoria igual a la del
equipo simulado, con `ble_hs_id_set_rnd`).

## Lo que se ha verificado con él (23-24 sep 2026)

- La P4 descifra la trama: `Vbat=12.34V Ibat=-5.678A SOC=77.7% TTG=321 min`, y
  los valores salen en la pantalla.
- El puente se conecta al AP de la P4 y recibe IP por DHCP (`192.168.4.2`).
- El portal contesta a la petición HTTP del puente (`Auth required`).
