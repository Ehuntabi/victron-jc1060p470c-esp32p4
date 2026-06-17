---
name: feedback-destructive-commands-ask-first
description: "NUNCA ejecutar comandos destructivos (fullclean, rm -rf build, reset --hard, etc.) sin permiso explicito - aunque parezca solucion obvia a un warning"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

NO ejecutar comandos que destruyan estado de trabajo (cache de build, managed_components, sdkconfig, working tree) sin pedir permiso explicito al usuario. Aunque parezca la solucion obvia para un warning o error, primero preguntar.

**Why:** El 2026-05-24 ejecute `idf.py fullclean` en ~/victron para "arreglar" un warning de toolchain version mismatch (`Expected esp-14.2.0_20241119, found esp-14.2.0_20260121`). El warning era inofensivo y el build funcionaba. El fullclean nuked `managed_components/` y regenero `sdkconfig` con valores distintos (SPIRAM_SPEED 200M -> 20M, LDO 1800 -> 1900 mV, etc.), causando DPI underrun continuo en MIPI-DSI y dejando la pantalla azul. Usuario justificadamente decepcionado: "Ya funcionaba todo... En vez de avanzar retrocedemos". Esto era un patron repetido que segun el usuario ya debiamos haber documentado.

**How to apply:**
- Antes de `idf.py fullclean`, `rm -rf build/`, `git reset --hard`, `rm -rf node_modules/`, `git clean`, etc.: **PARA y pregunta con AskUserQuestion**
- Warnings != errors: un build con warnings que produce binario valido es un build OK
- "Surgical Changes" (regla global Karpathy): tocar solo lo minimo necesario
- Si el problema es un warning, investigarlo sin destruir estado funcional
- Si imprescindible regenerar: hacer copia previa (`cp -r build build.bak`, `cp sdkconfig sdkconfig.bak`)
- Aplica especialmente a [[feedback-sdkconfig-restore-compare]] y proyectos ESP-IDF

**Senales de que estoy a punto de violar esto:**
- "Esto deberia arreglarlo con un clean"
- "Voy a regenerar X para asegurarme"
- "Lo mas rapido es borrar y volver a empezar"
- Cualquier comando con `-f`, `--force`, `clean`, `reset` que afecte estado actual
