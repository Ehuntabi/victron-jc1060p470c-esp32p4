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

## Simulador de uso completo (BLE y WiFi)

Además de las órdenes sueltas, el puente lleva un **simulador** que machaca la P4
como si el vehículo estuviera en marcha, y un **satélite** que hace de cabina.

```bash
# Emitir con los 3 aparatos configurados, rotando los SEIS tipos de registro
python3 puente.py "sim ble <mac1> <clave1> <mac2> <clave2> <mac3> <clave3>"

python3 puente.py "sim ritmo 50"      # ms entre tramas (50 = ~20/s; 30 = a tope)
python3 puente.py "sim caos on"       # rota solo: normal -> fijos -> extremos -> fuzz
python3 puente.py "sim fijo 637 1337 -4444"   # valores fijos, para comparar
python3 puente.py "sim extremo on"    # centinelas de "sin dato" y maximos de 32 bits
python3 puente.py "sim"               # informe: cuantas tramas de cada tipo
python3 puente.py "sim stop"

# Hacer de cabina/satelite contra la P4
python3 puente.py "sat VictronConfig <clave del AP> victron <clave del portal>"
python3 puente.py "sat informe"
python3 puente.py "sat stop"
```

Modos del simulador, para qué sirve cada uno:

| Modo | Qué manda | Para qué |
|---|---|---|
| normal | los 6 tipos con valores que se mueven (onda triangular) | uso normal |
| fijos | monitor de batería con números reconocibles | comparar envío/recepción número a número |
| extremos | centinelas 0x7FFF/0xFFFF, máximos de 32 bits, SOC 102,3 %, 215 °C | buscar desbordes y basura en pantalla |
| fuzz | tramas malformadas: fabricante que no es Victron, tipo desconocido, longitudes raras, clave que no cuadra | comprobar que la P4 se defiende |

## Banco de pruebas de las dos placas (banco.py)

```bash
PUENTE_AP_CLAVE=<clave del AP> PUENTE_PORTAL_CLAVE=<clave del portal> \
    python3 banco.py 4 300          # 4 horas, informe cada 300 s
```

Abre **un solo dueño por puerto** (importante: abrir el puerto de la P4 la
reinicia, así que se abre una vez y no se vuelve a tocar), lee las dos placas a
la vez, cuenta lo que se envía y lo que se descifra, y **se re-arma solo** si el
puente se reinicia. Deja:

- `/tmp/banco_p4.log` y `/tmp/banco_esp.log`: todo lo que dicen las dos placas.
- `/tmp/banco_informe.txt`: un informe cada periodo (reinicios, asserts, pánicos,
  watchdog, registros descifrados por tipo, contadores del satélite).

Se lanza desacoplado para que no dependa de la sesión:

```bash
PUENTE_AP_CLAVE=... PUENTE_PORTAL_CLAVE=... setsid nohup python3 banco.py 4 300 \
    > /tmp/banco_salida.txt 2>&1 < /dev/null &
```

## Lo que ha salido de las pruebas (24-sep-2026, 4 h a tope)

- **La cadena entera cuadra**: emitiendo en modo fijos (63,7 % | 13,37 V |
  −4,444 A), la P4 lo descifra igual (`Vbat=13.37V Ibat=-4.444A SOC=63.7%`) y lo
  publica igual en su telemetría UDP (`soc=637 V=1337 I=-4444`), que el satélite
  recibe con **0 CRC malos**.
- **Los valores imposibles no la rompen**: 327,67 V, SOC 102,3 %, 215 °C,
  `OffReason=0xFFFFFFFF`, `Flags=0xFFFFFFFF`… todo se parsea, se registra y se
  publica, con **0 asserts, 0 pánicos y 0 reinicios** en la P4.
- **El reparto por tipo es uniforme** (~47 % de lo enviado, igual en los seis):
  no hay ningún camino roto; lo que no se descifra es la cuota de tramas
  malformadas del modo fuzz y el ciclo de escaneo de la P4.
- **La cámara de la placa de reserva está caída**: `/snapshot` contesta **503** y
  la P4 lo dice claro (`la camara no ha dado un fotograma nuevo a tiempo; NO
  sirvo una foto vieja`). No es un fallo del firmware: prefiere no servir basura.
- **Los CSV y `/ota` viven en el puerto 8081**, no en el 80: pedirlos al 80 da
  **404** (el cazatodo del portal). Apuntado aquí porque es una trampa fácil.

## Medidas finas (24-sep-2026, banco de 4 h)

- **La P4 no pierde nada por su lado**: lleva su propia cuenta (`udp_tx: TX ok=...
  err=...`) y en 11 minutos iba por 660 envios con **0 errores**, uno por segundo
  clavado (`vTaskDelayUntil(..., 1000)`).
- **Lo que se pierde es el aire**: el satelite recibio el 80 % de esos paquetes,
  todos con el CRC bien. Es lo esperado: la difusion UDP no se reintenta nunca
  (best effort, sin ACK) y ademas el receptor esta emitiendo BLE a 20/s con la
  misma radio. El protocolo ya lo asume (manda el estado entero cada segundo).
- **Con el ahorro de energia del receptor puesto se perdia un 23 % mas** (76,7 %
  frente a 87,7 %): una estacion con modem-sleep se pierde las difusiones que
  caen entre balizas DTIM. Por eso el satelite lleva `esp_wifi_set_ps(WIFI_PS_NONE)`.
- **La alarma de bateria salta**: con la simulacion mandando SOC bajo (22 %),
  la P4 registra `alarma: alarma de bateria activa` y su telemetria publica
  `alarmas=0x04` (MINI_ALARM_BATERIA). Las ordenes de silencio del satelite
  tambien se atienden (`orden de silencio mask=0x0f`).
- **La P4 sanea lo imposible antes de publicar**: los centinelas se convierten en
  "sin dato" (`MINI_NO_DATA_I16`, `MINI_NO_DATA_I32`) y el 0x7FFF de tension en 0,
  en vez de propagar 327,67 V o 102,3 % como si fueran buenos.

## Trampas de taller de esta herramienta

- **Abrir el puerto serie reinicia la placa** (USB-Serial-JTAG). Por eso
  `banco.py` abre cada puerto una sola vez y nunca lo vuelve a tocar; y por eso
  `puente.py` espera al aviso `puente>` antes de mandar nada.
- **Un solo dueño por puerto**: con dos procesos leyendo el mismo puerto, los
  datos se reparten y se pierden la mitad (pasó y costó un rato entenderlo).
- Las credenciales van **por variables de entorno** (`PUENTE_AP_CLAVE`,
  `PUENTE_PORTAL_CLAVE`), nunca dentro del fichero: así se puede versionar.
