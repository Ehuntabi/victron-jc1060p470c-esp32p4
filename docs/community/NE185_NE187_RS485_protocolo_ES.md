# Protocolo RS-485 NordElettronica NE185 ⇄ NE187 — ingeniería inversa

**Estado:** funcionando, validado en hardware real (2026-06).
**Qué es esto:** una descripción completa y autocontenida del protocolo RS-485
entre la centralita **NordElettronica NE185** (la unidad de control/PSU de muchas
autocaravanas europeas) y su panel **NE187** — para que puedas **leer todos los
datos de la camper y/o sustituir el panel por tu propio microcontrolador**
(ESP32, Arduino, Raspberry Pi…).

No existe documentación pública/oficial de este protocolo. Se sacó por ingeniería
inversa con un sniffer pasivo del bus y se confirmó sustituyendo el panel por un
ESP32-P4. Si tienes un sistema NordElettronica, esto te ahorra semanas.

> Licencia CC0 / dominio público — copia, adapta y comparte libremente. Sin
> garantía; eres responsable de lo que hagas con la electricidad de tu vehículo.

---

## Resumen rápido

- **Bus:** RS-485, 2 hilos, **38400 baudios, 8N1**. Ambos usan un **ADM485**.
- **NE185 = esclavo** (tiene todos los datos: tanques, baterías, cargas).
  **NE187 = master** (sondea y muestra). El NE187 además aporta el **bias** del bus.
- **Poll (master → NE185), 5 bytes:** el panel **alterna** dos polls:
  - `FF 40 00 00 3F`
  - `FF 00 00 00 FF`  (dominante)
- **Respuesta (NE185 → master), 15 bytes** tras el poll. Con eco es una trama de
  20 bytes: `[poll 5B][respuesta 15B]`.
- **Checksum:** último byte = `suma(bytes 5..18) & 0xFF`.
- **La alternancia importa:** si sondeas **solo** con `FF 40`, el NE185 degrada a
  una trama que omite los bytes de tanque. Sondea con ambos (sobre todo `FF 00`).
- **Botón "CHECK" / arranque en frío:** tras dar corriente, el NE185 está mudo
  hasta que alguien lo sondea. El botón CHECK del panel **no manda ningún comando
  mágico** — simplemente **empieza a sondear**. Tu micro despierta la unidad con
  solo ponerse a sondear.

---

## 1. Hardware y conexionado

```
   NE187 (panel)                    NE185 (centralita / PSU)
   ┌───────────────┐   A  ───────►  ┌───────────────┐
   │  ADM485       │   B  ───────►  │  ADM485       │
   │  MASTER       │   GND ──────►  │  ESCLAVO      │
   │  + bias bus   │                │  tiene datos  │
   └───────────────┘                └───────────────┘
```

- Par diferencial RS-485 de 2 hilos **A**/**B** + **GND** común.
- Las **resistencias de bias están en el panel (NE187)**. Si lo quitas, debes
  poner tu propio bias o el bus flota y el NE185 no recibe tus polls. Bias típico:
  **A → +5V por ~680 Ω**, **B → GND por ~680 Ω**, y un terminador **120 Ω** A–B
  opcional (el NE185 probablemente termina internamente; a esta velocidad/longitud
  el terminador es opcional).
- **Referencia del bias = +5V** (el riel del ADM485), **no** los 12V de batería.
  Los 12V sacarían el idle diferencial fuera de spec.
- Pinout ADM485 (SOIC-8): pin 6 = **A**, pin 7 = **B**, pin 5 = GND, pin 8 = VCC.

---

## 2. Capa física

| Parámetro | Valor |
|-----------|-------|
| Estándar  | RS-485, 2 hilos half-duplex |
| Baudios   | **38400** |
| Formato   | 8N1 |
| Cadencia  | ~60–100 ms (el panel original sondea de continuo) |

---

## 3. Comandos de poll (master → NE185, 5 bytes)

Formato: `FF | b1 | 00 | 00 | checksum`, con `checksum = (b0+b1+b2+b3) & 0xFF`.

```
POLL A:  FF 40 00 00 3F
POLL B:  FF 00 00 00 FF      <-- dominante; el panel manda este casi siempre
```

### ⚠️ Hay que ALTERNAR los dos polls

El NE185 solo devuelve la trama **completa** (con tanques) cuando ve el patrón
del panel: sobre todo `FF 00 00 00 FF` con `FF 40 00 00 3F` intercalado. Si mandas
**solo `FF 40`**, el NE185 **degrada** y responde con una trama que sustituye los
dos primeros bytes (de tanque) por `F8 E0` (ver §5).

Ratio validado que funciona: manda `FF 00 00 00 FF` por defecto y
`FF 40 00 00 3F` ~**1 de cada 16**. (No hace falta clavarlo; lo importante es que
aparezcan los dos.)

### Comandos de control de cargas (toggle)

Al pulsar un botón del panel, mientras se mantiene, cambia `b1`/`b3`:

```
Luz interior:  FF 01 00 C0 C0
Luz exterior:  FF 02 00 C0 C1
Bomba de agua: FF 04 00 C0 C3
```

Estos **alternan** (toggle) la carga (no fijan estado). Para encender con
seguridad, lee primero el estado actual (`b15`) y manda el toggle solo si está
apagado. El NE185 necesita ver el comando **≥2 tramas consecutivas** para
registrar el cambio.

---

## 4. Trama de respuesta (NE185 → master)

En el bus, cada ciclo es `[poll 5B][respuesta 15B]` = **20 bytes**. Los 5
primeros son el eco del poll; los 15 siguientes, los datos.

```
Idx  Ejemplo   Significado
 0   FF        eco poll: cabecera
 1   40 / 00   eco poll: b1
 2   00        eco poll
 3   00        eco poll
 4   3F / FF   eco poll: checksum
 5   01        TANQUE AGUA LIMPIA (nibble bajo): 0/1/3/7/F -> 0, 1/4, 2/4, 3/4, lleno
 6   02        *** CONSTANTE 0x02 *** (úsalo para descartar basura, ver §6)
 7   00        TANQUE AGUAS GRISES: bit0 -> 0 vacío / 1 lleno
 8   40        constante (a veces 00)
 9   xx        variable (sensor/contador) — IGNORAR, pero ENTRA en el checksum
10   00        constante
11   FF        constante (en el bus; puede variar si lees sin eco)
12   9C        BATERÍA 1 (servicio):  V = (byte - 30) / 10  -> 0x9C=156 -> 12.6V
13   AB        BATERÍA 2 (motor):     V = (byte - 30) / 10  -> 0xAB=171 -> 14.1V
14   xx        variable — IGNORAR, pero en el checksum
15   81        BITMAP de estados:
                 bit0 = luz interior ON
                 bit1 = luz exterior ON
                 bit2 = bomba ON
                 bit7 = heartbeat del poll (alterna) — NO es la red 230V
16   31        RED 230V (shore): bit0 -> 0x31 conectada / 0x30 no
17   00        constante
18   00        constante
19   15        CHECKSUM
```

### Checksum

```
byte[19] = ( suma de byte[5..18] ) & 0xFF
```

Ejemplo — cuerpo `01 02 00 40 ED 00 FF 9C AB ED 81 31 00 00`:
suma = `0x515` → `& 0xFF` = `0x15` = byte[19]. ✓

### Variables útiles (las 8)

1. Luz interior on/off — `b15.0`
2. Luz exterior on/off — `b15.1`
3. Bomba on/off — `b15.2`
4. Agua limpia 0..4/4 — nibble bajo de `b5` (`0/1/3/7/F`)
5. Aguas grises vacío/lleno — `b7.0`
6. Red 230V presente — `b16.0` (`0x31`/`0x30`)
7. Batería servicio V — `(b12 - 30) / 10`
8. Batería motor V — `(b13 - 30) / 10`

---

## 5. La trama degradada `F8 E0` (sin tanques)

Si el NE185 no recibe el patrón de poll correcto, responde con una trama de 15
bytes que **omite los dos bytes de tanque**:

```
buena (con tanques):  01 02 00 40 EC 00 FF 9C AB EC 81 31 00 00 13
degradada (F8 E0):    F8 E0 00 40 EE 00 FF 9C AA ED 05 31 00 00 99
                      ^^^^^ los bytes de tanque (01 02) sustituidos por F8 E0
```

Del índice 3 en adelante es estructuralmente igual (batería, estados, red), así
que esos datos sí los tienes — solo pierdes los tanques. Solución: alternar los
polls (§3).

---

## 6. Descartar tramas basura

Como master, alguna trama mal alineada/parcial puede pasar por azar un checksum a
secas. Una trama válida **siempre tiene `byte[6] == 0x02`** (constante confirmada
en miles de tramas). Exígelo además del checksum:

```c
bool frame_valid(const uint8_t *b) {
    if (b[6] != 0x02) return false;        // descarta F8 E0 / basura
    uint16_t s = 0;
    for (int i = 5; i <= 18; i++) s += b[i];
    return (uint8_t)s == b[19];
}
```

(NO exijas también `b11 == 0xFF`: se cumple en el bus pero no siempre cuando lees
solo la respuesta de 15 bytes sin el eco.)

---

## 7. Leer sin eco (master, 2 hilos)

En RS-485 de 2 hilos puede que leas o no tu propio poll (el "eco"), según el
timing DE/RE de tu transceptor:

- **Con eco:** lees `[poll 5B][respuesta 15B]` → 20 bytes que empiezan por `FF`;
  validas directo.
- **Sin eco:** lees solo la respuesta de 15 bytes (empieza por el byte de tanque,
  sin cabecera fija). Reconstruye una trama de 20 bytes como
  `tu_poll(5) + respuesta(15)` y valida con el checksum.

Como la lectura cae en una fase arbitraria de un stream continuo, **escanea** el
buffer buscando una trama válida en cualquier offset, no asumas el offset 0.

---

## 8. Arranque en frío / el botón "CHECK"

En el garaje la batería suele estar desconectada. Al reconectar, el NE185 está
**mudo**: solo habla si lo sondean, y el panel queda en idle (el bus lee todo
`FF`) hasta que pulsas **CHECK**.

Comportamiento capturado del CHECK: **no manda ningún comando de init/wake
especial**. Simplemente hace que el panel **empiece a sondear** — primero ~2 s de
`FF 00 00 00 FF`, luego pasa a `FF 40 00 00 3F`. El NE185 responde con datos
completos (tanques incluidos) desde el primer poll.

**Consecuencia para sustituir el panel:** tu micro despierta la unidad con solo
**ponerse a sondear** (con la alternancia de §3). Sin CHECK, sin secuencia
especial, sin hardware extra. El CHECK solo despierta la unidad; **no** enciende
cargas — eso es pulsar esos botones aparte.

---

## 9. Sustituir el panel por tu micro — checklist

1. Conecta tu transceptor RS-485 a A/B/GND. Añade bias (§1) porque has quitado el
   panel que lo aportaba.
2. UART **38400 8N1**.
3. Sondea de continuo, **alternando** `FF 00 00 00 FF` (por defecto) y
   `FF 40 00 00 3F` (~1 de cada 16).
4. Parsea la respuesta de 15 bytes (reconstruye a 20B si no hay eco), valida con
   `byte[6]==0x02` + checksum.
5. Decodifica las 8 variables (§4).
6. Para controlar cargas, manda el toggle (§3) durante ≥2 tramas, tras leer el
   estado actual para no encender/apagar al revés.
7. El arranque en frío "funciona solo": ponte a sondear al boot y la unidad
   despierta.

---

## 10. Créditos y procedencia

Ingeniería inversa en junio de 2026 sobre una autocaravana real (NE185 + NE187),
esnifando el bus en vivo con un ESP32-P4 y luego sustituyendo el panel por él.
Capturas, mapa de bytes completo e implementación funcional en ESP-IDF en:

- Repo: https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4
  (`main/ne185/ne185.c`, `docs/ne185_protocolo_completo.md`)

Trabajo previo relacionado: `class142/ne-rs485` documenta el panel hermano
**NE334** (comandos `FF 0X`/`FF 8X`, sondeo más lento). El NE187/NE185 de aquí usa
la misma familia de comandos pero distinto sondeo/respuesta/checksum — documentado
arriba por primera vez, que sepamos.

Si te ha servido, añade las particularidades de tu modelo para que al siguiente le
sea aún más fácil. 🚐
