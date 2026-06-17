---
name: project_stability_review_2026-06
description: Revision de estabilidad 2026-06-17 - pendientes con causa raiz y hallazgos verificados-OK
metadata: 
  node_type: memory
  type: project
  originSessionId: 5c9e46a5-ed43-43c5-b968-9a33b9e8eb45
---

Revision de estabilidad del firmware Victron hecha el 2026-06-17 (5 agentes por
subsistema + verificacion manual). Lo ARREGLADO esta en git con detalle
(commits 7dc0af4 concurrencia/locking, 6bd90b4 watchdog, e761482 over-read NVS,
a107fe4 RTC, 29295d3 pulido). Lo que importa recordar:

**RESUELTO #7 - salvapantallas "Rotar" (commit 3b1b336, era pendiente #1 del CLAUDE.md):**
Estaba desactivado a proposito por supuesta fragmentacion del pool LVGL de 128 KB.
Fase 0 (instrumentacion de heap temporal) demostro que ese blocker YA NO APLICA:
con LV_MEM_CUSTOM=y LVGL asigna del heap del sistema (30 MB, mayor bloque 29.9 MB)
-> 15 ciclos crear/destruir de ambos overlays = heap estable, sin leak ni
fragmentacion. El comentario del autor era del modelo de memoria antiguo (pool
propio). El fix fue de 15 lineas: screensaver_timer_cb crea el rotate_timer
(rotate_period_min) en vez de caer a Dim; el teardown en screensaver_wake ya existia.
Verificado a 4s (12 rotaciones 0->1->2 limpias). PENDIENTE leve: probar en uso real
a periodo de minutos con un dia entero de CSV (mecanismo probado, falta corrida larga).
LECCION: medir antes de refactorizar; ahorro reescribir 450 lineas de overlays.

**PENDIENTE #10 - DS18B20 trigger sin chequeo:**
frigo.c (~177) no comprueba el retorno de ds18b20_trigger_temperature_conversion.
Solo es riesgo cuando haya sensor cableado y se desconecte en caliente. Con
n_sensors==0 (hoy, sin cablear) el camino es seguro. Abordar al cablear el sensor.
Ver [[project_ne185_implementation_status]] para mas hardware pendiente.

**VERIFICADO-OK (no re-revisar):**
- Parsing serie/BLE: SIN desbordamientos explotables. PZEM valida CRC16+cabecera,
  NE185 valida checksum, fix AES Victron (MAX_PAYLOAD 21->32) aplicado y correcto.
- Sin fugas: ningun fopen sin fclose, malloc sin free, ni handle NVS filtrado.
- Camino "sin DS18B20/ventilador cableado" no cuelga.

**Nuevo modelo de watchdog (commit 6bd90b4):** ademas del wd_monitor_task que
vigila LVGL, las tareas ne185/pzem/frigo laten via watchdog_heartbeat(); frigo usa
frigo_set_heartbeat_cb (alimentado por frigo_task, NO por el simulador). Salvaguardas
anti-reboot-loop: grace 30s en boot, solo se vigila una tarea tras su primer latido,
umbral 10s. Si se anaden tareas criticas nuevas, registrarlas igual.
