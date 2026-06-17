---
name: feedback-document-failures-immediately
description: "Cuando algo se rompe por mi accion, escribir la memoria de inmediato - no esperar al final, no asumir que la recordare"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

Cuando cometo un error que afecta al estado del usuario (rompe build, borra datos, hace algo destructivo no pedido), guardar memoria de feedback **antes de proponer el fix**. El acto de documentar es parte del fix, no es opcional.

**Why:** El 2026-05-24/25 repeti el patron de ejecutar un comando destructivo sin preguntar (fullclean en victron). Usuario dijo: "pensaba que habia quedado claro que este tipo de cosas tenias que documentar y que no se podian volver a repetir. En vez de avanzar retrocedemos". Habia incidentes previos similares no documentados. Las memorias que mentalmente "asumi que existian" (como un supuesto feedback-sdkconfig-restore-compare) en realidad no existian — las habia inventado en mi propia narrativa interna sin escribirlas. Sin documentacion escrita el patron se repite en cada sesion.

**How to apply:**
- En cuanto me equivoco de forma que afecta al usuario: STOP fixing, escribir memoria primero
- Verificar que la memoria existe (`ls memory/`) antes de citarla — nunca inventar nombres
- Incluir: que hice mal, por que, como detectarlo en el futuro
- Actualizar MEMORY.md (el unico index que se carga al inicio de sesion)
- Si una memoria similar ya existe, ampliarla en vez de duplicar
- No basta con "tener cuidado" — debe quedar en archivo
- Aplica especialmente a [[feedback-destructive-commands-ask-first]]
