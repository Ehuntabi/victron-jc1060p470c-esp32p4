# `forzar`: cómo autenticar a la pantalla (diseño, 20-oct-2026)

Estado: **diseño aprobado pendiente de implementar**. Hoy (v4.56) el `forzar` del
`fin`/`descartar` acepta el nivel abierto si el cuerpo trae un `dev` con MAC
valida. Eso para un dedazo (escribir 12 hex), pero no autentica: cualquiera en el
AP con la clave del portal —que está en el historial de dos repos publicos— puede
mandarlo, y un portatil puede hasta ponerse la MAC de una cabina.

## Que hay que proteger

`forzar:true` cierra o aparta el viaje que QUIZA esta grabando otra pantalla: le
escribe el resumen, lo renombra y el dueño pierde el viaje en curso. Es la unica
operacion "destructiva" que no pide la clave de administracion (y no puede
pedirla: la cabina no la tiene). La salida de emergencia tiene que seguir
existiendo — un viaje cuyo dueño ya no existe no se podria cerrar de otra forma.

## Decision: token por pantalla (bearer), no HMAC

Cada pantalla tiene un **secreto de 16 bytes** propio, guardado en su NVS y en la
P4 **por MAC**. El `fin`/`descartar` con `forzar` lo manda como `"token"` (32 hex).
La P4 lo compara en tiempo constante con el de esa MAC.

Por que bearer y no HMAC: el canal ya es WPA2 entre la cabina y la P4 (no hay
tercero que escuche sin la clave del AP), el cuerpo no se reutiliza (cada `fin`
lleva su `id` y se deduplica por el) y un HMAC obligaria a duplicar la logica de
firma en dos firmwares y en los tests. Lo que aporta el HMAC aqui —que el token
no viaje— no vale esa duplicacion en este despliegue.

## Emparejamiento (una vez por pantalla)

1. La cabina, al arrancar o al primer `inicio`, manda `inicio` como siempre (con
   su `dev`).
2. En la respuesta del `inicio` la P4 añade `"token":"<32 hex>"` **solo si esa MAC
   no tenia secreto** (primer contacto) o si el que trae no cuadra (NVS borrada en
   la P4: se re-emite). La cabina lo guarda en NVS (`save_pantalla_token`).
3. A partir de ahi lo manda en cada `forzar`.

Limite honesto de este paso: en el **primer** contacto, quien pueda mandar un
`inicio` con la MAC de una cabina se lleva el token de esa MAC. Se acepta porque
(a) para entonces el atacante ya tiene la clave del portal y podria empezar/cerrar
viajes a su gusto por el camino abierto, y (b) cerrar esa ventana del todo pide
emparejar por cable/SD, que no compensa aqui. Lo que SI cierra: una vez emparejada
una pantalla, un portatil ya no puede hacerse pasar por ella.

## Contrato (JSON)

```
{"op":"fin","forzar":true,"id":N,"dev":"<mac>","token":"<32 hex>","carpeta":"..."}
{"op":"descartar","forzar":true,"id":N,"dev":"<mac>","token":"<32 hex>"}
```
`token` es opcional: sin el, `forzar` se comporta como hoy. La P4 lo mira SOLO
cuando decide si `forzar` necesita la clave estricta.

## Cambios, fichero a fichero

**P4 (`victron`)**
- `portal/config_server_viaje.c`
  - `static bool token_de_pantalla_ok(const char *dev, const char *token);`
    (lee `NVS_NS` clave `"tok_<mac>"`, compara con `auth_equal_ct`, que ya existe
    en `config_server_auth.c` — hay que exportarla en `config_server_internal.h`).
  - `static void token_de_pantalla_emite(const char *dev, char *out, size_t n);`
    (genera con `esp_random()`, guarda en NVS, NVS_READWRITE + commit).
  - En `op_inicio`: si el cuerpo trae `dev` y no hay secreto para esa MAC, emitir
    y añadir al JSON de respuesta. (El inicio ya contesta con `carpeta` y
    `libre_mb`; es un campo mas.)
  - En el bloque de `forzar`: `bool es_una_pantalla = dev_es_mac(dev) &&
    token_de_pantalla_ok(dev, token_del_cuerpo);` y **el fallback es pedir la
    clave estricta**, nunca denegar (la leccion de la v4.55).
- `main/portal/config_server_internal.h`: exportar `auth_equal_ct`.

**Cabinas (`cabina5` y `35cabina`, espejo)**
- `components/config_storage`: `bool load_pantalla_token(char *out, size_t n)` /
  `esp_err_t save_pantalla_token(const char *tok)` (NVS, 32 hex + NUL).
- `main/net/p4_api.c`: `p4_api_cuerpo_fin_ajeno(..., int forzar)` y
  `p4_api_cuerpo_descartar(..., int forzar)` añaden `,"token":"<tok>"` si hay
  token guardado (leerlo en el llamante, no dentro de `p4_api`, que es puro).
- `main/ui/view_registro.c`: al recibir la respuesta del `inicio`, si trae
  `token`, guardarlo. Es el unico sitio que parsea esa respuesta
  (`inicio_resultado_cb`).
- `components/config_storage` (test): caso de ida y vuelta del token.

## Migracion y compatibilidad

- Pantalla con firmware viejo (sin token): el `forzar` pide admin → **es lo que
  pasa hoy**; no se rompe nada nuevo, y la P4 re-emite el token en el siguiente
  `inicio` cuando la pantalla se actualice.
- P4 vieja + cabina nueva: el `token` es un campo mas del JSON; la P4 vieja lo
  ignora (cJSON no se queja) y sigue aceptando el `dev`. Compatible.
- NVS borrada en la cabina: pide token otra vez en el proximo `inicio`.
- NVS borrada en la P4: emite token nuevo en el proximo `inicio`.

## Pruebas

Host (sin placa):
- `victron/test/contrato_app.sh`: el `forzar` sin token de una MAC desconocida →
  camino estricto; con token correcto → camino abierto; con token de OTRA MAC →
  estricto; sin `dev` → estricto.
- `victron/test/entrada_hostil.sh`: token de 33 caracteres, token con `%s`,
  `forzar` con `dev` de 12 hex inventado y token aleatorio → estricto.
- `cabina5|35cabina/test/viaje_contadores.sh`: el cuerpo del `fin` ajeno lleva
  `token` cuando el `dev` esta emparejado y no lo lleva cuando no.

Placa (dos pantallas, lo unico que de verdad cierra el circulo):
1. Viaje abierto en la 5". En la 3,5" → "VIAJE EN LA P4" → GUARDARLO: debe cerrar
   **sin pedir clave** (token valido).
2. `curl` con la clave del portal, `dev` de la 5" y token inventado → **403**.
3. Borrar el token de la 3,5" (Ajustes→ datos, o reflash) y repetir el 1: en el
   siguiente `inicio` la P4 le da token nuevo y vuelve a funcionar.
4. La 5" con firmware viejo (v1.41): `forzar` → pide admin (comportamiento actual),
   y el viaje se puede cerrar igual con la clave.

## Por que esto y no las otras dos

- **MAC en el latido (proto v6)**: no autentica — falsificar la MAC de una cabina
  por Wi-Fi es un campo del driver — y cuesta un cambio de protocolo en tres
  placas. Mucho ruido para poca seguridad.
- **Solo concesiones DHCP**: cierra el dedazo pero no a un portatil (tiene su
  propia concesion) y añade un modo de fallo (tabla vacia) que puede tumbar la
  salida de emergencia, que es justo lo que paso con la v4.55.

El token es el unico que convierte "creo que es una pantalla" en "es ESTA
pantalla", sin tocar el protocolo UDP y con la salida de emergencia siempre viva.
