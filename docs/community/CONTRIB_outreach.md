# Textos de difusión (listos para pegar)

Borradores para aportar el hallazgo a la comunidad. El **publicado externo lo
decide/ejecuta el usuario** (o autoriza explícitamente). Enlazan al doc completo
del repo.

---

## A) Issue para `class142/ne-rs485` (en inglés)

**Título:**
`NE187/NE185 protocol (sibling of NE334): polls, 20-byte frame, checksum, panel replacement`

**Cuerpo:**

> Thanks for documenting the NE334 — it was the closest prior art I found while
> reverse-engineering the **NordElettronica NE187 panel ⇄ NE185 control unit** in
> my motorhome. I ended up replacing the panel with an ESP32 and mapped the whole
> protocol. Sharing in case it helps others, and to extend your docs to this model.
>
> Same command family as the NE334 (`FF 4X` idle/keep-alive, `FF 0X` load
> control), but **different polling, response layout and checksum**:
>
> - **38400 8N1**, ADM485 on both ends. NE187 = master + bus bias; NE185 = slave
>   holding the data.
> - **Polls alternate** `FF 40 00 00 3F` and `FF 00 00 00 FF` (dominant). With
>   `FF 40` only, the unit degrades and drops the tank bytes (`F8 E0`).
> - **Response = 15 bytes** after the poll (20B with echo). Clean-water tank =
>   low nibble of byte 5 (`0/1/3/7/F`), grey = byte 7 bit0, batteries = `(b-30)/10`
>   at bytes 12/13, status bitmap at byte 15, mains at byte 16.
> - **Checksum** = `sum(bytes 5..18) & 0xFF`.
> - The panel's **CHECK** button doesn't send a wake command — it just **starts
>   polling**; so a replacement MCU wakes the unit simply by polling.
>
> Full byte map, captures, gotchas and a working ESP-IDF implementation:
> https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4/blob/main/docs/community/NE185_NE187_RS485_protocol_EN.md
>
> Happy to turn this into a PR adding an `NE187`/`NE185` section to the repo if
> you'd like.

(Reemplaza `https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4/blob/main/docs/community/NE185_NE187_RS485_protocol_EN.md` por la URL final del doc en GitHub.)

---

## B) Post para foros de autocaravana (español)

**Título:** `He "hackeado" el panel NordElettronica NE187/NE185 (RS-485) — protocolo completo`

> A quien tenga una autocaravana con centralita **NordElettronica NE185** y panel
> **NE187**: he sacado por ingeniería inversa el protocolo RS-485 entre ambos y
> he **sustituido el panel por un ESP32**, leyendo tanques de agua, baterías,
> luces, bomba y 230V — e incluso encendiendo cargas y arrancando la centralita
> en frío (como el botón CHECK) automáticamente.
>
> No hay documentación oficial de esto, así que lo dejo público por si le sirve a
> alguien (CC0, dominio público):
>
> - Bus RS-485, 38400 8N1. El panel sondea alternando `FF 40 00 00 3F` y
>   `FF 00 00 00 FF`; la centralita responde con una trama de 15 bytes con todos
>   los datos.
> - Mapa de bytes completo, checksum, el truco de la alternancia de polls y cómo
>   sustituir el panel: https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4/blob/main/docs/community/NE185_NE187_RS485_protocolo_ES.md
>
> Si tienes otro modelo (NE334, etc.), comparte sus diferencias y ampliamos la doc.

---

## C) Post corto para Reddit (r/vandwellers, r/Coachmen, r/diyelectronics) — inglés

**Título:** `Reverse-engineered the NordElettronica NE187/NE185 RS-485 protocol (motorhome control panel) — full byte map + panel replacement`

> Common control unit in European campervans, zero public docs. Sniffed the bus,
> mapped the whole protocol, and replaced the panel with an ESP32 (tank levels,
> batteries, lights, pump, mains, cold-start wake). Full writeup, CC0:
> https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4/blob/main/docs/community/NE185_NE187_RS485_protocol_EN.md

---

## D) Gist

Subir el doc en inglés (`NE185_NE187_RS485_protocol_EN.md`) como **Gist público**
para tener un enlace corto y fácil de encontrar por buscadores. Título del gist:
`NordElettronica NE185 / NE187 RS-485 protocol (reverse-engineered)`.

---

## URLs (ya en el repo público)

- EN: https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4/blob/main/docs/community/NE185_NE187_RS485_protocol_EN.md
- ES: https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4/blob/main/docs/community/NE185_NE187_RS485_protocolo_ES.md

## Antes de publicar

- [x] URLs reales rellenadas en los textos de arriba.
- [x] Sin MACs/claves/datos personales en los docs comunitarios (revisado).
- [ ] (Opcional) Crear gist público con el doc EN y usar su enlace corto.
