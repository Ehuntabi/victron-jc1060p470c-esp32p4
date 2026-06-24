# NordElettronica NE185 ⇄ NE187 RS-485 protocol — reverse-engineered

**Status:** working, validated on real hardware (2026-06).
**What this is:** a complete, self-contained description of the RS-485 protocol
between a **NordElettronica NE185** control unit (the "centralina"/PSU found in
many European motorhomes/caravans) and its **NE187** display panel — so you can
**read all the camper data and/or replace the panel with your own
microcontroller** (ESP32, Arduino, Raspberry Pi, …).

There is no public/official documentation for this protocol. This was
reverse-engineered with a passive bus sniffer and confirmed by replacing the
panel with an ESP32-P4. If you have a NordElettronica system, this should save
you weeks.

> Licensed CC0 / public domain — copy, adapt, share freely. No warranty; you are
> responsible for what you do to your vehicle's electrics.

---

## TL;DR

- **Bus:** RS-485, 2-wire, **38400 baud, 8N1**. Both devices use an **ADM485**.
- **NE185 = slave** (holds all the data: tanks, batteries, loads). **NE187 =
  master** (polls + displays). The NE187 also provides the bus **bias**.
- **Poll (master → NE185), 5 bytes:** the panel alternates two polls:
  - `FF 40 00 00 3F`
  - `FF 00 00 00 FF`  (dominant)
- **Response (NE185 → master), 15 bytes** following the poll. With echo it's a
  20-byte frame: `[poll 5B][response 15B]`.
- **Checksum:** last byte = `sum(bytes 5..18) & 0xFF`.
- **The alternation matters:** if you poll with `FF 40` *only*, the NE185
  degrades to a frame that omits the tank bytes. Poll with both (mostly `FF 00`)
  to get full data.
- **"CHECK" button / cold start:** after power-up the NE185 is silent until
  something polls it. The panel's CHECK button doesn't send a magic command — it
  just **starts polling**. So your own MCU wakes the unit simply by polling.

---

## 1. Hardware & wiring

```
   NE187 (panel/display)            NE185 (control unit / PSU)
   ┌───────────────┐   A  ───────►  ┌───────────────┐
   │  ADM485       │   B  ───────►  │  ADM485       │
   │  MASTER       │   GND ──────►  │  SLAVE        │
   │  + bus bias   │                │  has the data │
   └───────────────┘                └───────────────┘
```

- 2-wire RS-485 differential pair **A**/**B** + common **GND**.
- The **bias resistors live in the panel (NE187)**. If you remove the panel you
  must add your own bias or the bus floats and the NE185 won't receive your
  polls. Typical bias: **A → +5V via ~680 Ω**, **B → GND via ~680 Ω**, and an
  optional **120 Ω** A–B terminator (the NE185 likely terminates internally, so
  the terminator is optional at this speed/length).
- **Bias reference = +5V** (the ADM485 rail), **not** the 12V battery. 12V would
  push the idle differential out of spec.
- ADM485 pinout (SOIC-8): pin 6 = **A**, pin 7 = **B**, pin 5 = GND, pin 8 = VCC.

---

## 2. Physical layer

| Parameter | Value |
|-----------|-------|
| Standard  | RS-485, 2-wire half-duplex |
| Baud      | **38400** |
| Framing   | 8N1 |
| Poll rate | ~60–100 ms (the original panel polls continuously) |

---

## 3. Poll commands (master → NE185, 5 bytes)

Format: `FF | b1 | 00 | 00 | checksum`, where `checksum = (b0+b1+b2+b3) & 0xFF`.

```
POLL A:  FF 40 00 00 3F
POLL B:  FF 00 00 00 FF      <-- dominant; the panel sends this most of the time
```

### ⚠️ You must alternate the two polls

The NE185 only returns the **full** frame (with tank levels) when it sees the
panel's polling pattern: mostly `FF 00 00 00 FF` with `FF 40 00 00 3F`
interspersed. If you send **`FF 40` only**, the NE185 **degrades** and answers
with a frame that replaces the first two (tank) bytes with `F8 E0` (see §5).

A working ratio that has been validated: send `FF 00 00 00 FF` by default and
`FF 40 00 00 3F` roughly **1 poll in 16**. (You don't need to match this exactly;
the point is that both polls appear.)

### Load-control commands (toggle a load)

Pressing a button on the panel sends, while held, a different `b1`/`b3`:

```
Interior light:  FF 01 00 C0 C0
Exterior light:  FF 02 00 C0 C1
Water pump:      FF 04 00 C0 C3
```

These **toggle** the load (they don't set a state). To turn something on
reliably, read the current state first (see `b15` below) and only send the toggle
if it's currently off. The NE185 needs to see the command for **≥2 consecutive
frames** to register the toggle.

---

## 4. Response frame (NE185 → master)

On the wire each cycle is `[poll 5B][response 15B]` = **20 bytes**. The first 5
bytes are the echo of the poll; the next 15 carry the data.

```
Idx  Example   Meaning
 0   FF        poll echo: header
 1   40 / 00   poll echo: b1
 2   00        poll echo
 3   00        poll echo
 4   3F / FF   poll echo: checksum
 5   01        CLEAN-WATER tank (low nibble): 0/1/3/7/F -> 0, 1/4, 2/4, 3/4, full
 6   02        *** CONSTANT 0x02 *** (use it to reject garbage frames, see §6)
 7   00        GREY-WATER tank: bit0 -> 0 empty / 1 full
 8   40        constant (occasionally 00)
 9   xx        variable (sensor/counter) — ignore, but it IS in the checksum
10   00        constant
11   FF        constant (on the wire; may differ in echo-less reads)
12   9C        BATTERY 1 (service):  volts = (byte - 30) / 10  -> 0x9C=156 -> 12.6V
13   AB        BATTERY 2 (vehicle):  volts = (byte - 30) / 10  -> 0xAB=171 -> 14.1V
14   xx        variable — ignore, but in the checksum
15   81        STATUS bitmap:
                 bit0 = interior light ON
                 bit1 = exterior light ON
                 bit2 = water pump ON
                 bit7 = poll heartbeat (alternates) — NOT mains
16   31         MAINS (shore power) 230V: bit0 -> 0x31 connected / 0x30 not
17   00         constant
18   00         constant
19   15         CHECKSUM
```

### Checksum

```
byte[19] = ( sum of byte[5..18] ) & 0xFF
```

Worked example — body `01 02 00 40 ED 00 FF 9C AB ED 81 31 00 00`:
sum = `0x515` → `& 0xFF` = `0x15` = byte[19]. ✓

### Decoded variables (the useful 8)

1. Interior light on/off — `b15.0`
2. Exterior light on/off — `b15.1`
3. Water pump on/off — `b15.2`
4. Clean-water tank 0..4/4 — `b5` low nibble (`0/1/3/7/F`)
5. Grey-water tank empty/full — `b7.0`
6. Mains 230V present — `b16.0` (`0x31`/`0x30`)
7. Service battery volts — `(b12 - 30) / 10`
8. Vehicle battery volts — `(b13 - 30) / 10`

---

## 5. The degraded `F8 E0` frame (no tanks)

If the NE185 doesn't get the right polling pattern, it answers with a 15-byte
frame that **omits the two tank bytes**:

```
full (with tanks):  01 02 00 40 EC 00 FF 9C AB EC 81 31 00 00 13
degraded (F8 E0):   F8 E0 00 40 EE 00 FF 9C AA ED 05 31 00 00 99
                    ^^^^^ tank bytes (01 02) replaced by F8 E0
```

Everything from index 3 on is structurally the same (battery, status, mains),
so you still get those — you just lose the tank levels. Fix: alternate the polls
(§3).

---

## 6. Rejecting garbage frames

When you act as master, occasional malformed/partial frames can accidentally
pass a pure checksum test. A valid frame **always has `byte[6] == 0x02`** (a
constant confirmed across thousands of frames). Require that in addition to the
checksum:

```c
bool frame_valid(const uint8_t *b) {
    if (b[6] != 0x02) return false;        // reject F8 E0 / garbage
    uint16_t s = 0;
    for (int i = 5; i <= 18; i++) s += b[i];
    return (uint8_t)s == b[19];
}
```

(Do **not** also require `b11 == 0xFF`: it holds on the wire but not always when
you read only the 15-byte response without the echo.)

---

## 7. Reading without an echo (master, 2-wire)

In half-duplex 2-wire RS-485 you may or may not read back your own poll (the
"echo"), depending on your transceiver's DE/RE timing:

- **Echo present:** you read `[poll 5B][response 15B]` → 20 bytes starting with
  `FF`; validate directly.
- **No echo:** you read only the 15-byte response (starts with the tank byte, no
  fixed header). Rebuild a 20-byte frame as `your_poll(5) + response(15)` and
  validate with the checksum above.

Because the read lands at an arbitrary phase of a continuous stream, **scan** the
buffer for a valid frame at any offset rather than assuming offset 0.

---

## 8. Cold start / the "CHECK" button

In a garage the battery is often disconnected. On reconnect the NE185 is
**silent**: it only talks when polled, and the panel sits idle (bus reads all
`FF`) until you press **CHECK**.

Captured behaviour of CHECK: it sends **no special init/wake command**. It simply
makes the panel **start polling** — first ~2 s of `FF 00 00 00 FF`, then settling
into `FF 40 00 00 3F`. The NE185 answers with full data (tanks included) from the
very first poll.

**Consequence for a replacement:** your MCU wakes the unit just by **starting to
poll** (with the alternation from §3). No CHECK, no special sequence, no extra
hardware. CHECK only wakes the unit; it does **not** turn on loads — that's a
separate button press.

---

## 9. Replacing the panel with your own MCU — checklist

1. Wire your RS-485 transceiver to A/B/GND. Add bias (§1) since you removed the
   panel that provided it.
2. UART **38400 8N1**.
3. Poll continuously, **alternating** `FF 00 00 00 FF` (default) and
   `FF 40 00 00 3F` (~1 in 16).
4. Parse the 15-byte response (rebuild to 20B if no echo), validate with
   `byte[6]==0x02` + checksum.
5. Decode the 8 variables (§4).
6. To control loads, send the toggle command (§3) for ≥2 frames, after checking
   current state so you don't toggle the wrong way.
7. Cold start "just works": start polling on boot and the unit wakes.

---

## 10. Credits & provenance

Reverse-engineered in June 2026 on a real motorhome (NE185 + NE187), by sniffing
the live bus with an ESP32-P4 and then replacing the panel with it. Captures,
the full byte map, and a working ESP-IDF implementation are in:

- Repo: https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4
  (`main/ne185/ne185.c`, `docs/ne185_protocolo_completo.md`)

Related prior art: `class142/ne-rs485` documents the **NE334** sibling panel
(commands `FF 0X`/`FF 8X`, slower polling). The NE187/NE185 here uses the same
command family but a different polling/response/checksum — documented above for
the first time, as far as we know.

If this helped you, consider adding your own model's quirks so the next person
has it even easier. 🚐
