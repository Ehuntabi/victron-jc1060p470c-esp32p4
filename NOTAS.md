v4.16 — la recuperación de la cámara deja de reiniciar la placa (se rearma el ISP en caliente)

## Qué cambia

- Tras recuperar el sensor por I2C, el ISP se rearma **EN CALIENTE** con
  `esp_video_isp_pipeline_set_ipa_config()` sobre la configuración del sensor
  (`esp_ipa_pipeline_get_config("OV02C10")`). Esa API **crea un pipeline IPA nuevo
  y lo intercambia con el vivo**, así que el AWB/AE arrancan de cero y se
  reaplican los parámetros iniciales al ISP: exactamente lo que hacía el
  reinicio, pero en el sitio.
- El **reinicio queda de respaldo**: si el rearme falla, o si la imagen sigue
  roja tras recuperar (detector de color, ventana de 10 min), se reinicia la
  placa como hasta ahora, con el tope de 2 por enchufe.

## Verificado (con la placa, 4-oct-2026)

- Forzando la corrupción a propósito, cadena completa en el log, **sin reinicio**:
  `imagen corrupta (grano 99)` → `recuperando el sensor (pauso captura...)` →
  `sensor reseteado y reconfigurado` → `IPA rearmado EN CALIENTE (0), sin
  reiniciar la placa` (11 ms después) → `imagen normal otra vez (grano 0)`.
- Medido con el **techo blanco** como referencia (que es lo que delata el
  magenta): antes de la corrupción R/G 0,97; después de recuperar **1,03-1,04**,
  estable durante los 2,5 min de sondeo. Cero reinicios (`MOTIVO ULTIMO REINICIO`
  siguió siendo el del cable USB, no `ESP_RST_SW`).
- Frente al reinicio: **~85 s sin AP y sin vigilancia → 11 ms**. La vigilancia no
  se corta y no hay que rearmarla.
- Reglas nuevas en `test/auditar.sh`, **probadas al revés** (borrando la llamada y
  el tope, la auditoría falla). Ojo al detalle: la primera versión de la regla
  buscaba el nombre de la función y pasaba con la llamada borrada (el nombre
  aparecía en un comentario); ahora busca la llamada con su argumento.

v4.15 — la cámara de noche: se va el magenta y la cara sale de la sombra

## Qué cambia

- **AWB: la ventana de puntos blancos vuelve a estar CENTRADA en el neutro real
  del sensor** (rg 0,53 / bg 0,64, calibrado con el techo blanco de referencia).
  Queda en rg 0,35-0,80 y bg 0,40-0,90. La v4.8 la había ensanchado a 0,2-1,6
  para arreglar el rojo de día, y eso **devolvió el magenta de noche**.
- **AE: modo `low_light_priority`** (protege las sombras) en lugar de
  `high_light_priority`, que daba 5× de peso a las zonas brillantes.
- Lo que arreglaba de verdad el rojo de día era el **objetivo del AE** (55 → 100,
  de la v4.8), no ensanchar la ventana: con la ventana centrada y el objetivo en
  100 no hay rojo saturado (0,0% en la lámpara cálida).

## Verificado (mismo salón y misma luz, 4-oct-2026)

| medida | antes (v4.14) | después |
|---|---|---|
| techo blanco, R/G | 1,99 (magenta) | **0,98-1,03** |
| imagen completa, R/G | 4,10 | **1,38-1,58** |
| cara (luma Y) | 46 | **93** |
| lámpara (luma Y) | 215 (quemando) | 159 |
| píxeles quemados con luz fuerte | (85% en la v4.7) | 2,9-4,0% (la propia luz) |

- Comprobado en **dos luces**: lámpara cálida (el caso que salía magenta) y luz
  blanca fuerte (para descartar el fallo contrario, pasarse de exposición).
- Reglas nuevas en `test/auditar.sh`: ventana del AWB centrada en el neutro real
  y AE de sombras. **Probadas al revés**: rompiendo cada ajuste, la auditoría falla.
- Lo que **no** está medido: luz de día natural (a las 17:32 la misma cámara daba
  R/G 1,46 y la cara bien, pero con la configuración anterior; conviene repetir la
  foto a plena luz para confirmar).

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
