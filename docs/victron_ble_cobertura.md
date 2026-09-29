# Qué se lee de los aparatos Victron (y qué queda pendiente)

Nota técnica de la lectura Bluetooth de los aparatos Victron. El manual de uso lo
cuenta en «Victron Keys»; esto es el detalle para cuando haya que tocar código.

## Cómo se leen

La pantalla P4 escucha el **protocolo de advertising** de Victron: cada aparato emite
su estado por Bluetooth y la pantalla lo descifra con la clave AES de ese aparato
(32 caracteres, la «Instant readout encryption key» de VictronConnect). No hace falta
emparejarse ni conectarse: es difusión, así que **puede haber varios receptores a la
vez** sin molestarse entre ellos.

Código: `victron/components/victron_ble/` (NimBLE a través del C6 con `esp_hosted`).
Tipos de registro: `include/victron_records.h`. Nombres de producto:
`include/victron_products.h` y `victron_products.c`.

## Tipos de registro

| Código | Aparato | En la P4 |
|---|---|---|
| 0x00 | Test | definido, no se usa |
| 0x01 | Cargador solar (MPPT) | **sí**: potencia PV, cosecha de hoy, corriente de carga, tensión, estado, error, salida de carga |
| 0x02 | Monitor de batería (BMV, SmartShunt) | **sí**: carga, tensión, corriente, consumido, tiempo restante, auxiliar/mid/temperatura, alarmas |
| 0x03 | Inversor (Phoenix) | **sí**: tensión y corriente alterna, potencia aparente, estado, error |
| 0x04 | Convertidor DC/DC | **sí**: tensión y corriente de entrada y salida, estado, error, motivo de apagado |
| 0x05 | SmartLithium | **sí**: celdas 1 a 8, balancer, banderas BMS, tensión, corriente, temperatura |
| 0x06 | Inversor RS | definido |
| 0x07 | GX | **no**, ni falta: los GX no emiten (tampoco lo soporta la librería de referencia) |
| 0x08 | Cargador de red (AC) | **sí**: tensiones y corrientes de las tres salidas de batería |
| 0x09 | Smart Battery Protect | definido, faltan campos |
| 0x0A | Lynx Smart BMS | definido, faltan campos |
| 0x0B | Multi RS | definido, faltan campos |
| 0x0C | VE.Bus | definido, faltan campos |
| 0x0D | DC Energy Meter | definido, faltan campos |
| 0x0F | Orion XS (Buck-Boost) | **sí**: corriente y potencia de entrada y salida, estado, error |

## Campos declarados pero sin rellenar

En `victron_records.h` están las estructuras completas, pero el decodificador
(`victron_ble.c`) no rellena estos campos, porque son de aparatos que no están
montados:

| Campo | Aparato |
|---|---|
| `output_state`, `warning_reason`, `error_code` y las alarmas BMS | Smart Battery Protect |
| `io_status`, `warnings_alarms`, `error` | Lynx Smart BMS |
| `active_ac_in`, `active_ac_in_power`, `ac_out_power` | Multi RS y VE.Bus |
| `bmv_monitor_mode` | DC Energy Meter |

Rellenarlos es trabajo pequeño (la estructura ya está), pero **no tiene sentido hacerlo
sin el aparato delante para comprobarlo**. Si algún día se monta uno de esos, ahí está
la lista.

## Nombres de producto

`victron_products.c` se genera con:

```bash
cd victron && python3 scripts/gen_victron_products.py            # actualiza desde la lista oficial
python3 scripts/gen_victron_products.py --comprobar               # solo dice qué cambiaría
python3 scripts/gen_victron_products.py --conservar-nombres       # no renombra los que ya había
```

La fuente es `Victron_ProductId_mapping.txt` del repositorio `keshavdv/victron-ble`,
que es la lista de ids extraída del binario oficial de Victron (`vecan-dbus`):
**784 productos** frente a los 135 que había antes, que salían de la lista corta del
componente de ESPHome. Lo que se arregla con eso:

- El **Orion XS** (registro 0x0F) ya tiene nombre. Antes el id `0xA3D0` decía
  «Orion-Tr Smart DC-DC», que es otro aparato: ahora dice lo que es,
  «Orion Smart 12V/12V-30A Buck-Boost Converter».
- Aparecen los **Blue Smart**, los **MultiPlus**, los **Lynx**, los **Skylla** y el
  resto de la gama, así que cualquier aparato que se añada sale con su nombre en vez
  de «desconocido».
- Se conservan las entradas que ya estaban y no salen en la lista oficial
  (`0xA07F All-In-1 SmartSolar MPPT 75/15 12V`), para no perder nada.

## Avisos que vienen bien (y están en el manual)

1. El aparato tiene que llevar activado **Instant readout via Bluetooth**
   (VictronConnect → Ajustes → Product Info). Si no, no emite.
2. Mientras **VictronConnect está conectado** a un aparato, ese aparato **deja de
   emitir**: la pantalla lo verá caído y volverá solo al cerrar la app. No es una
   avería.
3. La clave son **32 caracteres**, y en la app el último a veces cae en una segunda
   línea. Con 31 hay que **resetear el PIN de Bluetooth** del aparato.
4. Los **Blue Smart** necesitan **firmware 3.61 o superior**.
5. Si un aparato queda lejos, un **segundo receptor** no estorba: al ser difusión,
   otro ESP32 con el componente `victron_ble` de ESPHome puede escucharlo y
   reenviarlo. Sirve también para comparar si algún día se pierden tramas.

Fuentes: [protocolo de advertising de Victron](https://community.victronenergy.com/questions/187303/victron-bluetooth-advertising-protocol.html)
y [componente victron_ble de ESPHome](https://github.com/Fabian-Schmidt/esphome-victron_ble),
que trae la tabla de campos por aparato que se ha usado como lista de comprobación.
