---
name: project-ne185-implementation-status
description: "Estado de la implementacion NE185 en pantalla 7\" - intentos, resultados, hipotesis pendientes. Esto va a tardar en estabilizar, persistir contexto entre sesiones"
metadata: 
  node_type: memory
  type: project
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

# Estado de la implementacion NE185 (2026-05-26)

El usuario avisa que dar con la implementacion correcta del NE185 master mode **va a tardar** (problema iterativo). Esta memoria persiste TODO lo intentado para no repetir trabajo.

## Hardware

- ESP32-P4 en pantalla 7" Guition JC1060P470C_I
- Transceptor RS-485: **MAX485 con auto-DE** (no hay GPIO de control direction, hace switch interno con TX activity)
- UART2 a 38400 8N1
- Pines exactos: ver `main/ne185/ne185.c` define UART_TX/UART_RX (revisar si dudas)
- Bus va al panel NE187 original Y al NE185 modulo principal en el armario

## 2 modos de operacion

1. **SNIFFER** (`s_master_mode = false`): solo escucha. Funciona si el NE187 panel original esta conectado al bus y poleando. **2056 frames capturados OK 2026-05-25** que sirvieron para el reverse engineering del protocolo (ver [[project-ne185-protocol]]).

2. **MASTER** (`s_master_mode = true`): emite `FF 4X 00 00 chk` cada 60ms en lugar del NE187. Necesario cuando reemplazamos al panel original.

## Lo intentado (cronologico)

### 2026-05-25 — SNIFFER OK
- Captura de 2056 frames del bus mientras NE187 conectado.
- Reverse engineering completo: cmd FF 4X, checksum (b5+b9+b14+b15+0xB1), layout 20 bytes, press hold 2 frames.

### 2026-05-26 manana — implementacion master mode v1
- Polling 60ms (16 Hz)
- Press hold de 4 frames
- Press queue desde UI
- Verbose log default ON
- `READ_TIMEOUT_MS = 50ms`
- Expectativa: respuesta de 20 bytes (FF eco cmd + 15 bytes de status + checksum)

### 2026-05-26 tarde — viaje autocaravana = FRACASO
- 0 frames recibidos
- User verifico cableado (conectores tienen polaridad fisica, no se pueden conectar mal)
- Bus tenia que funcionar: dias atras recibia tramas
- Hipotesis principal: el codigo intentaba leer 20 bytes pero el MAX485 con auto-DE **bloquea RX durante TX**, asi que cuando el TX termina y el NE185 ya esta respondiendo, los primeros bytes (eco del cmd, 5 bytes) NO se reciben -> solo llegan los ultimos 15 bytes -> codigo descarta por longitud != 20

### 2026-05-27 manana — viaje #9 con flash anterior

**Hallazgos del log SD:**

1. NO recibimos 0 frames. El NE185 SI responde 15 bytes consistentes:
   `rx 15 bytes b[0..4]=7C E0 00 40 XX` donde XX es un counter que incrementa
   (3A, 3D, 3F, 41, 42, 43, ...). Formato totalmente distinto al sniffer NE187.

2. NE187 desconectado en este viaje -> DESCARTA hipotesis de collision.

3. El checksum del sniffer (b19 = b5+b9+b14+b15+0xB1) NO valida estos bytes.
   Sugiere que el NE185 responde con formato DISTINTO cuando hablamos
   directamente sin que NE187 intermedie.

4. Orion DC/DC sigue sin llegar al callback BLE (ningun adv con MAC E7:59).
   El "DC En: 12.5V" de la UI viene del BMV Aux (12.52V en log), no del Orion.

5. SD save panic confirmado: al pulsar "Guardar SD" 2 veces consecutivas
   -> pantallazo azul + reboot. Workaround: pulsar 1 vez y esperar.
   Causa probable: race condition en log_capture_save_to_file con mutex
   portMAX_DELAY mientras otras tasks loguean a 16Hz -> task watchdog panic.

### 2026-05-27 tarde — TX erratico confirmado por user

Tras visita 11:30: "funcionan los botones de manera erratica, pero consigo
encender luces y bomba". TX llega al NE185 pero el toggle requiere MULTIPLES
pulsaciones (no es pulse->ON, pulse->OFF limpio).

Causa probable: bus RS-485 sin terminacion ni pull-up/down adecuados (foto
NE187 mostro 19.2k pull-up + 2.26k pull-down asimetricos + 2x150ohm serie
+ SIN 120ohm). En el bus de la autocaravana con solo nuestra pantalla
conectada al NE185, falta bias estable -> los 4 frames del press hold a
60ms tienen ruido -> NE185 no detecta 2 frames consecutivos validos.

Soluciones a probar (NO aplicadas todavia, requieren commit):
1. HOLD_FRAMES 4 -> 8 (mas margen)
2. POLL_PERIOD_MS 60 -> 100ms (mas espacio entre cmd)
3. RELEASE_FRAMES 2 -> 5 (mas idle entre press)
4. Delay 10ms entre frames del press

**HW descartado**: el usuario tiene placa de bias montada con
R1=R2=680ohm pull-up/down + R3=132ohm (220||330) terminacion.
Documentada en `~/joint/victron/docs/ne185_bias_board.pdf`.
Idle diff ~420mV, bias ~7.4mA. Bus electricamente OK.

Tambien confirmado en este viaje:
- Frame15 estatico (b[0..1]=7C E0, b[2..3]=00 40, b[4]=counter, b[5..13]
  datos sin cambios significativos, b[14]=b[4]|0xA0 = "checksum simple")
- Orion DC/DC NO llega al callback BLE (solo BMV y SmartSolar)
- Reencode legacy completado: 168/173 ok, 42.5 GB liberados

### 2026-05-27 tarde — fixes "lo basico" (committed, NO probado)

Tres fixes en bloque sin viaje:

1. **`fix(log_capture): SD save panic con 2 clicks`** (commit `600227f`)
   Copy-then-write + flag busy. Resuelve el pantallazo azul al pulsar
   "Guardar SD" 2 veces consecutivas. Permite diagnosticar sin perder logs.

2. **`fix(view_overview): desactivar tank_demo cycle`** (commit `5d4f855`)
   Quita el placeholder que ciclaba aguas grises 0/1 cada 1.5s. Confundia
   al usuario haciendo creer que habia datos NE185 reales.

### Hipotesis Orion DC/DC descartadas/vigentes (estado 2026-05-27)

**DESCARTADA: Extended Advertising BLE 5.0**
Agente WebSearch 2026-05-27 confirmo que el Orion XS emite 24 bytes mfg_data
en LEGACY adv (cabe en 31 bytes). Multiples proyectos publicos
(keshavdv/victron-ble, Fabian-Schmidt/esphome-victron_ble,
hoberman/Victron_BLE_Advertising_example) reciben Orion con legacy scan
estandar en ESP32. NADIE necesita CONFIG_BT_NIMBLE_EXT_SCAN. Cambio
sdkconfig DESCARTADO. Tag git de seguridad creado pero no aplicado:
`sdkconfig-known-good-2026-05-27`.

**VIGENTES - en orden de probabilidad:**

1. **VE.Smart networking mode** activado en el Orion (mas probable).
   Cuando activo, el Orion emite mensajes distintos al "Instant Readout"
   estandar que decodifica nuestro codigo. Verificacion: app Victron
   oficial -> Orion -> Settings -> "VE.Smart networking" -> debe estar
   OFF. Pendiente: usuario verificara proximo viaje.

2. **esp_hosted C6 RX queue/buffer drop** silencioso de paquetes ~24
   bytes. El C6 actua como controller y reporta adv al P4 via SDIO.
   Si su queue es chica o tiene filter de longitud, puede descartar
   los adv del Orion sin que el P4 los vea. Verificacion compleja
   (requiere instrumentar slave).

3. **NimBLE MSYS_1_BLOCK_COUNT bajo** en host P4. Si los buffers para
   advertising packets se llenan, NimBLE descarta. Verificacion:
   `idf.py menuconfig` -> Bluetooth -> NimBLE -> Memory pool sizing.

4. **Firmware Orion < v3.61** (necesario para Instant Readout segun
   esphome-victron_ble). Verificacion: app Victron -> Product info -> FW.

### Hipotesis nueva Orion DC/DC (a verificar)

El adv del Orion NO llega al callback BLE en la 7" pero SI en la 3.5".
Mismas keys/MAC verificadas. Parametros de scan identicos (NimBLE
itvl=0x0060 window=0x0030 passive=1) en ambas pantallas.

Diferencia critica: 3.5" usa ESP32-S3 con BLE nativo. 7" usa ESP32-C6
como controller via esp_hosted SDIO. **Hipotesis: el slave esp_hosted
no esta reportando extended advertising (BLE 5.0) al host P4.** Si el
Orion DC/DC emite adv en formato extended, P4 nunca lo recibe.

Verificacion: revisar sdkconfig de C6 slave (proyecto esp_hosted_slave)
y ver si tiene `CONFIG_BT_NIMBLE_EXT_ADV=y`. Si no, habilitarlo en
slave Y master.

Otra hipotesis: el Orion en el momento del viaje 2026-05-27 estaba
**Off** (la card mostraba "Estado: Off"). Quiza emite menos frecuencia
de adv cuando esta off (idle vs activo) -> P4 nunca capta.
Verificacion: arrancar motor antes del proximo viaje para que el
Orion entre en modo carga -> mas adv -> mas posibilidad de captura.

### 2026-05-27 tarde — fix de diagnostico (committed)

Commit `797fab4 diag(ne185): loguear 15 bytes completos`. Cambia el log de
error path para volcar los 15 bytes completos en hex agrupados de 5. La
proxima visita capturara el frame completo para reverse-engineer el
formato real del NE185 (sin intermediacion NE187).

### 2026-05-26 noche — fix v2 (committed, NO probado aun)
Commits: `5c9fe75 refactor(ne185)` + `f1b3bd5 refactor(ne185)`.

Cambios concretos:
- `READ_TIMEOUT_MS` 50 -> 200ms (NE185 puede tardar 50-150ms en responder, no 50)
- Aceptar respuesta de 15 bytes reconstruyendo el frame de 20 con el eco del cmd (memcpy tx_cmd al principio)
- Verbose log default OFF (16Hz saturaba buffer RAM)
- Press confirm watcher: tras release, esperar N frames y verificar si bit toggled, log CONFIRMED/FAILED
- Eliminado `uart_flush_input` antes de TX (podia borrar bytes ya en buffer)

**Estado**: flasheado el 2026-05-26 ~20:00, NO probado todavia en autocaravana. Proxima visita confirmara si recibe frames.

## Hipotesis pendientes si la v2 NO funciona en el proximo viaje

Listadas por probabilidad descendente. Investigar EN ESTE ORDEN:

1. **Master collision con NE187 original**: si el NE187 sigue conectado al bus mientras nosotros emitimos como master, los dos compiten -> colisiones -> NE185 ignora todo. **Fix:** desconectar fisicamente el NE187 antes de modo master, o detectarlo via sniff inicial y deshabilitar master automaticamente.

2. **Polaridad RS-485 invertida**: A/B de MAX485 podrian estar al reves para enviar (auto-DE invierte la salida). El NE185 espera idle=high en A. **Diagnostico:** osciloscopio en A/B con cmd enviado. **Fix:** swap A/B.

3. **Auto-DE timing**: el MAX485 tarda ~1.5us en hacer switch TX->RX. A 38400 baud (~260us/byte), eso son menos de 1% del byte time. Improbable.

4. **Termination resistor faltante**: 120 ohm entre A/B. Si solo NE187 lo provee y lo desconectamos, no hay terminacion -> reflexiones -> NE185 lee basura. **Fix:** poner 120 ohm en la pantalla 7" cuando es master.

5. **Pull-up/pull-down ausentes**: en bus idle. Sin ellos, el bus flota y NE185 puede ver 1's espurios. **Fix:** R10k a VCC en A, R10k a GND en B.

6. **Velocidad mal**: ¿es REALMENTE 38400? El NE187 puede usar otra. Ya verificado contra captura SNIFFER que es 38400.

7. **NE185 espera secuencia init antes de responder**: quiza tras power-on espera un cmd especifico (handshake) que el NE187 envia. **Diagnostico:** sniff primeros segundos tras power-on del NE185.

8. **El comando va bien pero NE185 NO responde (es slave silencioso)**: quiza solo responde a queries explicitas, no a polls. Buscar cmd `FF 50` o `FF 60` en logs anteriores.

## Bug del codigo a verificar antes de proximo viaje

- En `ne185.c` linea ~XXX: la reconstruccion del frame de 20 hace `memcpy(frame20, tx_cmd, 5)`. Verificar que `tx_cmd` esta declarado y poblado en el scope del read. Si esta declarado fuera del bucle pero modificado, podria tener valor stale.
- Verificar que `checksum_ok()` funciona sobre el frame reconstruido (que tiene 5 bytes eco + 15 bytes respuesta de NE185, el checksum se calcula sobre los ultimos).

## Logs para verificar en proximo viaje

Con `s_verbose_log = true` activado desde UI (LOG ON button):
- `RX 20 bytes, OK` -> v2 funciona, todo OK
- `RX 15 bytes, reconstruido a 20, checksum OK` -> v2 acepta el caso esperado
- `RX 15 bytes, checksum FAIL` -> hipotesis 1 (collision) o 2 (polaridad)
- `Timeout, 0 bytes recibidos` -> hipotesis 1, 2, 4, 5, 7, 8
- `RX X bytes (X != 15, 20)` -> caso no contemplado, sniffear hex

## Archivos relevantes

- `/home/jc/joint/victron/main/ne185/ne185.c` — implementacion principal
- `/home/jc/joint/victron/main/ne185/include/ne185.h` — API
- `/home/jc/joint/victron/main/ui/view_overview.c` — UI card camper con LOG ON / botones
- `/home/jc/Documentos/joint spl 154/logs_joint_ne185/log_2026*.txt` — capturas sniffer 2026-05-25

## Tests pendientes

- Bench test con generador de frames (otro ESP32 simulando NE185) para validar master mode sin viajar
- Si no es posible bench: viaje proximo + log SD insertada antes de power-on + 2 minutos con LOG ON

## 2026-06-21 noche (autocaravana) — sesion monitor en vivo

Nuevo: Claude lee `/dev/ttyACM0` en vivo via pyserial (ver
[[feedback-live-serial-sniffer-workflow]]). Bucle cerrado usuario<->Claude.

**BLE: resuelto, todos los dispositivos OK.**
- BMV/SmartShunt FF:3C:F3:77:D6:86 (0xA389) OK. Motor en marcha: Ibat +21.5A
  cargando, Aux 13.95V (antes -0.2A descarga) -> el Orion/alternador carga bien.
- SmartSolar MPPT 100/30 C2:6D:F3:71:63:2F (0xA056) OK.
- Orion DC/DC: el usuario CONFIRMA que ahora SI ve sus valores en pantalla.
  (Hipotesis Orion previas de esta memoria quedan superadas / a revisar.)
- F6:40:14:6F:DF:4F (0xA075 SmartSolar 75/15, rssi -92..-99) = VECINO, descartado OK.

**Incidencia USB (no firmware):** el P4 se reenumeraba en bucle (USB disconnect
device 8->14) con la PANTALLA encendida (sin reset del chip) => cable/conector USB
de datos defectuoso, NO brownout ni codigo. Sintoma usuario: "a veces marca y
otras no". Alimentacion era solo-USB-portatil. Fix: cable USB-C de datos bueno.

**Sniffer NE185 NO probado aun** (se acabo la sesion). Pendiente #1 sigue: sniff
del boton CHECK del NE187 (tanques agua) + luces ON/OFF. Plan paso a paso en
`~/joint/victron/SNIFFER-PLAN.md`. Firmware HEAD de main ya flasheado al P4.

## 2026-06-22 (autocaravana) — sniffer + master OK, casi todo operativo

Sesion en vivo (P4 + NE187 + 220V), bucle cerrado pyserial. Resultados:
- **Tanques confirmados en el bus**: limpia 1/4 -> b[5]=0x01, grises vacio -> b[6]=0x02.
  Viajan en cada frame, NO hace falta CHECK (CHECK solo cambia cadencia a FF40 ~29s).
- **Shore 230V corregido (commit 7f436b9)**: indicador real = b[16] bit0
  (0x31 red / 0x30 sin red), NO b[15] bit7 (heartbeat del poll).
- **Luces int/ext + bomba (b[15] bit0/1/2) validadas en vivo** las tres.
- **Botones camper arreglados (commit 21630e0)**: el `row` (lv_obj_create) nacia
  CLICKABLE en LVGL v8 y robaba el click; fix = clear_flag CLICKABLE.
- **Master mode FUNCIONA con NE187 conectado, sin colision**: la UI togglea las
  cargas fisicas (FF 41/42/44), confirmado por el watcher.
- **Test strip de pruebas eliminado (commit e850c9a)**.
- **Orion XS senal debil (commit a613a08)**: rssi -96..-99 dBm -> card DC/DC
  parpadeaba a "--V"; mitigacion DCDC_TIMEOUT_MS=5min. Arreglo real = posicion Orion.

PENDIENTES (usuario 2026-06-22): niveles de agua 2/4..4/4 y grises lleno (cambiar
agua), bits 3-6 de b[15], ventiladores GPIO21 y DS18B20 GPIO26 (a CABLEAR).

Relacionado: [[project-ne185-protocol]] - decodificacion protocolo, [[project-joint-autocaravana]] - paths, [[feedback-live-serial-sniffer-workflow]]
