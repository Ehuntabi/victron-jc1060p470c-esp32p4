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
