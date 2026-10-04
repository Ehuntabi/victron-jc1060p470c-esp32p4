v4.14 — la cámara atascada se recupera SOLA, por software (ya no hay que cortar la corriente)

## Qué cambia

- **Recuperación del sensor por I2C**, en tres pasos y con el bucle de captura
  **en pausa** para que el AE del ISP no escriba a la vez:
  1. parar el stream del sensor (`0x0100=0x00`);
  2. reset por software (`0x0103=0x01`) y reescritura de su **tabla de modo
     completa** por I2C, respetando `REG_END`/`REG_DELAY` como el driver;
  3. **reiniciar la placa** para rearmar el ISP del P4.
- Mientras la imagen esté corrupta **no se sirven fotos**: se acabó guardar en la
  galería ~350 KB de ruido.
- El paso 3 no es un adorno: el reset del sensor deja el **AWB del ISP del P4
  desbocado** (imagen magenta saturada) y **no vuelve sola**. Medido en **3 de 3**
  recuperaciones: R/G (×100) pasaba de 138-195 a 269-666, con hasta el 78% de los
  píxeles con R≥250. Tras reiniciar, el color vuelve al de siempre (comprobado).
- **Tope de 2 reinicios por enchufe** (contador en memoria RTC: sobrevive al
  reinicio de software —que es lo que hay que contar— y el corte de corriente lo
  borra). Si se agotan, deja de reiniciar y lo dice: nada de bucles.
- **Aviso de imagen roja** en el log y en el portal (`salud`), sin reiniciar por
  color: solo informa. Si la cámara lleva 3 recuperaciones sin obedecer, el
  mensaje sigue siendo **cortar la corriente** (entonces el sensor no acepta
  órdenes y no hay software que lo arregle).

## Verificado (con la placa, 4-oct-2026)

- Cadena completa en el log, forzando la corrupción a propósito:
  `imagen corrupta (grano 99)` → `recuperando el sensor (pauso captura...)` →
  `sensor reseteado y reconfigurado` → `REINICIO la placa ... intento 2/2` →
  arranque con `MOTIVO ULTIMO REINICIO: 3` (= `ESP_RST_SW`, reinicio por software,
  no un cuelgue) → **cámara sana y neutra** (R/G 1,22 en la foto siguiente).
- El contador RTC sobrevivió a dos reinicios de software (marcó `intento 2/2`), que
  es justo lo que tiene que hacer para no entrar en bucle.
- Reglas nuevas en `test/auditar.sh` (sección 11): orden de la recuperación, pausa
  del bucle, reinicio con tope y no servir fotos corruptas. **Probadas al revés**:
  rompiendo cada cosa a propósito, la auditoría falla.
- Trampas del banco, documentadas en `documentacion/CONTINUAR_AQUI.md`: abrir
  `/dev/ttyACM0` **reinicia la placa** (un lector que se reconecta la deja
  arrancando en bucle: 341 arranques en 4 minutos), y el JPEG del portal **estira
  el color** respecto a la miniatura en crudo (R/G 335 vs 201 en la misma escena),
  así que las medidas de una vía y de la otra no se pueden comparar.

v4.7 — el watchdog vigila también el enlace con la cabina (y dice qué tarea se colgó)

## Qué cambia

- **Dos tareas más en el watchdog**, las del enlace con la cabina:
  - `udp_tx` (telemetría P4 → cabina): si se cuelga, la cabina deja de recibir
    datos **en silencio**.
  - `udp_latido` (latido cabina → P4): si se cuelga, **la P4 cree que la cabina se
    ha muerto** — su latido deja de llegar — y empieza a reiniciar su propio AP en
    cascada. Un cuelgue se convertía en un problema de radio.
- **El reset controlado dice el nombre de la tarea**: antes
  `Tarea 7 sin latido — reset controlado`, ahora
  `Tarea 'udp_tx (telemetria)' sin latido — reset controlado`.
- Las dos laten en su bucle (y también mientras reintentan el socket, para que un
  AP raro no provoque un reset en bucle: el watchdog vigila que la tarea esté
  viva, no que el socket esté listo).

## Verificado

- **Sin falsos positivos** en 75 s de arranque, y con la cabina **ausente** (el
  caso peligroso: `udp_latido` late por su timeout de 2 s, no por los paquetes que
  recibe; si latiera con los paquetes, una cabina apagada provocaría un reset).
- **Caza de verdad**: quitando el latido de `udp_tx` salvo en las 3 primeras
  vueltas, a los ~35 s salió
  `Tarea 'udp_tx (telemetria)' sin latido — reset controlado`, la placa se
  reinició de forma controlada y el arranque siguiente lo explicó
  (`Reset FORZADO por watchdog SW: tarea sin latido`).
- Regla nueva en `test/auditar.sh` (2e) para que no se caigan de la tabla.

## Hueco cerrado el 3-oct-2026 (v4.10): aviso de las que nunca laten

Una tarea que **nunca** llega a latir no se vigilaba: el monitor ignora las entradas
con `s_last_beat == 0`, así que si `xTaskCreate` fallara (o la tarea muriera antes
del primer latido) su cuelgue no se detectaba — solo quedaba el error del arranque
en el log. **Ya no**: pasado **2× el plazo de ESA entrada** (nunca un plazo global:
`bh_flush` late cada 600 s y `udp_tx` cada 1 s) el monitor escribe un aviso por
cada entrada que no haya latido nunca. No se resetea: un reset ahí sería
injustificado, y por eso mismo el aviso es la respuesta correcta.

Probado quitando a propósito los latidos de `udp_latido`: sale su aviso con el
plazo que le toca (16 s) y **no** reinicia. La primera versión avisaba nada más
pasar la ventana de gracia de 30 s y saltaba con tareas sanas que aún no habían
llegado a su primer latido (`datalogger` 60 s, `viaje` 30 s, `bh_flush` 600 s):
se vio en el banco y se corrigió antes de publicar. Regla 2e de `test/auditar.sh`,
comprobada rompiéndola a propósito.
