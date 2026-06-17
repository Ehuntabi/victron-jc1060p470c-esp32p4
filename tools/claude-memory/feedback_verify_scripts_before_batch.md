---
name: feedback-verify-scripts-before-batch
description: SIEMPRE testar el script exacto con 1-2 inputs reales antes de lanzar batch grande. No basta con haber probado el comando suelto previamente
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

Antes de lanzar un batch sobre N archivos, ejecutar el script EXACTO (mismo binario, mismas variables, mismo input set) con N=1 o N=2 elementos representativos y CONFIRMAR el output esperado. No basta con haber probado el comando suelto antes — el script puede diferir en pequeneces (paths con espacios, extensiones, escaping, variables).

**Why:** El 2026-05-26 lance un batch de ffmpeg reencode HEVC sobre 190 videos. Antes habia probado el comando suelto encodeando 1 video a `/tmp/test.mp4` y funciono. Despues escribi un script donde el output tenia nombre `file.mp4.hevc.tmp` (extension `.tmp` en vez de `.mp4`), no lo probe. Lanzar batch -> 190 errores "Invalid argument" en muxer ffmpeg, 0 archivos procesados. Desperdicio de ~2 min de GPU y la confianza del usuario que dijo "no verificas lo que lanzas?".

**How to apply:**
- Antes de lanzar batch: ejecutar script con 1-2 archivos reales (mismo input que el batch)
- Verificar que produce el output esperado en disco/log
- Si todo OK, recien lanzar batch
- Aplicar especialmente cuando:
  - Hay paths con espacios, caracteres no-ASCII
  - Output con extensiones no estandar
  - Comandos externos (ffmpeg, exiftool, etc.) que validan extension del output
- Relacionado: [[feedback-destructive-commands-ask-first]] - tambien aplica a batch que MODIFICAN archivos in place
