---
name: feedback-victron-ble-max-payload-size
description: "VICTRON_ENCRYPTED_DATA_MAX_SIZE en victron_ble.c debe ser >= 25, idealmente 32. Si esta a 21 los Orion DC/DC Tr Smart se rechazan silenciosamente"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

En `components/victron_ble/include/victron_records.h`, la macro `VICTRON_ENCRYPTED_DATA_MAX_SIZE` debe ser **>= 25 (ideal 32)**.

Antes estaba en 21 (cambio hecho para arreglar un buffer overflow legitimo donde memcpy escribia hasta `encr_size` bytes en un array de 21). Pero efecto colateral: los advertisements del **Orion DC/DC Tr Smart envian 22-25 bytes encriptados**, y el filtro `if (encr_size > 21)` los rechazaba con `ESP_LOGW "Invalid encrypted data size: 24"` y volvia 0. Sin warning visible para el usuario, la card DC/DC del overview quedaba siempre en `--`.

**Why:** El cambio del bound resolvio el overflow pero ROMPIO la deteccion de dispositivos con payload >= 22 bytes. El proyecto hermano (3.5") tenia bound 25 y mostraba el Orion sin problema. Diagnosis correcta requiere subir el buffer Y el bound a 32 (margen) en vez de bajar el bound.

**How to apply:**
- Si el user reporta que un dispositivo Victron NO aparece en el dashboard pero SI con VictronConnect movil, mirar PRIMERO el log con verbose ON: si sale `ESP_LOGW "Invalid encrypted data size: N"` con N > VICTRON_ENCRYPTED_DATA_MAX_SIZE -> subir el define
- Productos conocidos con payload grande:
  - **Orion DC/DC Tr Smart** (0x04): 22-25 bytes
  - **Orion XS** (0x0F): similar
  - Posibles: Multi RS, Inverter RS, Lynx Smart BMS
- 32 cubre todos los productos Victron actuales sin riesgo (struct mas grande es ~14 bytes plaintext + IV)
- Relacionado: [[project-victron-esp-idf]] - el componente victron_ble es duplicado entre proyectos victron (7") y victronsolardisplayesp-multi-device_pantalla_3.5 (3.5"). Si arreglas en uno copialo al otro.
