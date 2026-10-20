# `forzar`: cómo autenticar a la pantalla (diseño, 20-oct-2026)

Estado: **la mitad de la P4 hecha y grabada en la v4.57 (tanda 1, 20-oct-2026);
falta la mitad de las cabinas (tanda 2)**. Lo implementado, y en qué se aparta
del diseño, al final: «Cómo ha quedado en la v4.57».

Hasta la v4.56 el `forzar` del `fin`/`descartar` aceptaba el nivel abierto si el
cuerpo traia un `dev` con MAC valida. Eso para un dedazo (escribir 12 hex), pero
no autentica: cualquiera en el AP con la clave del portal —que está en el
historial de dos repos publicos— puede mandarlo, y un portatil puede hasta
ponerse la MAC de una cabina.

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
   su `dev`) y con un campo `"token"`: el que tenga guardado, o `""` si no tiene.
   **Ese campo es la señal de que la pantalla habla este protocolo**: una cabina
   con el firmware viejo no lo manda y la P4 no la empareja (ver «Migracion»).
2. En la respuesta del `inicio` la P4 añade ` token=<32 hex>` al final **solo si
   esa MAC no tenia secreto** (primer contacto, o NVS borrada en la P4) o si el
   que trae no cuadra (entonces le reenvia el guardado). La cabina lo guarda en
   NVS (`save_pantalla_token`).
3. A partir de ahi lo manda en cada `forzar`.

Limite honesto de este paso: quien pueda mandar un `inicio` con la MAC de una
cabina y `"token":""` se lleva el token de esa MAC (y si ya estaba emparejada, se
lo vuelven a dar). Se acepta porque (a) para entonces el atacante ya tiene la
clave del portal y podria empezar/cerrar viajes a su gusto por el camino abierto,
y (b) cerrar esa ventana del todo pide emparejar por cable/SD, que no compensa
aqui. Lo que SI cierra: un navegador, la app o un POST a mano que no hable el
emparejamiento ya no puede cerrar el viaje de la otra pantalla sin la clave de
administracion; y que la cabina no tenga que llevar encima esa clave.

## Contrato (JSON)

```
{"op":"fin","forzar":true,"id":N,"dev":"<mac>","token":"<32 hex>","carpeta":"..."}
{"op":"descartar","forzar":true,"id":N,"dev":"<mac>","token":"<32 hex>"}
```
`token` es opcional: sin el, `forzar` se comporta como hoy. La P4 lo mira SOLO
cuando decide si `forzar` necesita la clave estricta.

## Cambios, fichero a fichero

**P4 (`victron`): HECHO en la v4.57**

- `portal/config_server_viaje.c`
  - `token_normaliza()`: 32 hex, a minusculas. Cualquier otra cosa (33 caracteres,
    `%s`, 31, una `g`) es «sin token».
  - `token_de_pantalla_clave()`: clave de NVS `tk_<8 ultimos hex de la MAC>`. Las
    claves de NVS no pasan de 15 caracteres y `tok_` + los 12 de la MAC son 16.
  - `token_de_pantalla_lee()` / `token_de_pantalla_emite()`: leen o crean el
    secreto de esa MAC en `NVS_NS` (`nvs_commit` comprobado, como el resto).
  - `token_autoriza_forzar()`: la decision, **pura** (sin NVS ni httpd), probada
    en el host. Sin secreto para esa MAC vale el `dev` (v4.56); con secreto, solo
    el token de ESA MAC.
  - `token_de_pantalla_emparejar()`: en `op_inicio`, si el cuerpo trae `"token"`,
    emite o reenvia el secreto. Va al final del cuerpo de la respuesta
    (`... libre_mb=NNN token=<32 hex>`), **detras de `libre_mb`**: una cabina con
    el firmware viejo copia solo los primeros 96 bytes y busca ahi ese campo.
  - En el bloque de `forzar` del handler: `dev` **y** token, y el fallback es
    pedir la clave estricta, nunca denegar (la leccion de la v4.55).
- `main/portal/config_server_internal.h`: `auth_equal_ct` exportada (ya existia en
  `config_server_auth.c`, era `static`).

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

- Pantalla con firmware viejo (sin `token` en el `inicio`): la P4 **no la
  empareja**, asi que su `forzar` sigue valiendo con el `dev`, igual que en la
  v4.56. Es lo que evita que actualizar la P4 deje a las dos cabinas sin la unica
  salida de emergencia que tienen. Cuando se actualice la cabina, manda
  `"token":""` y se empareja sola en ese mismo `inicio`.
- P4 vieja + cabina nueva: el `token` es un campo mas del JSON y un sufijo mas del
  texto de la respuesta; la P4 vieja los ignora y sigue aceptando el `dev`.
- NVS borrada en la cabina: manda `""` y la P4 le reenvia el mismo secreto.
- NVS borrada en la P4: no hay secreto → emite uno nuevo en el proximo `inicio`.

## Pruebas

Host (sin placa, en `test/run.sh` y en el CI):
- `victron/test/test_viaje_token.sh`: 35 casos, con las funciones REALES extraidas
  del fuente (no una copia). Forma del token (32 hex, mayusculas, 31, 33, `%s`,
  vacio, NULL), clave de NVS por MAC (distinta por pantalla y <= 15 caracteres) y
  la tabla del `forzar`: token correcto / de otra MAC / sin token / inventado /
  pantalla sin emparejar / `dev` que no es una MAC.

Placa:
1. P4 v4.57 con las cabinas de antes (SIN token): `curl` con la clave del portal,
   un `dev` valido y SIN token → el `forzar` pasa (nivel abierto, como la
   v4.56). Es la prueba de que no se ha roto la salida de emergencia.
2. `inicio` con `"token":""` y esa MAC → la respuesta trae ` token=<32 hex>` y el
   log dice «pantalla ... emparejada».
3. El mismo `forzar` de (1), sin token → ahora **403** (pide la clave estricta).
4. Ese `forzar` con el token de (2) → pasa. Con el token de otra MAC → 403.

Y con las cabinas de la tanda 2 (lo que cierra el circulo):
5. Viaje abierto en la 5". En la 3,5" → "VIAJE EN LA P4" → GUARDARLO: cierra
   **sin pedir clave**.
6. Borrar el token de la 3,5" (Ajustes → datos, o reflash) y repetir el 5: en el
   siguiente `inicio` la P4 le da el token y vuelve a funcionar.

## Por que esto y no las otras dos

- **MAC en el latido (proto v6)**: no autentica — falsificar la MAC de una cabina
  por Wi-Fi es un campo del driver — y cuesta un cambio de protocolo en tres
  placas. Mucho ruido para poca seguridad.
- **Solo concesiones DHCP**: cierra el dedazo pero no a un portatil (tiene su
  propia concesion) y añade un modo de fallo (tabla vacia) que puede tumbar la
  salida de emergencia, que es justo lo que paso con la v4.55.

El token es el unico que convierte "creo que es una pantalla" en "es ESTA
pantalla", sin tocar el protocolo UDP y con la salida de emergencia siempre viva.

## Cómo ha quedado en la v4.57 (tanda 1, 20-oct-2026)

Implementado en la P4 y grabado en el banco. Tres desviaciones del diseño de
arriba, todas para que lo que hay grabado siga funcionando:

1. **El emparejamiento es opt-in**: la P4 solo crea secreto si el `inicio` trae el
   campo `"token"`. El diseño no lo decia y habria roto la salida de emergencia:
   al primer `inicio` de una cabina vieja la P4 le habria creado secreto, y su
   `forzar` (que no puede mandar token) habria pasado a pedir la clave — 403, el
   usuario sin salida, la regresion de la v4.55 otra vez.
2. **Sin secreto para esa MAC, el `forzar` con `dev` vale** (comportamiento de la
   v4.56). El diseño decia «siempre token»; eso solo es cierto a partir de que la
   pantalla se empareja.
3. **La clave de NVS es `tk_<8 hex>`**, no `tok_<mac>`: no cabe.

Lo que queda para la tanda 2: `load_pantalla_token`/`save_pantalla_token` en
`components/config_storage` de las dos cabinas, mandar el token en
`p4_api_cuerpo_fin_ajeno()`/`p4_api_cuerpo_descartar()`, guardarlo al leer la
respuesta del `inicio`, y la prueba con dos pantallas.
