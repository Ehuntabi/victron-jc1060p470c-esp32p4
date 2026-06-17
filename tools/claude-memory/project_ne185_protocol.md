---
name: project-ne185-protocol
description: "Protocolo NordElettronica NE185 RS-485 - ingenieria inversa completa (cmd format real, checksum, layout de respuesta). Sustituye la heuristica NE334 del repo class142 que NO aplicaba"
metadata: 
  node_type: memory
  type: project
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

Protocolo RS-485 38400 8N1 del NE185 (autocaravana del usuario). Reverse-engineered 2026-05-26 sobre 2056 frames reales capturados del NE187 panel original (logs en `~/Documentos/joint spl 154/logs_joint_ne185/log_2026*.txt`).

## Comandos (master -> NE185, 5 bytes)

`FF | 0x40 | bit_botones | 00 | 00 | checksum`

donde checksum = `(b[0]+b[1]+b[2]+b[3]) & 0xFF`:

```
CMD_IDLE     = FF 40 00 00 3F   (poll status, ningun boton pulsado)
CMD_BTN_LIN  = FF 41 00 00 40   (luz interior pulsada, bit 0)
CMD_BTN_LOUT = FF 42 00 00 41   (luz exterior pulsada, bit 1)
CMD_BTN_PUMP = FF 44 00 00 43   (bomba pulsada, bit 2)
```

NE187 envia los botones MIENTRAS estan pulsados (no como cmd unico).
NE185 procesa el toggle al **2do frame FF 4X consecutivo** a 60ms. Un solo frame se ignora.

**Why** el codigo anterior usaba FF 01/02/04 (heuristica del repo class142/ne-rs485 para NE334) que NO existe en NE185 -> bomba NUNCA funcionaba, luces toggle al azar. Lo descubrimos viendo SNIFF reales: NE187 NUNCA envia FF 0X, solo FF 4X.

**Confirmado via busqueda web 2026-05-26**: el repo class142/ne-rs485 documenta el panel **NE334** explicitamente (cmd FF 0X, polling ~5s, "timing no importa"). El panel del user es **NE187** (otro modelo) con protocolo DIFERENTE (cmd FF 4X overlay, polling 60ms, hold semantic obligatorio). No existe doc publica del NE187 - este reverse engineering es probablemente la primera.

## Respuesta NE185 -> master (20 bytes)

```
Pos  Bytes    Significado
0    FF       Header
1    eco cmd byte1 (40/41/42/44)
2    00       constante
3    00       constante
4    eco cmd checksum (3F/40/41/43)
5    nibble bajo = tank LIMPIO (0/1/3/7/F -> 0,1/4,2/4,3/4,4/4)
6    bit 1 = tank GRISES (1 = vacio per observacion user; 0 = lleno hipotesis pendiente)
7    00       constante
8    40       constante (algun glitch ocasional con 00)
9    variable (48 valores observados) - IGNORAR, no es info util pero entra en checksum
10   00       constante
11   FF       constante
12   battery1 servicio: V = (byte - 30) / 10
13   battery2 motor:    V = (byte - 30) / 10
14   variable (3 valores ED/EC/EE) - IGNORAR, no es info util pero entra en checksum
15   bitmap estados:
       bit 0 = luz interior ON
       bit 1 = luz exterior ON
       bit 2 = bomba ON
       bit 7 = shore (230V red conectada)
16   30       constante
17   00       constante
18   00       constante (posible reservado)
19   checksum = (b[5] + b[9] + b[14] + b[15] + 0xB1) & 0xFF
```

## Cadencia

NE187 original poleaba a **60ms (16 Hz)**. La cadencia anterior de 5000ms (5 seg) en el codigo del usuario daba UI lenta y nunca confirmaba toggles.

## Press hold semantica

Para que NE185 procese un toggle, el master debe enviar `FF 4X` durante >=2 frames consecutivos a 60ms. Recomendado **4 frames (240ms)** de margen.

Tras el hold, volver a `FF 40` durante al menos 2 frames (release) antes de aceptar otro press del mismo boton (evita doble toggle accidental).

## Variables UTILES del usuario (8 elementos)

1. Luz interior on/off (b15.0)
2. Luz exterior on/off (b15.1)
3. Bomba on/off (b15.2)
4. Aguas limpias 0..4/4 (b5 nibble bajo)
5. Aguas grises 0 vacio/1 lleno (b6.1 inverted)
6. 230V conectado on/off (b15.7)
7. Bateria habitaculo V (b12)
8. Bateria motor V (b13)

## How to apply

- Codigo de referencia: `/home/jc/victron/main/ne185/ne185.c` (refactor 2026-05-26)
- Si surge confusion sobre que cmd usar, mirar comentarios al principio del archivo
- Si aparecen bytes nuevos o tramas con valores fuera del rango esperado, activar `ne185_set_sniffer_tx(true)` para ver hex de cada RX
- Validaciones realizadas 2026-05-26 (user confirma estado real autocaravana):
  - Tank limpio en limite 1/4-2/4: byte 5 oscila entre 0x01 (34% frames) y 0x03 (66%) - VALIDA hipotesis con 2 niveles
  - Tank grises VACIO: byte 6 = 0x02 SIEMPRE (2056 frames) - VALIDA hipotesis "0x02 = vacio"
- TODO pendientes (a validar en autocaravana con condiciones distintas):
  - Tank grises lleno: confirmar que b[6] pierde bit 1 (hipotesis: 0x00 = lleno)
  - Tank limpio 3/4 (0x07) y 4/4 (0x0F) y reserva (0x00): aun no observados
  - Bateria habitaculo subiendo (cargador) o bajando: ver si b[12] cambia o sigue 0x9A
  - 230V shore: bit 7 de byte 15 nunca observado activo en los 2056 frames (siempre desenchufado en captura)
  - Bits 3-6 de byte 15: estados no observados (¿calefaccion? ¿gas? ¿LEDs ext?)
- Relacionado: [[feedback-toolchain-warnings-not-errors]], [[project-victron-esp-idf]]
