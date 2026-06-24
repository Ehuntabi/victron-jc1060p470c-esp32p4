# Protocolo NE185 RS-485 — Documentación completa

> **Estado: RESUELTO Y VERIFICADO EN VIVO (2026-06-24).**
> El ESP32-P4 reemplaza al panel NE187 como único master del bus y lee
> **todos** los datos camper, incluidos los **niveles de agua** (que llevaban
> meses sin funcionar). Verificado en banco: 73 tramas con tanque vs ~5
> degradadas filtradas; datos en pantalla (Overview) confirmados por el usuario.

Esta es, probablemente, la primera ingeniería inversa pública completa del
protocolo del NordElettronica **NE187 ↔ NE185** (no hay documentación oficial).

Código de referencia: `main/ne185/ne185.c`.
Memorias relacionadas: `project_ne185_protocol`, `project_ne185_tanks_replace_topology`,
`project_ne185_captures_no_bias`.

---

## 1. Topología del bus

```
   ┌──────────┐   RS-485 (2 hilos A/B + GND)   ┌──────────┐
   │  NE187   │◄──────────────────────────────►│  NE185   │
   │ (panel)  │      38400 baud, 8N1           │ (central)│
   └──────────┘                                 └──────────┘
        │                                            │
   Es el MASTER:                              Es el ESCLAVO:
   - sondea (poll)                            - tiene TODOS los datos
   - muestra los datos                          (tanques, baterias, sensores)
   - lleva el BIAS HW del bus                 - responde al poll
```

### Roles
- **NE185** = central electrónica. **Recibe y tiene todos los datos** (tanques,
  baterías, estados de luces/bomba, shore 230V). Es el esclavo: solo habla
  cuando lo sondean.
- **NE187** = panel/display. **Solo muestra**. Es el **master**: sondea al
  NE185 y además aporta la **polarización (bias)** del bus.

### Objetivo del proyecto
Sustituir el NE187 por el ESP32-P4: que el P4 sea el master, sondee al NE185 y
pinte los datos en la pantalla DSI 1024×600.

### Implicación crítica del bias
Al quitar el NE187 se pierden **dos cosas**:
1. El **master** (sin él, nadie sondea → el NE185 calla).
2. El **bias HW** del bus (sin él, el bus flota en idle → el NE185 no recibe el
   poll limpio y no responde → **"no hay datos camper"**).

Por eso, para que el P4 reemplace al NE187 hace falta una **placa de bias**
externa que reponga la polarización. Ver `docs/ne185_bias_board.pdf`
(R1=680Ω pull-up A, R2=680Ω pull-down B, R3≈120Ω terminación). Sin esa placa,
el reemplazo no funciona aunque el firmware sea correcto.

---

## 2. Capa física

| Parámetro | Valor |
|-----------|-------|
| Estándar | RS-485 2 hilos (half-duplex) |
| Baudios | **38400** |
| Formato | 8N1 |
| Transceptor | ADM485JRZ (mismo chip en NE187 y NE185) |
| Cadencia poll | ~100 ms (16 Hz) en nuestro firmware; el NE187 original ~60–100 ms |
| Bias | pull-up A→+5V 680Ω, pull-down B→GND 680Ω, terminación A-B ≈120Ω |

Referencia de tensión del bias: **+5V del ESP**, NO los 12V de batería (el
ADM485 tiene VCC 4.75–5.25V; 12V saturaría el idle y rozaría el ABS MAX +13V).

---

## 3. Comandos (poll): master → NE185, 5 bytes

Formato: `FF | b1 | 00 | 00 | checksum` donde `checksum = (b0+b1+b2+b3) & 0xFF`.

```
CMD_IDLE   = FF 40 00 00 3F     (poll "A")
CMD_IDLE2  = FF 00 00 00 FF     (poll "B", DOMINANTE en el NE187)
```

### ⚠️ Hallazgo clave: la ALTERNANCIA de polls es obligatoria

El NE187 **alterna** los dos polls: manda `FF 00 00 00 FF` la mayor parte del
tiempo y `FF 40 00 00 3F` de vez en cuando. **El NE185 solo entrega los tanques
si ve esa alternancia.**

- Si el master manda **solo `FF 40 00 00 3F`** → el NE185 **degrada** y responde
  con una trama `F8 E0 …` **sin los bytes de tanque**.
- Replicando la alternancia (en nuestro firmware: `FF 40` 1 de cada 16 polls, el
  resto `FF 00`) → el NE185 vuelve a dar la trama buena `01 02 …` con tanques.

Implementación: `main/ne185/ne185.c`, FSM idle:
```c
static uint32_t s_idle_cnt = 0;
if ((s_idle_cnt++ % 16) == 0) tx_cmd = CMD_IDLE;   /* FF 40 00 00 3F */
else                          tx_cmd = CMD_IDLE2;  /* FF 00 00 00 FF */
```

### Comandos de control (botones)
```
CMD_BTN_LIN  = FF 01 00 C0 C0    (luz interior, bit 0)
CMD_BTN_LOUT = FF 02 00 C0 C1    (luz exterior, bit 1)
CMD_BTN_PUMP = FF 04 00 C0 C3    (bomba,        bit 2)
```
Semántica de "press hold": para que el NE185 procese un toggle, el master debe
enviar el comando del botón durante **≥2 frames consecutivos** (usamos 8 frames
de margen), y luego volver a idle ≥2 frames (release) antes de aceptar otra
pulsación del mismo botón (evita doble toggle).

---

## 4. Respuesta del NE185: trama canónica de 20 bytes

En el bus, cada ciclo es `[poll 5B][respuesta 15B]` = **20 bytes contiguos**.
Los 5 primeros son el eco del poll; los 15 siguientes, los datos.

### Layout byte a byte

```
Idx  Valor ejemplo   Significado
 0   FF              eco poll: header
 1   40 / 00         eco poll: b1 (segun poll A/B)
 2   00              eco poll: const
 3   00              eco poll: const
 4   3F / FF         eco poll: checksum
 5   01              TANQUE LIMPIO (nibble bajo): 0/1/3/7/F -> 0, 1/4, 2/4, 3/4, 4/4
 6   02              *** CONSTANTE 0x02 *** (discriminador anti-basura, ver §6)
 7   00              TANQUE GRISES: bit0 -> 0 vacio / 1 lleno
 8   40              constante (glitch ocasional 00)
 9   ED/EC/EA…       variable (sensor/contador) — IGNORAR, pero entra en checksum
10   00              constante
11   FF              constante (en sniff; en master no siempre es FF)
12   9C              BATERIA 1 (servicio): V = (byte - 30) / 10  -> 0x9C=156 -> 12.6V
13   AB              BATERIA 2 (motor):    V = (byte - 30) / 10  -> 0xAB=171 -> 14.1V
14   ED/EC           variable — IGNORAR, pero entra en checksum
15   81 / 05 / 85    BITMAP estados:
                       bit0 = luz interior ON
                       bit1 = luz exterior ON
                       bit2 = bomba ON
                       bit7 = HEARTBEAT del poll (alterna). NO es shore.
16   31 / 30         SHORE 230V: bit0 -> 0x31 con red / 0x30 sin red
17   00              constante
18   00              constante
19   15              CHECKSUM
```

### Checksum (REAL, descubierto 2026-06-24)

```
b[19] = ( suma de b[5..18] ) & 0xFF
```

Ejemplo verificado:
```
cuerpo (b5..b18): 01 02 00 40 ED 00 FF 9C AB ED 81 31 00 00
suma = 0x515  ->  & 0xFF = 0x15  ==  b19 (0x15)   ✓
```

> ⚠️ La fórmula **antigua** `(b5+b9+b14+b15+0xB1) & 0xFF` era **errónea**.
> Hacía que **ninguna** respuesta pasara el checksum → en master el firmware
> caía a la trama nativa degradada (sin tanques) y daba lecturas falsas/parpadeo
> en botones y 230V.

### Variables útiles para el usuario (8)
1. Luz interior on/off — `b15.0`
2. Luz exterior on/off — `b15.1`
3. Bomba on/off — `b15.2`
4. Agua limpia 0..4/4 — `b5` nibble bajo
5. Agua grises vacío/lleno — `b7.0`
6. 230V (shore) on/off — `b16.0`
7. Batería habitáculo V — `b12`
8. Batería motor V — `b13`

---

## 5. Trama degradada `F8 E0` (sin tanques)

Cuando el NE185 **no** recibe la alternancia correcta (p.ej. el master manda
solo `FF 40`), responde con una trama de 15 bytes que **omite los 2 bytes de
tanque**:

```
buena (con tanque):  01 02 00 40 EC 00 FF 9C AB EC 81 31 00 00 13
degradada (F8 E0):   F8 E0 00 40 EE 00 FF 9C AA ED 05 31 00 00 99
                     ↑↑↑↑↑ los 2 primeros bytes (clean=01, const=02)
                           se reemplazan por F8 E0 = SIN tanque
```

Del **byte 3 en adelante es idéntico** en estructura (40, var, 00, FF, bat1,
bat2, var, bitmap, shore, 00, 00, checksum). Es decir: en modo degradado siguen
llegando baterías, luces, bomba y shore; **solo se pierde el tanque**.

Por eso, aun en degradado, "hay datos camper" en pantalla (todo menos tanques).

---

## 6. Filtrado de tramas parásitas

Al sondear como master aparecen, intercaladas, tramas basura (la `F8 E0` y
fragmentos mal alineados) que **por azar pueden cuadrar el checksum** y colarse
como datos falsos (`b6=0xFF`, batería = -3.0V, parpadeo).

**Filtro:** una trama válida **siempre** tiene `b6 == 0x02` (confirmado en 2056
tramas históricas y en vivo). La basura no. Implementado en `checksum_ok()`:

```c
static bool checksum_ok(const uint8_t *b)
{
    if (b[6] != 0x02) return false;          /* descarta parasitas F8 E0/basura */
    uint16_t sum = 0;
    for (int i = 5; i <= 18; i++) sum += b[i];
    return (uint8_t)sum == b[19];
}
```

> ⚠️ **NO** usar `b11 == 0xFF` como filtro adicional: en modo master `b11` no
> siempre es 0xFF y rechazaba las tramas buenas (regresión "no hay datos camper"
> del 2026-06-24). Solo `b6 == 0x02` es fiable como discriminador.

---

## 7. Lógica del receptor (master) — escáner de frame

El read coge `RX_READ_LEN` (40) bytes en una fase arbitraria del stream. Se
**escanea** buscando un frame válido en cualquier offset:

1. **Caso B (echo presente):** el buffer trae `[poll 5B][resp 15B]` = 20 B que
   empieza en `FF` y pasa `checksum_ok` directamente.
2. **Caso A (sin echo):** solo llega la respuesta de 15 B (empieza por el nibble
   de tanque, sin cabecera fija). Se reconstruye `frame20 = tx_cmd(5) + resp(15)`
   y se valida con `checksum_ok`.

El checksum (suma b5..b18) + el filtro `b6==0x02` son suficientemente
discriminantes para no dar falsos positivos. El antiguo path "nativo F8 E0" se
eliminó (era el artefacto de responder al poll erróneo).

---

## 8. Modos de operación del firmware

| Modo | `s_polling_paused` | Qué hace | Para qué |
|------|--------------------|----------|----------|
| **MASTER** | `false` (default) | Sondea (alternancia) y **parsea a la UI** | Reemplazar al NE187 |
| **SNIFF**  | `true`  | Read-only: NO transmite, **loguea** POLL/RESP/RAW | Ingeniería inversa con el NE187 puesto |

Se conmuta desde **Settings → Logs → botón "MASTER MODE" ⟷ "SNIFF ON"**
(`main/ui/settings_logs_panel.c`). NB: está en la página **Logs**, no en Consola.

### Logs del modo SNIFF (capturados a fichero vía pyserial)
- `POLL t=…: FF 40 00 00 3F` — poll detectado (5 B, checksum FF), nivel W.
- `RESP t=…: <20 bytes> clean(b5)=.. grey(b7)=.. shore(b16)=..` — respuesta válida.
- `RAW n=40: <hex>` — volcado crudo del buffer (dedup vs anterior), para ver la
  respuesta completa sea cual sea su formato.

---

## 9. Cómo capturar el bus (workflow de banco)

1. **Hardware:** NE187 + NE185 en el bus, P4 enchufado en paralelo (con su bias).
2. **Firmware:** poner el P4 en **SNIFF ON** (Settings → Logs).
3. **Captura a fichero** con pyserial (NO `idf.py monitor`):
   ```bash
   PY=~/.espressif/python_env/idf5.4_py3.12_env/bin/python   # tiene pyserial
   $PY cap.py /dev/ttyACM0 20 > captura.txt                  # 20 s
   ```
   (`cap.py`: abre el puerto a 115200, lee líneas N segundos y vuelca a stdout.)
4. **Para reemplazar al NE187:** desconectar el NE187, conectar la **placa de
   bias**, poner el P4 en **MASTER MODE**. Los tanques deben salir en Overview.

> En el portátil el puerto es **/dev/ttyACM0** (no ttyACM1). Build/flash con
> `. ~/.espressif/esp-idf-5.4/export.sh` (ESP-IDF v5.4.4).

---

## 10. Cronología de la ingeniería inversa (2026-06-24)

| Paso | Hallazgo |
|------|----------|
| 1 | Las capturas previas se hicieron **sin placa de bias** → idle contaminado, alineación poco fiable (pero payload válido). |
| 2 | En modo master **sin** el NE187, el P4 no obtenía tanques: la trama nativa de 15B no los lleva en esos offsets. |
| 3 | El usuario aclara la arquitectura: **el NE185 tiene los datos, el NE187 solo muestra** → los tanques DEBEN viajar por el bus. |
| 4 | Sniff limpio (NE187 master + P4 sniff) captura el **poll real**: alterna `FF 40 00 00 3F` y `FF 00 00 00 FF`. El código tenía `FF 40 00 80 BF` (error de mayo). |
| 5 | El **RAW** revela la respuesta: `[poll][01 02 00 40 …]` con **`b5=01` → tanque 1/4**, baterías 9C/AB, shore 31. |
| 6 | Se descubre el **checksum real**: `suma(b5..b18) & 0xFF` (la fórmula vieja fallaba → 0 RESP). |
| 7 | En master, el NE185 degradaba a `F8 E0` (sin tanque) porque el P4 mandaba **solo `FF 40`**. |
| 8 | **Solución:** replicar la **alternancia** de polls + filtro `b6==0x02`. → 73 tramas `b5=01` válidas vs ~5 `F8 E0` filtradas. **Tanques en pantalla.** |

| 9 | **Arranque en frío:** tras un corte de batería el NE187 queda en standby (bus en idle `FF FF FF`, no sondea). Se captura que el **CHECK** del NE187 NO manda init especial: simplemente **empieza a sondear** (FF00 ~2s, luego FF40) y el NE185 responde con tanques desde el primer poll. |
| 10 | Como el wake = "ponerse a sondear", el **P4 en master lo hace solo**: confirmado en vivo que el P4 (sin NE187, con bias) despierta la centralita tras ciclar batería. |
| 11 | **Prueba real definitiva:** P4 a batería (USB fuera), ciclo off/on → el P4 arranca en frío, despierta la centralita y **auto-enciende luz int + bomba**. "Hay magia." 🎉 |

### Commits
- `adacaa3` — modo captura de poll en sniff (POLL/RESP segmentado + timestamp).
- `47f486f` — protocolo crackeado: poll `FF 40 00 00 3F` + checksum `suma(b5..18)` + reconstrucción resp 15B.
- `c5ddeeb` — alternancia de polls (`CMD_IDLE2` + FSM 1/16) + filtro `b6==0x02` → tanques en master.
- `0566261` — **auto-encendido** de luz int + bomba al arranque (toggle Settings + NVS).

---

## 11. Arranque en frío / función del CHECK

Escenario real: en el garaje la batería se desconecta. Al reconectarla, la
centralita queda dormida hasta que se le sondea.

Capturado en vivo (P4 en sniff, NE187 conectado, ciclo de batería off/on):

1. Tras reconectar, el NE187 queda en **STANDBY**: bus en idle puro `FF FF FF`,
   **no sondea, no arranca solo**.
2. Al pulsar **CHECK** en el NE187, el bus pasa de idle a sondeo:
   - un flanco `80`,
   - ~2 s sondeando con `FF 00 00 00 FF` → el NE185 responde `01 02…` con tanque
     (`clean b5=1`) **desde el primer poll**,
   - luego pasa a `FF 40 00 00 3F` en régimen estable.

> **El CHECK NO manda ningún comando de init/wake especial. Solo hace que el
> NE187 EMPIECE A SONDEAR** (FF00 de entrada para "cebar", luego FF40).

Como eso es exactamente lo que hace el P4 en modo master (alternancia FF00/FF40),
**el P4 despierta la centralita él solo** — sin NE187 ni CHECK. Confirmado en
condiciones reales (P4 a batería, ciclo off/on): el P4 arranca en frío, despierta
la centralita y muestra tanques + datos camper. **El P4 reemplaza al NE187 al
100%, arranque en garaje incluido.**

(Nota: el CHECK despierta la centralita pero **no** enciende cargas; `b15=00`.
Encender luz/bomba es pulsar esos botones aparte — automatizado en §12.)

---

## 12. Auto-encendido de cargas al arranque (feature)

Replica lo que el usuario hacía manualmente: al reconectar la batería en el
garaje, encender luz interior + bomba. Ahora el P4 lo hace solo.

**Comportamiento** (`ne185.c`):
- Cuando el P4 arranca y la centralita despierta (**≥10 tramas buenas**, estado
  fiable), si la función está activada **enciende luz int + bomba SI están
  apagadas** (toggle condicional: nunca las apaga).
- **One-shot por wake.** Se **rearma si el bus muere y vuelve** (`consec_timeouts
  == BUS_DEAD_THRESH` → `s_autostart_done = false`), útil si el P4 sigue vivo por
  USB y solo cae el bus.

**Persistencia / UI:**
- Flag en NVS (namespace `ne185`, key `autostart`), **default OFF**.
  `load_autostart_loads()` / `save_autostart_loads()` en `config_storage`.
- Toggle **`⏻ AUTO ON/OFF`** en **Settings → Consola** (junto a MASTER/SNIFF/LOG).
- API: `ne185_set_autostart(bool)` (persiste) / `ne185_get_autostart()`.

**Verificado en condiciones reales (2026-06-24):** P4 a batería, USB fuera, ciclo
off/on → arranque en frío → luz int + bomba se encienden solas. ✅

---

## 13. Pendientes / a confirmar

- Solo se ha visto en vivo el nivel de agua limpia **1/4** (`b5=0x01`). Los demás
  niveles (0, 2/4, 3/4, 4/4) y grises lleno usan el mismo mapeo ya validado en
  sesiones anteriores, pero no se han visto cambiar en este montaje. Para
  confirmarlos: puentear **JP9** (limpia) y **JP7** (grises) en el shunt y ver
  `clean`/`grey` cambiar en el log.
- ~5% de tramas siguen siendo `F8 E0` (filtradas) → refresco un pelín más lento,
  imperceptible en UI. El ratio de alternancia (1/16) es empírico; afinable.
- Control de botones (luces/bomba) desde el P4: los comandos `FF 0X 00 C0`
  están en el código y el auto-encendido los usa (luz int + bomba) con éxito;
  falta re-verificar luz ext de forma aislada.
