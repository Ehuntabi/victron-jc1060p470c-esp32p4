---
name: reference_victron_ble_protocol
description: Referencia canonica para verificar el decode Victron BLE + hallazgo Orion XS
metadata: 
  node_type: memory
  type: reference
  originSessionId: 5c9e46a5-ed43-43c5-b968-9a33b9e8eb45
---

Decode Victron BLE "Instant Readout" del proyecto: `components/victron_ble/victron_ble.c`
(+ structs en `include/victron_records.h`).

**Fuente canonica para verificar el protocolo** (la usan ESPHome y Home Assistant):
https://github.com/keshavdv/victron-ble  (devices/*.py = layouts; base.py = header+AES).
Verificado 2026-06-17 contra esta fuente; el nucleo coincide:
- Manufacturer ID 0x02E1. Cabecera: prefix 0x10, product_id(LE), record type, nonce(2B LE),
  byte key-match, datos cifrados.
- AES-128-CTR: contador de 16 bits = nonce como valor inicial little-endian en bloque de 16
  bytes con relleno cero. Check encryptKeyMatch == key[0].
- Battery Monitor: TTG[0:16], V[16:16 signed], alarm[32:16], aux[48:16], aux_mode[64:2],
  corriente[66:22 signed /1000], consumido[88:20], SoC[108:10 /10].
- Solar: state,err, V[0.01], I[0.1], yield[10Wh], pv[1W], load[9b 0.1A].
- Orion XS (0x0F): state,err, Vout[0.01], Iout[0.1], Vin[0.01], Iin[0.1], off_reason(32) = 14B.

**HALLAZGO (corregido en commit d288815):** el switch de decode solo cubria 5 de 14 tipos.
El Orion XS (0x0F) -el DC/DC real del usuario- NO se decodificaba -> caia en "Unsupported"
y la card DC/DC quedaba en "--", aunque overview y dashboard_state YA tenian su case
esperando. Tambien faltaba aplicar centinelas NA (0x7FFF/0xFFFF/0x3FF): se mostraban como
basura (655V/327V/102.3%); ahora se normalizan a 0 (UI muestra "--"); SoC NA lo gestiona el
UI con >1000.

PENDIENTE: 9 tipos aun sin decodificar (Lynx BMS 0x0A, AC Charger, Smart Battery Protect,
Multi RS, VE.Bus, DC Energy Meter) - los structs existen, solo si el usuario tiene alguno.
No se pudo probar en vivo (sin dispositivos Victron en rango en el banco).
Relacionado: [[feedback_victron_ble_max_payload_size]], [[project_stability_review_2026-06]].
