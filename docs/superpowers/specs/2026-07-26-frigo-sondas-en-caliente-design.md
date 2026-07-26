# Sondas del frigo en caliente — diseño

Fecha: 2026-07-26 · Rama: `feat/frigo-sondas-en-caliente` (sobre `fix/auditoria-2026-07-26`)

## El problema

Las sondas DS18B20 solo se buscan **una vez, al arrancar** (`frigo_init`). Si el
usuario arregla un cable o sustituye una sonda con la pantalla encendida, no
aparece hasta el siguiente reinicio.

Además, hoy los roles se guardan **por posición**: `assignment[FRIGO_SLOT_CONGELADOR] = 1`
significa "la sonda número 1 de la lista", no "la sonda con este número de serie".
Mientras la lista solo se construya al arrancar eso es estable, pero en cuanto se
permite reescanear deja de serlo: si aparece una sonda nueva, las posiciones se
desplazan y el ventilador pasaría a regularse con la temperatura equivocada **sin
intervención del usuario**. Por eso el cambio de "por posición" a "por número de
serie" no es un extra: es lo que hace que el reescaneo sea seguro.

## Qué se construye

### 1. Reescaneo bajo petición

Botón **"Buscar sondas"** en el panel del frigo. Llama a `frigo_request_rescan()`,
que solo levanta una bandera. El escaneo real ocurre dentro de `frigo_task`, nunca
desde el hilo de LVGL: así no se pisa con el ciclo de conversión de temperatura ni
con el mutex del estado.

### 2. Reescaneo automático de fondo

Dentro de `frigo_task`, cada 5 minutos. Dos caminos:

- **Camino barato (el normal):** `onewire_bus_reset()` + recorrido de ROMs sobre el
  bus existente. No destruye nada.
- **Camino de recuperación:** solo si el reset falla. Ahí sí se destruye y recrea el
  bus, hasta 5 intentos, exactamente como hace hoy `frigo_init` — porque tras un
  timeout el canal RX del RMT queda inservible y los reintentos sobre el mismo
  handle no tocan el cable.

Esta separación evita molestar cada 5 minutos a un sistema que funciona bien.

### 3. Roles anclados al número de serie

Cada DS18B20 trae una dirección ROM única de 64 bits (`onewire_device_t.address`),
que ya se guarda en `s_state.sensors[i].address`.

- Nuevo campo persistente: `uint64_t role_addr[FRIGO_MAX_SENSORS]` — la dirección de
  la sonda asignada a cada rol (aletas / congelador / exterior). `0` = sin asignar.
- Se guarda en NVS junto al resto de ajustes del frigo.
- `assignment[]` **se conserva** como campo derivado (rol → índice actual), para que
  `frigo_panel.c` y sus desplegables sigan funcionando sin cambios. Se recalcula
  después de cada escaneo a partir de `role_addr[]`.
- Cuando el usuario cambia un desplegable, `frigo_set_assignment()` actualiza además
  `role_addr[]` con la dirección de la sonda elegida.

**Migración:** al arrancar con `role_addr[]` vacío (primer arranque con esta
versión), se rellena con las direcciones que correspondan al `assignment[]` que ya
estuviera guardado. Nadie pierde su configuración.

### 4. Aviso emergente cuando algo cambia

El escaneo compara el conjunto de direcciones nuevo contra el anterior y clasifica:

| Situación | Qué se hace |
|---|---|
| Sonda **vuelve** (misma dirección) | Recupera su rol sola. Sin aviso. |
| Sonda **desaparece** | El rol queda "sin sonda". Aviso. |
| Sonda **nueva** (dirección desconocida) | Queda sin rol. Aviso. |
| Sin cambios | Nada. |

El estado gana un campo `frigo_scan_event_t scan_event`. El callback de refresco que
ya existe (`frigo_update_cb_t`) lo entrega a la UI; la UI muestra una ventana modal
explicando qué ha pasado y con un botón que lleva a la pantalla de asignación.
Después llama a `frigo_ack_scan_event()` para bajar la bandera.

La modal se crea desde el hilo de LVGL, siguiendo el patrón de
`ui_show_new_trip_dialog()`.

### 5. Modo automático sin sonda

El modo AUTO regula el ventilador por **T_Aletas** (`FRIGO_SLOT_ALETAS`), no por el
congelador. Si ese rol queda sin sonda, AUTO **no** regula con un valor inventado:
se comporta como el caso `n_sensors == 0` que ya existe hoy (atiende cambios
manuales del ventilador, no calcula).

Esto sustituye al apaño actual de `frigo_init`:

```c
if (s_state.assignment[i] >= s_state.n_sensors) s_state.assignment[i] = 0;
```

que ante un índice fuera de rango lo manda **en silencio a la sonda 0** — es decir,
regula con la sonda equivocada sin decir nada. Con roles por número de serie, un rol
sin sonda se marca como ausente en vez de reapuntar a otra.

## Cómo se prueba sin la autocaravana

La parte con riesgo real —decidir qué sonda es cuál y qué cambió— se extrae a un
módulo de **lógica pura**, `frigo_sondas.c` / `frigo_sondas.h`, sin dependencias de
ESP-IDF, siguiendo el patrón que ya usa `frigo_solar.c`:

```c
/* Entra: direcciones encontradas + roles guardados. Sale: indices por rol y que cambio. */
frigo_scan_result_t frigo_sondas_resolver(const uint64_t *encontradas, int n,
                                          const uint64_t *role_addr,
                                          uint8_t *assignment_out);
```

Test de host en `components/frigo/test/test_frigo_sondas.c`, compilable con gcc como
el del excedente solar. Casos: sin cambios, sonda que vuelve, sonda que desaparece,
sonda nueva, sustitución (una se va y otra llega), lista vacía, y la migración desde
`assignment[]` sin `role_addr[]`.

Lo que **no** se puede probar aquí: que el bus 1-Wire detecte de verdad una sonda
enchufada en caliente. Eso exige estar delante del frigo.

## Ficheros que se tocan

| Fichero | Qué |
|---|---|
| `components/frigo/frigo_sondas.{c,h}` | **Nuevo.** Lógica pura de resolución de roles. |
| `components/frigo/test/test_frigo_sondas.c` | **Nuevo.** Test de host. |
| `components/frigo/frigo.h` | `role_addr[]`, `scan_event`, `frigo_request_rescan()`, `frigo_ack_scan_event()`. |
| `components/frigo/frigo.c` | Extraer la enumeración a una función reutilizable; bandera de petición; temporizador de 5 min; persistencia de `role_addr[]`. |
| `components/frigo/CMakeLists.txt` | Añadir `frigo_sondas.c`. |
| `main/ui/frigo_panel.c` | Botón "Buscar sondas"; actualizar `role_addr` al cambiar un desplegable. |
| `main/ui.c` (o `frigo_panel.c`) | La ventana modal del aviso. |

## Fuera de alcance

- Detectar sondas en otros pines: el bus es solo GPIO4.
- Más de `FRIGO_MAX_SENSORS` (3) sondas.
- Avisar por Telegram o por la app: el aviso es en la pantalla.
