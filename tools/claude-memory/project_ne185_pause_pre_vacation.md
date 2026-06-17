---
name: project-ne185-pause-pre-vacation
description: Pausa del proyecto NE185 durante 2-3 semanas (vacaciones autocaravana 2026-05-27). Resumen estado para retomar despues sin perder contexto
metadata: 
  node_type: memory
  type: project
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

**Pausa proyecto**: 2026-05-27. Usuario se va 2-3 semanas de vacaciones
con la autocaravana llevandose el panel NE187 original (luces/bomba
funcionaran normales en el viaje). Pantalla 7" Guition queda en casa
o desconectada del bus RS-485 J5.

## Estado del codigo en pause (commit ~7dc2380 + posteriores)

Repo: github.com/Ehuntabi/victron-jc1060p470c-esp32p4
Path: ~/joint/victron/

**Compilable y flasheado** con todos los fixes del 27-may:
- Init sequence DESACTIVADA (comentada). Era hipotesis del wake-up que
  rompia TX.
- Frame15 nativo NE185 aceptado por header `7C E0 00 40` o `FC E0 00 40`
  (sin validar checksum, formula real desconocida).
- Tank false positive arreglado: si frame15 nativo -> s1=r1=0xFF "sin dato"
  (los bytes b[0..1]=7C E0 son header, NO tanks).
- Timing TX: HOLD_FRAMES=8, POLL_PERIOD_MS=100, RELEASE_FRAMES=5.
- Verbose log ON por defecto.
- Boton LOG OFF/ON ahora se sincroniza con el estado real al init.
- SD save asincrono via task dedicada (evita Task Watchdog del LVGL).
- Mensaje SD friendly "Inserta la tarjeta SD" en lugar de "Error ESP_FAIL".
- Sniff mode + 5 botones cmd custom (FF 30/50/60/70/80) en Consola.
- Tank demo cycle DESACTIVADO (no mas falsos positivos visuales).
- Layout Consola compacto sin scroll.
- Markers 230V/Cargador para etiquetar log durante tests.
- BLE log [DIAG] mejorado: cualquier adv Victron, no solo MACs en NVS.

## Lo que SI sabemos del protocolo NE185 directo (sin NE187)

Frame de respuesta del NE185 a `FF 40 00 00 3F` (idle poll):
**15 bytes**, NO 20 como cuando NE187 intermedia.

Formato:
```
b[0..1] = 7C E0  (header, sometimes 7C E0 -> FC E0 transitorio raro 0.4%)
b[2..3] = 00 40  (constante, b[3] a veces 00)
b[4]    = counter o algo (35 valores observados, rango 0x40-0x80)
b[5]    = 00 (constante)
b[6]    = FF (constante)
b[7]    = 9A (constante, ¿bat fija 13.8V?)
b[8]    = A5-A7 (bat motor, V = (byte - 30)/10 = 11.7-12.0V)
b[9]    = EC-EE (sensor variable)
b[10]   = BITMAP DE LUCES/BOMBA - 6 valores observados:
  0x00 = todo OFF
  0x04 = bit 2 = bomba ON
  0x06 = bit 1+2 = luz_ext + bomba
  0x10 = bit 4 = ??? (siempre set tras tarde 27-may, hipotesis: motor ON o estado pegajoso)
  0x11 = bit 0+4 = luz_int + bit_4
  0x15 = bit 0+2+4 = luz_int + bomba + bit_4
b[11]   = 30 (constante)
b[12]   = 00 (constante)
b[13]   = 00 (constante)
b[14]   = CHECKSUM (formula real desconocida - no es b[4]|0xA0 como pensaba)
```

**Mapeo bitmap confirmado**: bit 0=luz_int, bit 1=luz_ext, bit 2=bomba.
**Bit 4 misterio**: aparece tras tarde 27-may sin que cambie nada
fisico (segun user). Hipotesis: arranque motor brevemente -> NE185 entro
en estado donde bit 4 persiste. Power cycle NE185 deberia resetear.

**Checksum**: NO formula simple. Probable depende de b[4]+b[8]+b[10].
Hace falta mas data para reverse-engineer.

## Pendientes para retomar

1. **Power cycle del NE185** antes del primer test post-vacaciones.
   El user dijo que tiene forma de cortar alimentacion al NE185.

2. **Test sin motor para confirmar TX limpio**: ¿enciende luces 1-a-1
   con pulse limpio cuando bit 4=0 (motor OFF, NE185 reset reciente)?

3. **Cmds custom FF 30/50/60/70/80**: probar con verbose ON desde el
   inicio. Si alguno trae frame de 20 bytes en vez de 15 -> encontramos
   el cmd que pide frame canonical con tanks.

4. **Reverse engineer del checksum b[14]**: pendiente con mas frames
   variados (motor ON/OFF, 230V ON/OFF, luces todas las combinaciones).

5. **TX errático**: razon desconocida. Hipotesis vivas:
   - bit 4 = lockout mode (descartar tras power cycle)
   - Interferencia eléctrica al arrancar motor
   - Timing del NE185 distinto sin NE187 (necesita PRESS_HOLD distinto)

## Hardware HW del bus RS-485 (descartado como problema)

User tiene placa bias montada con:
- R1=R2=680ohm pull-up/pull-down
- R3=132ohm (220||330) terminacion entre A/B

Doc: ~/joint/victron/docs/ne185_bias_board.pdf
Idle diff ~420mV, bias ~7.4mA. OK eléctricamente.

## Logs analizados hasta pausa

79 frame15 únicos extraidos de 5 logs del 27-may en
~/joint/victron/logs/. Analisis cross-log con scripts en /tmp/
(quizá borrados al apagar PC). Reconstruir con
`grep 'rx15:' ~/joint/victron/logs/*.txt`.

## Recordatorios al retomar

1. `git push` de los commits acumulados (estaban 14+ sin push)
2. Revocar token HA "claude-audit" si terminado
3. Estado HA + Proxmox al volver de vacaciones (Pi-hole, Immich, etc)
4. NE185 power cycle ANTES de primer test
5. Verbose log ya esta ON por defecto, no hace falta activar

## EUREKA 2026-05-27 23:00 — PROTOCOLO CORREGIDO (santo grial)

Tras busqueda profunda (2 agentes Web), descubri repos publicos con
codigo productivo NE185+NE319/334 que **refutan mis cmds sniffeados**:

- https://github.com/class142/ne-rs485 (NE334, Arduino 2023, Sebastian Seitz)
- https://github.com/thespinmaster/venus-os (NE319/334, Python Venus OS 2025)
- Validacion CRUZADA entre los 2 repos (coinciden exactamente)

**Cmds REALES de NE187/NE334 al NE185** (no los que yo creia):
```
IDLE:    FF 40 00 80 BF   (byte3=0x80, NO 0x00 como yo sniffee)
LIGHTIN: FF 01 00 C0 C0   (byte1=bit_boton, byte3=0xC0)
LIGHTOUT:FF 02 00 C0 C1
PUMP:    FF 04 00 C0 C3
AUX:     FF 08 00 C0 C7
ALL_OFF: FF 80 00 00 7F
```

**Por que mi sniffer me confundio**: el frame canonical de 20 bytes
del NE185 EMPIEZA con el ECHO del cmd recibido. Lo que vi como "cmd
del NE187 FF 40 00 00 3F" en mi sniff de 2056 frames eran los
primeros 5 bytes de respuestas del NE185 (echo) mezcladas en captura.

**Esto explica TODO**:
1. Por que el NE185 sin NE187 nos respondia frame15 degradado
   (7C E0 00 40...): rechazaba mi cmd invalido con b3=0x00 y emitia
   heartbeat de error.
2. Por que mi init sequence FF 40 00 80 BF al boot del 26-may
   "rompio" cosas: envie cmd valido 3 veces pero despues volvi al
   cmd invalido (legacy mal) -> NE185 entro en confusion.
3. Por que las luces a veces se encendian: cuando enviaba `FF 41 00 00 40`
   pulsando luz, casualmente FF 41 estaba reconocido y NE185 toggle.
   Pero el formato no estandar (b3=0x00) produce respuesta degradada.

**Cambios aplicados al codigo (commit 81b53c6, flasheado)**:
- CMD_IDLE = FF 40 00 80 BF (era FF 40 00 00 3F)
- CMD_BTN_LIN = FF 01 00 C0 C0 (era FF 41 00 00 40)
- CMD_BTN_LOUT = FF 02 00 C0 C1
- CMD_BTN_PUMP = FF 04 00 C0 C3

**Proximo viaje post-vacaciones**:
1. **Power cycle NE185** primero (corte alimentacion 1 min) - resetea
   cualquier estado pegajoso de tests previos.
2. Encender pantalla con los nuevos cmds flasheados.
3. Verificar en log si el rx ya es **20 bytes** (frame canonical) en
   vez de 15 bytes (frame degradado).
4. Si rx = 20 bytes: TANKS y BITMAP completos disponibles!
   - tanks: byte 11 (fresh) bits 0,1,2 y byte 13 (grey)
   - states byte 15: bit0=lin, bit1=lout, bit2=pump, bit3=aux
   - bat1 = byte 12 = (b-30)/10
   - bat2 = byte 13 = (b-30)/10
   - checksum: sum(b[0:18]) % 128 == (b[18]<<8 | b[19]) % 128 - 2
5. Test TX: pulsar luz int -> cmd FF 01 00 C0 C0 enviado -> NE185
   toggle.
6. **Polling rate**: class142 menciona "1 segundo" en spec, NO 60ms.
   Quiza nuestro POLL_PERIOD_MS=100 es OK, o quiza hay que subir a
   1000ms. A probar.

**Persona de contacto si falla todo**:
- smotek (https://github.com/smotek) tiene codigo NO publicado para
  NE333+N355, comento en class142/issues/1 (agosto 2023). Es la mejor
  shortcut para preguntar bytes exactos NE187-specific (que es
  Joint-exclusive y nadie ha hecho reverse publico).
