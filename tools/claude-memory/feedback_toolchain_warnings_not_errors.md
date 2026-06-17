---
name: feedback-toolchain-warnings-not-errors
description: "Los warnings de toolchain mismatch en ESP-IDF (Expected X found Y) son inofensivos si el build produce binario - NO ejecutar fullclean para \"arreglarlos\""
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

Cuando ESP-IDF imprime `Tool doesn't match supported version from list ['esp-X.Y.Z_DDDD']: ... esp-X.Y.Z_EEEE` y el build continua y produce binario valido: **es un warning, no un error**. El binario funciona. No actuar.

**Why:** El 2026-05-25 vi exactamente este warning en victron tras un cambio en ne185.c. Por reflejo ejecute `idf.py fullclean` para "limpiar". Eso nuked managed_components/ + sdkconfig (regenerado con la IDF 5.4.1 disponible en vez de 5.4.4 original), destrozando capabilities criticas como `CONFIG_SOC_AHB_GDMA_SUPPORT_PSRAM`. Resultado: pantalla azul con DPI underrun continuo. El warning original era inofensivo - el build estaba produciendo binario igualmente.

**How to apply:**
- Si build dice "Project build complete" o emite `.bin`: el warning es ignorable
- Solo investigar el warning si el build FALLA con error claro
- Para el warning especifico de toolchain mismatch:
  - Si vienen de actualizar IDF -> investigar version IDF instalada vs version que genero sdkconfig (head -3 sdkconfig)
  - Si las versiones IDF coinciden y solo difiere fecha del toolchain (esp-14.2.0_DDDD): casi siempre inofensivo
- NUNCA `fullclean` como "primera medida" frente a un warning. Solo cuando hay un error de build real, y siempre con [[feedback-destructive-commands-ask-first]]
