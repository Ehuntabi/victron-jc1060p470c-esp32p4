# Plan sesion SNIFFER NE185 — preparado 2026-06-21 para manana

Objetivo principal: **sniffear el boton CHECK del NE187** buscando el frame
con niveles de tanques de agua (prioridad #1 del AUTOCARAVANA-SETUP.md).

El firmware NO necesita cambios: el modo sniffer ya esta implementado.

## Estado dejado listo hoy

- [x] `idf.py build` OK (exit 0) sobre HEAD de `main` — flash sera inmediato manana.
- [x] Firmware incluye: modo sniffer (read-only), verbose log ON por defecto, Orion XS BLE.
- [x] Flasheado HEAD de main al P4 (2026-06-21) — verificado, hard reset OK.

## Sesion 2026-06-21 (autocaravana) — resultados

**Metodo nuevo que funciona**: Claude lee `/dev/ttyACM0` en vivo via pyserial
en background (NO `idf.py monitor`, que necesita TTY interactivo). Bucle cerrado:
usuario actua / Claude lee frames al instante. Ver memoria del workflow.

**BLE — todo OK**:
- BMV/SmartShunt `FF:3C:F3:77:D6:86` (0xA389) — OK. Con motor: Ibat +21.5A cargando, Aux 13.95V.
- SmartSolar MPPT 100/30 `C2:6D:F3:71:63:2F` (0xA056) — OK.
- Orion DC/DC — el usuario CONFIRMA que ahora SI ve sus valores en pantalla.
- `F6:40:14:6F:DF:4F` (0xA075 SmartSolar 75/15, rssi -92..-99) = VECINO, descartado OK.

**Incidencia USB (no es bug firmware)**: el P4 se reenumeraba en bucle
(`USB disconnect` device 8->14) PERO la pantalla seguia encendida (no reset del chip)
=> problema de CABLE/conector USB de datos, no alimentacion ni firmware.
Sintoma reportado: "a veces marca y otras no". Fix: cable USB-C de datos bueno,
reasentar conector, otro puerto USB.

**PENDIENTE para manana**: el sniffer NE185 (tanques agua via CHECK + luces ON/OFF)
NO se llego a probar. Empezar por el paso 4 de abajo.

## Pasos manana

1. Conectar P4 al portatil por **USB-C de datos**. Verificar puerto:
   ```bash
   ls /dev/ttyACM* /dev/ttyUSB*   # antes y despues de conectar
   ```
   El P4 suele ser `/dev/ttyACM0` o `/dev/ttyACM1`.

2. Cargar entorno y flashear el build ya hecho:
   ```bash
   cd ~/joint/victron
   . ~/.espressif/esp-idf-5.4/export.sh
   idf.py -p /dev/ttyACM0 flash monitor      # ajustar puerto si hace falta
   ```

3. Hardware del bus:
   - **NE187 conectado** al bus RS-485 junto con el P4 (el NE187 polea, el P4 escucha).
   - microSD insertada **antes** de arrancar (para Guardar SD).

4. En pantalla: **Settings -> Logs**
   - Pulsar boton **"MASTER MODE"** hasta que quede **naranja** = sniff/read-only
     (polling pausado, el P4 no emite nada al bus).
   - **LOG ON** ya viene activo (verbose).

5. En el monitor serie confirmar lineas `sniff rx15:` / `sniff rx20:`.

6. **Pulsar CHECK en el NE187** y observar diferencias en frame15/frame20
   antes y despues. Hipotesis: CHECK dispara frame20 canonico con tanques de agua.

7. **Guardar SD** — pulsar **UNA sola vez** y esperar (2 clicks seguidos daba
   pantallazo azul; fix 600227f aplicado pero confirmar en practica).

## Objetivos secundarios si hay tiempo

- **Luces ON/OFF**: capturar tramas al encender/apagar para diferenciar
  set ON vs set OFF (probar `FF 01 00 80 80` como set OFF en `ne185.c`).
- **Orion DC/DC BLE**: verificar por que el Orion Tr no llega al P4
  (VE.Smart networking OFF en app Victron, FW >= v3.61, motor arrancado = mas adv).

## Referencias

- `AUTOCARAVANA-SETUP.md` seccion 3.1 — estado y prioridades
- `main/ne185/ne185.c` — sniff mode lineas ~384, toggle `ne185_set_polling_paused`
- `main/ui/settings_logs_panel.c` — botones MASTER MODE / LOG / Guardar SD
- Memoria `tools/claude-memory/project_ne185_implementation_status.md` — 8 hipotesis
