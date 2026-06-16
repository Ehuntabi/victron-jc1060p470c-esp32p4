# VICTRON-CONTEXT — Contexto completo de los 3 proyectos

Documento de referencia compacto con todo el conocimiento acumulado en sesiones anteriores
con Claude. Diseñado para leer desde el portátil de la autocaravana y poder continuar el trabajo
con el mismo contexto que en el PC principal.

> Si trabajas con Claude desde el portátil, abrir este archivo al inicio de la sesión
> (`cat VICTRON-CONTEXT.md`) le da el contexto necesario para entender el proyecto.

---

## 1. ESP-IDF — version 5.4.4 estricta

**Todos los proyectos (victron, victron_mini, pantalla_3.5) usan ESP-IDF v5.4.4**. No 5.4.1, no 5.5.x.

- Path en PC principal: `~/.espressif/esp-idf-5.4` (en tag v5.4.4)
- Path en portátil tras setup-autocaravana.sh: mismo (`~/.espressif/esp-idf-5.4`)
- Activar: `. ~/.espressif/esp-idf-5.4/export.sh` (o usar alias `get_idf`)

**Capabilities críticas que necesita 5.4.4 (para JD9165BA + DSI 1024x600 sin underrun)**:
- `CONFIG_SOC_AHB_GDMA_SUPPORT_PSRAM=y`
- `CONFIG_LCD_DSI_OBJ_FORCE_INTERNAL=y`
- `CONFIG_LCD_DSI_ISR_HANDLER_IN_IRAM=y`
- `CONFIG_SPIRAM_SPEED_200M=y` (NUNCA 20M, da DPI underrun continuo)
- `CONFIG_ESP_LDO_VOLTAGE_PSRAM_DOMAIN=1800` (NO 1900)

En 5.4.1 esas capabilities NO existen en Kconfig → sdkconfig regenerado pierde optimizaciones → pantalla azul con `lcd.dsi.dpi: can't fetch data from external memory fast enough, underrun happens`.

---

## 2. Reglas de trabajo (no romper)

### 2.1 NO ejecutar comandos destructivos sin pedir permiso

**NUNCA**: `fullclean`, `rm -rf build/`, `git reset --hard`, `git clean`, `rm -rf node_modules/`, `idf.py erase-flash`, etc.

**Por qué**: el 2026-05-24 ejecuté `idf.py fullclean` para "arreglar" un warning de toolchain (que era inofensivo). Eso nuked `managed_components/` + regeneró `sdkconfig` con valores incompatibles. Resultado: pantalla azul con DPI underrun. Recovery requirió 1+ hora.

**Señales de que estoy a punto de violar la regla**:
- "Esto debería arreglarlo con un clean"
- "Voy a regenerar X para asegurarme"
- "Lo más rápido es borrar y volver a empezar"
- Cualquier comando con `-f`, `--force`, `clean`, `reset`

### 2.2 Toolchain warnings ≠ errors

Si ESP-IDF imprime `Tool doesn't match supported version from list ['esp-X.Y.Z_DDDD']: ... esp-X.Y.Z_EEEE` y el build **continúa y produce binario** → **es un warning, no un error. NO actuar**.

Solo investigar el warning si el build FALLA con error claro.

### 2.3 Antes de tocar sdkconfig: diff contra tag known-good

Tags disponibles:
- `sdkconfig-known-good-2026-05-22` (primer known-good validado)
- `sdkconfig-known-good-2026-05-27` (más reciente known-good)
- `pre-autocaravana-2026-06-16` (estado al salir de viaje, todos los proyectos)

```bash
git diff sdkconfig-known-good-2026-05-27 -- sdkconfig
# Si capabilities críticas están ausentes -> rollback:
git checkout sdkconfig-known-good-2026-05-27 -- sdkconfig
idf.py reconfigure && idf.py build
```

### 2.4 Política de commits/push

- **Commit local AUTOMÁTICO** tras un grupo lógico completo (un fix, una feature, un refactor de un componente).
- **Push al final del día/sesión** o cuando se pida explícitamente. No push automático.
- Mensajes siguiendo estilo del repo: `tipo(scope): resumen` + bullets + footer Co-Authored-By.
- NO `git add .` ni `git add -A`. Especificar archivos por nombre.
- Verificar `git status` antes de cada `git add`.

### 2.5 Verificar scripts antes de batch

Antes de lanzar un script sobre N archivos, ejecutarlo con 1-2 inputs reales y confirmar output. No basta con haber probado el comando suelto. Aplica especialmente con paths con espacios, caracteres no-ASCII, extensiones no estándar.

### 2.6 Documentar fallos al instante

Si cometo un error que afecta al usuario (rompe build, borra datos, hace algo destructivo no pedido), guardar memoria de feedback **antes de proponer el fix**. El acto de documentar es parte del fix.

---

## 3. Comandos básicos por proyecto

### 3.1 victron (pantalla 7" ESP32-P4)

```bash
victron                 # alias: cd ~/joint/victron && get_idf
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # o ttyACM1
# Ctrl+] para salir del monitor
```

### 3.2 victron_mini (ESP32-C6 sensor remoto)

```bash
victron_mini
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### 3.3 pantalla_3.5 (ESP32-S3)

```bash
pantalla
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # o ttyUSB0
```

### 3.4 Reset entorno (si se pierde IDF)

```bash
get_idf                 # alias para . ~/.espressif/esp-idf-5.4/export.sh
```

---

## 4. NE185 — protocolo RS-485 completo

**Bus**: RS-485 38400 8N1. Transceptor MAX485 con auto-DE (sin GPIO de control direction).

### 4.1 Comandos (master → NE185, 5 bytes)

`FF | 0x40 | bit_botones | 00 | 00 | checksum`

donde checksum = `(b[0]+b[1]+b[2]+b[3]) & 0xFF`:

```
CMD_IDLE     = FF 40 00 00 3F   (poll status, ningún botón pulsado)
CMD_BTN_LIN  = FF 41 00 00 40   (luz interior, bit 0)
CMD_BTN_LOUT = FF 42 00 00 41   (luz exterior, bit 1)
CMD_BTN_PUMP = FF 44 00 00 43   (bomba, bit 2)
```

NE185 procesa el toggle al **2do frame FF 4X consecutivo** a 60ms. Un solo frame se ignora.

⚠️ **NO usar la heurística del repo class142/ne-rs485** — esa documenta el NE334 (cmd FF 0X, polling ~5s). El panel real del usuario es **NE187** (otro modelo) con protocolo distinto (cmd FF 4X overlay, polling 60ms, hold semántico obligatorio).

### 4.2 Respuesta NE185 → master (20 bytes)

```
Pos  Bytes    Significado
0    FF       Header
1    eco cmd byte1 (40/41/42/44)
2    00       constante
3    00       constante
4    eco cmd checksum (3F/40/41/43)
5    nibble bajo = tank LIMPIO (0/1/3/7/F → 0,1/4,2/4,3/4,4/4)
6    bit 1 = tank GRISES (1 = vacío observado; 0 = lleno hipótesis pendiente)
7    00       constante
8    40       constante (algún glitch ocasional con 00)
9    variable (48 valores observados) - IGNORAR (entra en checksum)
10   00       constante
11   FF       constante
12   battery1 servicio: V = (byte - 30) / 10
13   battery2 motor:    V = (byte - 30) / 10
14   variable (3 valores ED/EC/EE) - IGNORAR (entra en checksum)
15   bitmap estados:
       bit 0 = luz interior ON
       bit 1 = luz exterior ON
       bit 2 = bomba ON
       bit 4 = 230V shore (confirmado 28-may)
       bit 7 = shore (otro bit alternativo según versiones)
16   30       constante
17   00       constante
18   00       constante
19   checksum = (b[5] + b[9] + b[14] + b[15] + 0xB1) & 0xFF
```

### 4.3 Cadencia y press hold

- Polling **60ms** (16 Hz). 5s daba UI lenta y nunca confirmaba toggles.
- Press hold: master debe enviar `FF 4X` durante ≥2 frames consecutivos. Recomendado **4 frames (240ms)** de margen.
- Tras hold, volver a `FF 40` durante al menos 2 frames (release) antes de aceptar otro press.

### 4.4 Variables útiles del usuario (8 elementos)

1. Luz interior on/off (b15.0)
2. Luz exterior on/off (b15.1)
3. Bomba on/off (b15.2)
4. Aguas limpias 0..4/4 (b5 nibble bajo)
5. Aguas grises 0 vacío/1 lleno (b6.1 inverted)
6. 230V conectado on/off (b15.4 o b15.7)
7. Batería habitáculo V (b12)
8. Batería motor V (b13)

---

## 5. NE185 — estado actual de la implementación (commits hasta 16-jun)

### 5.1 Estado al salir de viaje (tag `pre-autocaravana-2026-06-16`)

✅ **Funciona**:
- Sniffer mode (escucha NE187 panel original). 2056 frames capturados validan el protocolo.
- Luces interior/exterior/bomba se **encienden** vía RS-485 (test 28-may 7:42).
- 230V shore detectado (bit 4 de b[10] confirmado).

❌ **NO funciona / anomalías**:
- Luces **no se apagan** tras encenderse. Probable que `FF 01 00 C0 C0` sea "set ON" no toggle. **Probar `FF 01 00 80 80` (set OFF) hipotético**.
- Tanks de agua: NO aparecen. Frame15 llega "degradado" con header `F8 E0` en lugar de canónico.
- Orion Tr 12/12-30 DC/DC NO llega al P4 (parser idéntico al de pantalla 3.5 que SÍ recibe → problema en stack BLE P4+C6, no en parser).

### 5.2 Pasos prioritarios en autocaravana

#### Paso 1 (más prometedor): sniff CHECK button del NE187

- Conectar el panel NE187 al bus RS-485.
- Abrir serial monitor del P4 (`idf.py monitor`).
- Pulsar **CHECK** en el NE187 (el usuario explicó que CHECK NO es init/reset; **consulta el estado actual al NE185**).
- Buscar diferencias en `frame15`/`frame20` antes y después.
- **Hipótesis**: el CHECK trigger frame20 canónico con tanques de agua.

#### Paso 2: probar "set OFF" para luces

En `main/ne185/ne185.c`, probar comando `FF 01 00 80 80` (set OFF) en vez de toggle. Si funciona → diferenciar ON/OFF explícito.

#### Paso 3: debug Orion Tr 12/12-30

**ANTES de tocar código, verificar LO BÁSICO en este orden con serial monitor del P4 abierto**:
1. MAC del Orion añadida al NVS via web UI ESP32 → buscar WARN "MAC desconocida"
2. AES key del Orion correcta (copiada de VictronConnect)? → WARN "Key mismatch"
3. Bluetooth del Orion activado y advertising ON (verificar con VictronConnect app)
4. Distancia / jaula de Faraday del compartimento metálico (probar acercando ESP32)
5. Firmware Orion actualizado

**SOLO si todos esos 5 puntos OK y aún no llega, debug del code**:
- Comprobar que `record_type` real es 0x04 (no otro)
- Diagnóstico ampliado en `victron_ble.c` líneas 231-263 logue TODO adv Victron
- El `case VICTRON_BLE_RECORD_DCDC_CONVERTER (0x04)` SÍ está implementado en `victron_ble.c:522`. Parsea state, error, Vin, Vout, off_reason. Mínimo 10 bytes, Orion Tr Smart envía 22-25 bytes.

**Hallazgo crítico 28-may**: el case DCDC_CONVERTER (0x04) parser es **BYTE-A-BYTE IDÉNTICO** entre la pantalla 3.5 (que SÍ lo recibe) y la 7" P4-C6 (que NO). Por tanto el problema NO está en `victron_ble.c` sino en el stack BLE del P4+C6 (esp_hosted via SDIO/VHCI + NimBLE). Sospechas:
- packet truncation en el C6→SDIO→P4
- filter en firmware C6
- MAX_TRANSPORT_BUFFER_SIZE pequeño
- race condition entre múltiples devices

### 5.3 Hipótesis pendientes si master mode NO funciona

Listadas por probabilidad descendente. Investigar **en este orden**:

1. **Master collision con NE187 original**: si el NE187 sigue conectado al bus mientras emitimos como master, los dos compiten → colisiones. **Fix**: desconectar NE187 físicamente, o detectar via sniff inicial y deshabilitar master.

2. **Polaridad RS-485 invertida**: A/B de MAX485 al revés. **Diagnóstico**: osciloscopio en A/B. **Fix**: swap A/B.

3. **Auto-DE timing**: MAX485 tarda ~1.5us en hacer switch TX→RX. A 38400 baud (~260us/byte), <1% del byte time. Improbable.

4. **Termination resistor faltante**: 120 ohm entre A/B. Si solo NE187 lo provee y lo desconectamos, reflexiones. **Fix**: 120 ohm en pantalla 7" cuando es master.

5. **Pull-up/pull-down ausentes**: bus flota. **Fix**: R10k a VCC en A, R10k a GND en B. **HW descartado**: placa de bias montada con R1=R2=680ohm pull-up/down + R3=132ohm (220||330) terminación. Documentada en `~/joint/victron/docs/ne185_bias_board.pdf`. Idle diff ~420mV, bias ~7.4mA. Bus electricamente OK.

6. **Velocidad mal**: ¿REALMENTE 38400? Ya verificado contra captura SNIFFER que es 38400.

7. **NE185 espera secuencia init**: quizá tras power-on espera un cmd específico (handshake) que el NE187 envía. **Diagnóstico**: sniff primeros segundos tras power-on del NE185.

8. **NE185 slave silencioso**: quizá solo responde a queries explícitas. Buscar cmd `FF 50` o `FF 60`.

### 5.4 Logs a buscar (con `s_verbose_log = true` desde UI LOG ON)

- `RX 20 bytes, OK` → v2 funciona
- `RX 15 bytes, reconstruido a 20, checksum OK` → v2 acepta caso esperado
- `RX 15 bytes, checksum FAIL` → hipótesis 1 (collision) o 2 (polaridad)
- `Timeout, 0 bytes recibidos` → hipótesis 1, 2, 4, 5, 7, 8
- `RX X bytes (X != 15, 20)` → caso no contemplado, sniffear hex

---

## 6. Victron BLE — buffer MAX_PAYLOAD_SIZE

En `components/victron_ble/include/victron_records.h`, la macro `VICTRON_ENCRYPTED_DATA_MAX_SIZE` debe ser **>= 25 (ideal 32)**.

**Why**: Productos como Orion DC/DC Tr Smart envían 22-25 bytes encriptados. Antes estaba en 21 (cambio para arreglar un buffer overflow legítimo), pero efecto colateral: el filtro `if (encr_size > 21)` los rechazaba silenciosamente con `ESP_LOGW "Invalid encrypted data size: N"`.

Productos conocidos con payload grande:
- Orion DC/DC Tr Smart (0x04): 22-25 bytes
- Orion XS (0x0F): similar
- Posibles: Multi RS, Inverter RS, Lynx Smart BMS

32 cubre todos los productos Victron actuales sin riesgo. **El componente victron_ble está duplicado entre `victron` (7") y `pantalla_3.5` (3.5"). Si arreglas en uno, copia al otro**.

---

## 7. Archivos clave

```
~/joint/victron/
├── main/ne185/ne185.c                 # implementación NE185 master mode
├── main/ne185/include/ne185.h         # API
├── main/ui/view_overview.c            # UI card camper con LOG ON / botones
├── main/api_rest.h                    # header REST API (stub, .c pendiente)
├── components/victron_ble/            # parser BLE (compartido conceptualmente con pantalla_3.5)
├── components/audio_es8311/           # codec con jingles BOOT_OK/CRITICAL/WARNING/CONFIRM
├── components/alerts/                 # thresholds NVS (freezer/SoC)
├── components/config_storage/         # persistencia general (Wi-Fi, screensaver, etc.)
├── sdkconfig                          # cuidado, verificar diff vs known-good antes de tocar
├── docs/ne185_bias_board.pdf          # placa de bias RS-485
├── AUTOCARAVANA-SETUP.md              # guía operativa autocaravana
├── SESION-2026-06-16-resumen.md       # resumen ejecutivo sesión post-vacaciones
└── VICTRON-CONTEXT.md                 # este archivo
```

---

## 8. Hardware

### 8.1 Pantalla 7" (victron)
- ESP32-P4 (principal) + ESP32-C6 vía SDIO (Wi-Fi/BT con esp_hosted)
- Display DSI 1024x600 (panel JD9165BA)
- Touch GT911, RTC RX8130, microSD slot 0 IOMUX
- Codec audio ES8311 + amplificador NS4150 (GPIO11 PA_CTRL)
- Pines I2S: MCLK=GPIO13, BCLK=GPIO12, LRCK=GPIO10, DOUT=GPIO9
- Ventilador frigo PWM en GPIO21 (esperando cableado)
- Bus 1-Wire DS18B20 en GPIO26 (pullup 4.7K) (esperando conexión)

### 8.2 RS-485 (NE185)
- MAX485 con auto-DE (no GPIO direction)
- UART2 a 38400 8N1
- Placa de bias: R1=R2=680ohm pull-up/down + R3=132ohm (220||330) terminación

### 8.3 Pantalla 3.5"
- ESP32-S3 con BLE nativo (a diferencia del P4 que usa esp_hosted)
- Por eso Orion DC/DC SÍ llega aquí pero NO en la 7"

---

## 9. Convenciones del proyecto

- Estética card-based aplicada en Settings (cada página con su color de borde)
- Textos en español, sin emojis (excepto símbolos `LV_SYMBOL_*`)
- Persistencia con NVS por componente (namespace propio)
- Logs sin acentos para evitar problemas de codificación

---

## 10. Pendientes activos (orden de prioridad)

### Pendientes Victron core (los más urgentes):
1. Sniff CHECK button del NE187 → entender frame15 canónico
2. Luces NE185 no apagan: probar `FF 01 00 80 80`
3. Orion DC/DC: verificar los 5 básicos antes de tocar código (ver §5.2 paso 3)
4. Ventilador GPIO21 (esperando cableado físico)
5. DS18B20 físicos (esperando conexión)

### Pendientes UI:
1. Debug rotación del salvapantallas (modo Rotar Live/Frigo/Batería cada N min)
2. Aplicar cards a vistas Live: battery_monitor, solar_charger, simple, simple_devices
3. Victron Keys 2 columnas (intento alternativo, el anterior no renderizaba textos)

### Pendientes API REST (header stub creado, .c pendiente):
- `/api/system` — uptime, heap, freertos stats
- `/api/ne185/state` — estado parsed (luces, tanks, bat)
- `/api/ne185/raw` — último frame raw + counters frames_ok/fail
- `/api/ne185/toggle/<b>` — envía press (b = luz_int|luz_ext|bomba)
- `/api/state` — composición JSON

---

## 11. Si algo se rompe / rollback

```bash
# Volver al estado de salida de viaje (tag pre-autocaravana):
git checkout pre-autocaravana-2026-06-16

# Solo el sdkconfig al known-good:
git checkout sdkconfig-known-good-2026-05-27 -- sdkconfig
idf.py reconfigure
idf.py build

# Si tras un git pull el build falla con CMakeCache path antiguo:
rm -rf build/    # NO fullclean. Solo build/. Mantiene managed_components/ y sdkconfig.
idf.py build
```

---

## 12. Continuar desde aquí

Próxima sesión (en autocaravana o de vuelta):
1. Leer este archivo entero (`cat VICTRON-CONTEXT.md`)
2. Leer `AUTOCARAVANA-SETUP.md` para comandos operativos
3. Empezar con el paso prioritario §5.2 paso 1 (sniff CHECK button NE187)
4. Documentar hallazgos en `SESION-YYYY-MM-DD-notas.md` para que las próximas sesiones tengan contexto

Cuando vuelvas al PC principal:
- `git pull` para traer cambios del viaje
- Pedirme actualizar las memorias persistentes con lo nuevo descubierto
