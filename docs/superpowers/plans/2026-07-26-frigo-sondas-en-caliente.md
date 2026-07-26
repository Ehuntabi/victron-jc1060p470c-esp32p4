# Sondas del frigo en caliente — plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Que la pantalla encuentre sondas DS18B20 conectadas o sustituidas sin reiniciar, sin que los roles (aletas / congelador / exterior) se bailen nunca solos.

**Architecture:** La decisión de "qué sonda es cuál y qué ha cambiado" se aísla en un módulo de lógica pura (`frigo_sondas.c`) sin dependencias de ESP-IDF, probado en el PC con gcc — igual que `frigo_solar.c`. `frigo.c` se limita a hablar con el bus 1-Wire y a aplicar lo que decide ese módulo. La UI solo muestra un botón y una ventana modal.

**Tech Stack:** C99, ESP-IDF 5.4.4, LVGL, componentes `espressif__onewire_bus` y `espressif__ds18b20`, NVS.

## Global Constraints

- ESP-IDF **5.4.4** exacto. Entorno: `. ~/.espressif/esp-idf-5.4/export.sh`.
- Textos en español, **sin acentos en los logs** (convención del repo).
- No imprimir nunca temperaturas ni direcciones en logs de nivel INFO en bucle: satura `log_capture`, que persiste a la SD.
- `FRIGO_MAX_SENSORS` sigue siendo **3**. No ampliar.
- Bus 1-Wire: solo **GPIO4** (`FRIGO_ONEWIRE_GPIO`).
- El escaneo del bus ocurre **siempre dentro de `frigo_task`**, nunca desde el hilo de LVGL ni desde un handler HTTP.
- Commits en la rama `feat/frigo-sondas-en-caliente`. Commit al final de cada tarea.

## Estructura de ficheros

| Fichero | Responsabilidad |
|---|---|
| `components/frigo/frigo_sondas.h` | **Nuevo.** Tipos y firmas de la lógica pura. |
| `components/frigo/frigo_sondas.c` | **Nuevo.** Resolver roles, detectar cambios, migrar. Sin ESP-IDF. |
| `components/frigo/test/test_frigo_sondas.c` | **Nuevo.** Test de host (gcc). |
| `components/frigo/CMakeLists.txt` | Añadir `frigo_sondas.c` a `SRCS`. |
| `components/frigo/frigo.h` | `role_addr[]`, `scan_event`, `frigo_request_rescan()`, `frigo_ack_scan_event()`. |
| `components/frigo/frigo.c` | Extraer la enumeración; bandera de petición; temporizador; persistir `role_addr[]`. |
| `main/ui/frigo_panel.c` | Botón "Buscar sondas"; modal de aviso; actualizar `role_addr` al cambiar un desplegable. |

---

### Task 1: Lógica pura de resolución de sondas

Esta tarea es **verificable hoy, sin la autocaravana**. Es la que contiene el riesgo real.

**Files:**
- Create: `components/frigo/frigo_sondas.h`
- Create: `components/frigo/frigo_sondas.c`
- Test: `components/frigo/test/test_frigo_sondas.c`

**Interfaces:**
- Consumes: nada (módulo hoja).
- Produces: `frigo_sondas_resolver()`, `frigo_sondas_migrar()`, `frigo_sondas_evento()`, el tipo `frigo_scan_result_t`, las banderas `FRIGO_SCAN_HAY_NUEVA` / `FRIGO_SCAN_FALTA_UNA` y la constante `FRIGO_SONDA_AUSENTE`. Task 2 y Task 3 dependen de estos nombres exactos.

- [ ] **Step 1: Escribir la cabecera**

Crear `components/frigo/frigo_sondas.h`:

```c
#pragma once
/* Logica pura de las sondas del frigo: quien es quien tras un escaneo del bus.
 *
 * Sin dependencias de ESP-IDF a proposito, para poder probarla en el PC con gcc
 * (ver test/test_frigo_sondas.c). frigo.c habla con el hardware; esto decide.
 *
 * La idea: cada DS18B20 trae una direccion ROM unica de fabrica. Los roles
 * (aletas / congelador / exterior) se guardan por ESA direccion, no por la
 * posicion en la lista, para que al aparecer o desaparecer una sonda los demas
 * roles no se desplacen. */
#include <stdint.h>
#include <stdbool.h>

#define FRIGO_SONDAS_MAX     3
#define FRIGO_SONDA_AUSENTE  0xFF   /* el rol no tiene sonda presente ahora mismo */

/* Que hay que decidir. Se combinan con OR. */
enum {
    FRIGO_SCAN_HAY_NUEVA = 1 << 0,  /* hay una sonda presente sin rol asignado */
    FRIGO_SCAN_FALTA_UNA = 1 << 1,  /* hay un rol cuya sonda no responde */
};

typedef struct {
    uint8_t flags;                            /* combinacion de FRIGO_SCAN_* */
    uint8_t asignacion[FRIGO_SONDAS_MAX];     /* rol -> indice en 'encontradas', o FRIGO_SONDA_AUSENTE */
    uint8_t n_sin_rol;                        /* cuantas sondas presentes no tienen rol */
} frigo_scan_result_t;

/* Resuelve que indice ocupa cada rol tras un escaneo.
 *   encontradas[0..n-1] : direcciones ROM leidas del bus ahora mismo
 *   role_addr[rol]      : direccion guardada para ese rol (0 = rol sin asignar)
 * No modifica role_addr. n se recorta a FRIGO_SONDAS_MAX. */
frigo_scan_result_t frigo_sondas_resolver(const uint64_t *encontradas, int n,
                                          const uint64_t role_addr[FRIGO_SONDAS_MAX]);

/* Primer arranque con esta version: role_addr esta a cero y lo unico guardado es
 * la asignacion por posicion de siempre. Rellena role_addr con las direcciones
 * que le correspondan para no perder la configuracion del usuario.
 * Devuelve true si escribio algo. No hace nada si role_addr ya tiene contenido. */
bool frigo_sondas_migrar(const uint64_t *encontradas, int n,
                         const uint8_t asignacion[FRIGO_SONDAS_MAX],
                         uint64_t role_addr[FRIGO_SONDAS_MAX]);

/* Banderas que acaban de ACTIVARSE. Sirve para avisar al usuario solo cuando
 * aparece algo que decidir, y no repetir el aviso en cada escaneo mientras la
 * situacion siga igual. */
uint8_t frigo_sondas_evento(uint8_t flags_antes, uint8_t flags_ahora);
```

- [ ] **Step 2: Escribir el test que falla**

Crear `components/frigo/test/test_frigo_sondas.c`:

```c
/* Test host de la logica de sondas del frigo.
 * Compilar: gcc -I. frigo_sondas.c test/test_frigo_sondas.c -o /tmp/tfs && /tmp/tfs */
#include "frigo_sondas.h"
#include <stdio.h>
#include <assert.h>

#define A 0x2800000011111111ULL   /* aletas */
#define C 0x2800000022222222ULL   /* congelador */
#define E 0x2800000033333333ULL   /* exterior */
#define X 0x2800000099999999ULL   /* una desconocida */

int main(void)
{
    /* 1) Todo en su sitio: sin cambios y cada rol en su indice. */
    {
        uint64_t hay[] = { A, C, E };
        uint64_t roles[] = { A, C, E };
        frigo_scan_result_t r = frigo_sondas_resolver(hay, 3, roles);
        assert(r.flags == 0);
        assert(r.asignacion[0] == 0 && r.asignacion[1] == 1 && r.asignacion[2] == 2);
        assert(r.n_sin_rol == 0);
    }
    /* 2) El bus las devuelve en otro orden: los roles NO se bailan. */
    {
        uint64_t hay[] = { E, A, C };
        uint64_t roles[] = { A, C, E };
        frigo_scan_result_t r = frigo_sondas_resolver(hay, 3, roles);
        assert(r.flags == 0);
        assert(r.asignacion[0] == 1);   /* aletas esta ahora en la posicion 1 */
        assert(r.asignacion[1] == 2);
        assert(r.asignacion[2] == 0);
    }
    /* 3) Falta el congelador: se marca ausente y se avisa. Los otros no se mueven. */
    {
        uint64_t hay[] = { A, E };
        uint64_t roles[] = { A, C, E };
        frigo_scan_result_t r = frigo_sondas_resolver(hay, 2, roles);
        assert(r.flags == FRIGO_SCAN_FALTA_UNA);
        assert(r.asignacion[0] == 0);
        assert(r.asignacion[1] == FRIGO_SONDA_AUSENTE);
        assert(r.asignacion[2] == 1);
    }
    /* 4) Aparece una desconocida: se avisa, pero no se le da rol sola. */
    {
        uint64_t hay[] = { A, C, X };
        uint64_t roles[] = { A, C, 0 };
        frigo_scan_result_t r = frigo_sondas_resolver(hay, 3, roles);
        assert(r.flags == FRIGO_SCAN_HAY_NUEVA);
        assert(r.n_sin_rol == 1);
        assert(r.asignacion[2] == FRIGO_SONDA_AUSENTE);
    }
    /* 5) Sustitucion: se va la del congelador y llega otra nueva. Las dos cosas. */
    {
        uint64_t hay[] = { A, X, E };
        uint64_t roles[] = { A, C, E };
        frigo_scan_result_t r = frigo_sondas_resolver(hay, 3, roles);
        assert(r.flags == (FRIGO_SCAN_HAY_NUEVA | FRIGO_SCAN_FALTA_UNA));
        assert(r.asignacion[1] == FRIGO_SONDA_AUSENTE);
        assert(r.n_sin_rol == 1);
    }
    /* 6) Bus vacio: todos ausentes, se avisa de que faltan. */
    {
        uint64_t roles[] = { A, C, E };
        frigo_scan_result_t r = frigo_sondas_resolver(NULL, 0, roles);
        assert(r.flags == FRIGO_SCAN_FALTA_UNA);
        for (int i = 0; i < FRIGO_SONDAS_MAX; i++)
            assert(r.asignacion[i] == FRIGO_SONDA_AUSENTE);
    }
    /* 7) Sin roles guardados y sin sondas: no hay nada que decidir, no se avisa. */
    {
        uint64_t roles[] = { 0, 0, 0 };
        frigo_scan_result_t r = frigo_sondas_resolver(NULL, 0, roles);
        assert(r.flags == 0);
    }
    /* 8) Migracion: la asignacion por posicion de toda la vida se ancla a las series. */
    {
        uint64_t hay[] = { A, C, E };
        uint8_t asig[] = { 2, 0, 1 };        /* aletas=idx2, congelador=idx0, exterior=idx1 */
        uint64_t roles[] = { 0, 0, 0 };
        assert(frigo_sondas_migrar(hay, 3, asig, roles) == true);
        assert(roles[0] == E && roles[1] == A && roles[2] == C);
    }
    /* 9) Migracion que NO debe repetirse: si ya hay direcciones, no toca nada. */
    {
        uint64_t hay[] = { A, C, E };
        uint8_t asig[] = { 0, 1, 2 };
        uint64_t roles[] = { E, 0, 0 };
        assert(frigo_sondas_migrar(hay, 3, asig, roles) == false);
        assert(roles[0] == E);
    }
    /* 10) El aviso salta al aparecer el problema, no mientras dura. */
    {
        assert(frigo_sondas_evento(0, FRIGO_SCAN_FALTA_UNA) == FRIGO_SCAN_FALTA_UNA);
        assert(frigo_sondas_evento(FRIGO_SCAN_FALTA_UNA, FRIGO_SCAN_FALTA_UNA) == 0);
        assert(frigo_sondas_evento(FRIGO_SCAN_FALTA_UNA, 0) == 0);
        assert(frigo_sondas_evento(FRIGO_SCAN_FALTA_UNA,
                                   FRIGO_SCAN_FALTA_UNA | FRIGO_SCAN_HAY_NUEVA)
               == FRIGO_SCAN_HAY_NUEVA);
    }
    printf("test_frigo_sondas: OK\n");
    return 0;
}
```

- [ ] **Step 3: Ejecutar el test y comprobar que NO compila**

```bash
cd /home/jc/joint/victron/components/frigo
gcc -I. frigo_sondas.c test/test_frigo_sondas.c -o /tmp/tfs
```

Esperado: error, `frigo_sondas.c: No such file or directory`.

- [ ] **Step 4: Escribir la implementación mínima**

Crear `components/frigo/frigo_sondas.c`:

```c
#include "frigo_sondas.h"

/* Indice de 'addr' dentro de encontradas[], o FRIGO_SONDA_AUSENTE. */
static uint8_t indice_de(uint64_t addr, const uint64_t *encontradas, int n)
{
    if (addr == 0) return FRIGO_SONDA_AUSENTE;
    for (int i = 0; i < n; i++)
        if (encontradas[i] == addr) return (uint8_t)i;
    return FRIGO_SONDA_AUSENTE;
}

frigo_scan_result_t frigo_sondas_resolver(const uint64_t *encontradas, int n,
                                          const uint64_t role_addr[FRIGO_SONDAS_MAX])
{
    frigo_scan_result_t r = { .flags = 0, .n_sin_rol = 0 };
    if (n < 0) n = 0;
    if (n > FRIGO_SONDAS_MAX) n = FRIGO_SONDAS_MAX;

    /* Cada rol busca SU sonda por direccion. */
    for (int rol = 0; rol < FRIGO_SONDAS_MAX; rol++) {
        r.asignacion[rol] = indice_de(role_addr[rol], encontradas, n);
        if (role_addr[rol] != 0 && r.asignacion[rol] == FRIGO_SONDA_AUSENTE)
            r.flags |= FRIGO_SCAN_FALTA_UNA;
    }

    /* Sondas presentes que no reclama ningun rol. */
    for (int i = 0; i < n; i++) {
        bool reclamada = false;
        for (int rol = 0; rol < FRIGO_SONDAS_MAX && !reclamada; rol++)
            if (r.asignacion[rol] == (uint8_t)i) reclamada = true;
        if (!reclamada) {
            r.n_sin_rol++;
            r.flags |= FRIGO_SCAN_HAY_NUEVA;
        }
    }
    return r;
}

bool frigo_sondas_migrar(const uint64_t *encontradas, int n,
                         const uint8_t asignacion[FRIGO_SONDAS_MAX],
                         uint64_t role_addr[FRIGO_SONDAS_MAX])
{
    for (int rol = 0; rol < FRIGO_SONDAS_MAX; rol++)
        if (role_addr[rol] != 0) return false;   /* ya migrado */

    if (n < 0) n = 0;
    if (n > FRIGO_SONDAS_MAX) n = FRIGO_SONDAS_MAX;

    bool escrito = false;
    for (int rol = 0; rol < FRIGO_SONDAS_MAX; rol++) {
        uint8_t idx = asignacion[rol];
        if (idx < (uint8_t)n) {
            role_addr[rol] = encontradas[idx];
            escrito = true;
        }
    }
    return escrito;
}

uint8_t frigo_sondas_evento(uint8_t flags_antes, uint8_t flags_ahora)
{
    return (uint8_t)(flags_ahora & ~flags_antes);
}
```

- [ ] **Step 5: Ejecutar el test y comprobar que pasa**

```bash
cd /home/jc/joint/victron/components/frigo
gcc -Wall -Wextra -I. frigo_sondas.c test/test_frigo_sondas.c -o /tmp/tfs && /tmp/tfs
```

Esperado: `test_frigo_sondas: OK`, sin avisos del compilador.

- [ ] **Step 6: Añadir el fichero al componente y comprobar que el firmware sigue compilando**

En `components/frigo/CMakeLists.txt`, cambiar la línea de `SRCS`:

```cmake
    SRCS "frigo.c" "frigo_solar.c" "frigo_sondas.c"
```

```bash
cd /home/jc/joint/victron && . ~/.espressif/esp-idf-5.4/export.sh && idf.py build
```

Esperado: `Project build complete`.

- [ ] **Step 7: Commit**

```bash
cd /home/jc/joint/victron
git add components/frigo/frigo_sondas.c components/frigo/frigo_sondas.h \
        components/frigo/test/test_frigo_sondas.c components/frigo/CMakeLists.txt
git commit -m "feat(frigo): logica pura de resolucion de sondas por numero de serie"
```

---

### Task 2: Reescaneo del bus en `frigo.c`

**Files:**
- Modify: `components/frigo/frigo.h`
- Modify: `components/frigo/frigo.c`

**Interfaces:**
- Consumes de Task 1: `frigo_sondas_resolver()`, `frigo_sondas_migrar()`, `frigo_sondas_evento()`, `frigo_scan_result_t`, `FRIGO_SONDA_AUSENTE`, `FRIGO_SCAN_HAY_NUEVA`, `FRIGO_SCAN_FALTA_UNA`.
- Produces para Task 3: `void frigo_request_rescan(void)`, `void frigo_ack_scan_event(void)`, y los campos nuevos de `frigo_state_t`: `uint64_t role_addr[FRIGO_MAX_SENSORS]` y `uint8_t scan_event`.

- [ ] **Step 1: Ampliar `frigo.h`**

En `components/frigo/frigo.h`, añadir el include y los campos nuevos dentro de `frigo_state_t`, justo detrás de `assignment`:

```c
#include "frigo_sondas.h"

/* Dos cabeceras declaran el mismo tope por separado (frigo_sondas.h no puede
 * incluir frigo.h: tiene que compilar en el PC sin ESP-IDF). Que no se separen. */
_Static_assert(FRIGO_SONDAS_MAX == FRIGO_MAX_SENSORS,
               "FRIGO_SONDAS_MAX y FRIGO_MAX_SENSORS deben coincidir");
```

```c
    uint8_t assignment[FRIGO_MAX_SENSORS];
    /* Direccion ROM de la sonda de cada rol (0 = rol sin asignar). Es lo que
     * ancla cada rol a una sonda fisica: assignment[] se deriva de esto en cada
     * escaneo. Persiste en NVS. */
    uint64_t role_addr[FRIGO_MAX_SENSORS];
    /* Banderas FRIGO_SCAN_* que acaban de activarse y aun no ha visto el usuario.
     * La UI las lee, muestra el aviso y llama a frigo_ack_scan_event(). */
    uint8_t scan_event;
```

Y al final de la sección de funciones del frigo (junto a `frigo_set_mode`):

```c
/* Pide un escaneo del bus 1-Wire. Solo levanta una bandera: el escaneo real lo
 * hace frigo_task, que es quien puede tocar el bus sin pisar la lectura de
 * temperaturas. Se puede llamar desde la UI sin riesgo. */
void frigo_request_rescan(void);

/* La UI confirma que ya ha enseniado el aviso: baja scan_event. */
void frigo_ack_scan_event(void);
```

- [ ] **Step 2: Extraer la enumeración a una función reutilizable**

En `components/frigo/frigo.c`, sustituir el bucle de enumeración de `frigo_init` (el `for (int intento = 0; intento < 5 && s_state.n_sensors == 0; intento++)`) por una llamada a una función nueva. Añadir antes de `frigo_init`:

```c
/* Configuracion del bus, guardada para poder recrearlo en caliente. */
static onewire_bus_config_t s_bus_cfg = { .bus_gpio_num = FRIGO_ONEWIRE_GPIO };
static onewire_bus_rmt_config_t s_rmt_cfg = { .max_rx_bytes = 10 };

/* Recorre el bus y deja en 'out' las direcciones encontradas. Devuelve cuantas.
 *
 * 'recuperar' controla el coste: en el escaneo periodico se intenta primero por
 * las buenas (un reset sobre el bus que ya existe). Solo si eso falla se destruye
 * y recrea el bus, porque tras un timeout el canal RX del RMT queda en
 * INVALID_STATE y los resets siguientes sobre el mismo handle NO tocan el cable
 * (son un no-op silencioso). */
static int bus_enumerar(uint64_t out[FRIGO_MAX_SENSORS], bool recuperar)
{
    int n = 0;
    const int intentos = recuperar ? 5 : 1;

    for (int intento = 0; intento < intentos && n == 0; intento++) {
        if (intento > 0) {
            onewire_bus_del(s_bus);
            vTaskDelay(pdMS_TO_TICKS(200));
            if (onewire_new_bus_rmt(&s_bus_cfg, &s_rmt_cfg, &s_bus) != ESP_OK) {
                ESP_LOGE(TAG, "intento %d: recrear bus fallo", intento);
                continue;
            }
        }
        esp_err_t rr = onewire_bus_reset(s_bus);
        if (rr != ESP_OK) {
            ESP_LOGD(TAG, "intento %d: reset=%s", intento, esp_err_to_name(rr));
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        onewire_device_iter_handle_t iter;
        if (onewire_new_device_iter(s_bus, &iter) != ESP_OK) continue;
        onewire_device_t dev;
        while (onewire_device_iter_get_next(iter, &dev) == ESP_OK
               && n < FRIGO_MAX_SENSORS) {
            out[n++] = dev.address;
        }
        onewire_del_device_iter(iter);
    }
    return n;
}
```

**Nota para quien implemente:** `ESP_LOGI` del reset pasa a `ESP_LOGD`. Antes salía una vez al arrancar; ahora se ejecutaría cada 5 minutos y llenaría el log de la SD.

- [ ] **Step 3: Aplicar el resultado de un escaneo**

Añadir a `frigo.c`, después de `bus_enumerar`:

```c
/* Escanea y deja el estado coherente. Llamar SOLO desde frigo_task (o desde
 * frigo_init antes de arrancarla): toca s_devs y s_state. */
static void escanear_y_aplicar(bool recuperar)
{
    uint64_t hay[FRIGO_MAX_SENSORS] = {0};
    int n = bus_enumerar(hay, recuperar);

    /* Handles DS18B20 nuevos: los viejos ya no valen si el bus se ha recreado. */
    for (int i = 0; i < FRIGO_MAX_SENSORS; i++) s_devs[i] = NULL;
    for (int i = 0; i < n; i++) {
        onewire_device_t dev = { .bus = s_bus, .address = hay[i] };
        ds18b20_config_t ds_cfg = {};
        if (ds18b20_new_device_from_enumeration(&dev, &ds_cfg, &s_devs[i]) != ESP_OK)
            s_devs[i] = NULL;
    }

    /* Primer arranque con esta version: anclar la asignacion por posicion que ya
     * tuviera guardada el usuario a las direcciones de las sondas puestas. */
    frigo_sondas_migrar(hay, n, s_state.assignment, s_state.role_addr);

    frigo_scan_result_t r = frigo_sondas_resolver(hay, n, s_state.role_addr);

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.n_sensors = (uint8_t)n;
    for (int i = 0; i < FRIGO_MAX_SENSORS; i++) {
        s_state.sensors[i].address = (i < n) ? hay[i] : 0;
        s_state.sensors[i].valid   = (i < n);
        s_state.assignment[i]      = r.asignacion[i];
    }
    s_state.scan_event |= frigo_sondas_evento(s_scan_flags, r.flags);
    s_scan_flags = r.flags;
    if (s_mutex) xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "%d sonda(s) en bus GPIO%d (flags=0x%02x)",
             n, FRIGO_ONEWIRE_GPIO, r.flags);
}
```

Y declarar arriba, junto a los demás estáticos:

```c
static uint8_t  s_scan_flags   = 0;      /* ultimo estado visto, para detectar el flanco */
static volatile bool s_rescan_req = false;
```

- [ ] **Step 4: Usar la función nueva en `frigo_init`**

Sustituir en `frigo_init` el bloque de enumeración y el `ESP_LOGI` del recuento por:

```c
    escanear_y_aplicar(true);
```

Y **borrar** el apaño que reapunta a la sonda 0 (ya no aplica: un rol sin sonda debe quedarse ausente, no saltar a otra):

```c
    /* BORRAR estas dos lineas:
    for (int i = 0; i < FRIGO_MAX_SENSORS; i++)
        if (s_state.assignment[i] >= s_state.n_sensors)
            s_state.assignment[i] = 0;
    */
```

**Ojo:** `escanear_y_aplicar` usa `s_mutex`, así que la llamada tiene que ir **después** de crear el mutex en `frigo_init`.

- [ ] **Step 5: Petición de escaneo y temporizador en `frigo_task`**

Añadir las dos funciones públicas al final de `frigo.c`:

```c
void frigo_request_rescan(void) { s_rescan_req = true; }

void frigo_ack_scan_event(void)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_state.scan_event = 0;
        xSemaphoreGive(s_mutex);
    }
}
```

Y al principio del bucle de `frigo_task`, antes de disparar la conversión de temperatura:

```c
        /* Escaneo: bajo peticion, o de fondo cada 5 min. El periodico va por las
         * buenas (sin recrear el bus) para no molestar a un sistema que funciona;
         * el manual usa el camino de recuperacion porque el usuario acaba de
         * tocar el cableado. */
        static uint32_t ultimo_scan_ms = 0;
        uint32_t ahora_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_rescan_req) {
            s_rescan_req = false;
            escanear_y_aplicar(true);
            ultimo_scan_ms = ahora_ms;
        } else if (ahora_ms - ultimo_scan_ms >= 5 * 60 * 1000) {
            escanear_y_aplicar(false);
            ultimo_scan_ms = ahora_ms;
        }
```

- [ ] **Step 6: Que las lecturas respeten los roles ausentes**

Tres cambios en `frigo_task`, todos en el bloque que ya existe.

**6a.** Los handles pueden ser NULL si el bus enumeró la sonda pero `ds18b20_new_device_from_enumeration` falló. Proteger los dos bucles:

```c
        for (int i = 0; i < s_state.n_sensors; i++)
            if (s_devs[i]) ds18b20_trigger_temperature_conversion(s_devs[i]);
        vTaskDelay(pdMS_TO_TICKS(DS18B20_CONV_MS));

        for (int i = 0; i < s_state.n_sensors; i++) {
            float t;
            if (s_devs[i] && ds18b20_get_temperature(s_devs[i], &t) == ESP_OK)
                temps[i] = t;
        }
```

**6b.** `FRIGO_SONDA_AUSENTE` vale 0xFF y desbordaría `temps[]`. Sustituir:

```c
            float ta = temps[s_state.assignment[FRIGO_SLOT_ALETAS]];
            float tc = temps[s_state.assignment[FRIGO_SLOT_CONGELADOR]];
            float te = temps[s_state.assignment[FRIGO_SLOT_EXTERIOR]];
```

por:

```c
            /* -127.0f es el "sin dato" que ya usa todo el componente. */
            #define TEMP_DE(slot) \
                (s_state.assignment[(slot)] < s_state.n_sensors \
                     ? temps[s_state.assignment[(slot)]] : -127.0f)
            float ta = TEMP_DE(FRIGO_SLOT_ALETAS);
            float tc = TEMP_DE(FRIGO_SLOT_CONGELADOR);
            float te = TEMP_DE(FRIGO_SLOT_EXTERIOR);
            #undef TEMP_DE
```

**6c.** El modo AUTO no debe regular sin sonda de aletas. Sustituir:

```c
                case FRIGO_MODE_AUTO:
                default:
                    pct = compute_fan(s_state.T_Aletas, s_state.T_min,
                                      s_state.T_max, s_state.fan_percent);
                    break;
```

por:

```c
                case FRIGO_MODE_AUTO:
                default:
                    /* Sin sonda de aletas no se inventa una temperatura: mismo
                     * criterio que la rama "sin DS18B20" de mas arriba, el
                     * ventilador se para. */
                    pct = (s_state.T_Aletas <= -120.0f)
                            ? 0
                            : compute_fan(s_state.T_Aletas, s_state.T_min,
                                          s_state.T_max, s_state.fan_percent);
                    break;
```

- [ ] **Step 7: Persistir `role_addr[]`**

En `frigo.c`, junto a las demás claves NVS:

```c
#define NVS_KEY_ROLEADDR "roleaddr"
```

En `nvs_load()`, tras la carga de `NVS_KEY_ASSIGN`:

```c
    size_t rlen = sizeof(s_state.role_addr);
    if (nvs_get_blob(h, NVS_KEY_ROLEADDR, s_state.role_addr, &rlen) != ESP_OK)
        for (int i = 0; i < FRIGO_MAX_SENSORS; i++) s_state.role_addr[i] = 0;
```

En `nvs_save_task()`, junto a los demás `nvs_set_*`:

```c
            nvs_set_blob(h, NVS_KEY_ROLEADDR, s_state.role_addr,
                         sizeof(s_state.role_addr));
```

En `frigo_set_assignment()`, además de fijar el índice, anclar la dirección:

```c
        s_state.assignment[slot] = sensor_idx;
        s_state.role_addr[slot]  = (sensor_idx < s_state.n_sensors)
                                     ? s_state.sensors[sensor_idx].address : 0;
```

Y llamar a `nvs_save()` como ya se hace.

- [ ] **Step 8: Compilar**

```bash
cd /home/jc/joint/victron && . ~/.espressif/esp-idf-5.4/export.sh && idf.py build
```

Esperado: `Project build complete`. Los únicos avisos aceptables son los tres previos de `main` (`ap_netif`, un `strncpy` y un `snprintf` de datalogger).

- [ ] **Step 9: Commit**

```bash
cd /home/jc/joint/victron
git add components/frigo/frigo.c components/frigo/frigo.h
git commit -m "feat(frigo): reescaneo del bus 1-Wire bajo peticion y cada 5 minutos"
```

---

### Task 3: Botón y aviso en la pantalla

**Files:**
- Modify: `main/ui/frigo_panel.c`

**Interfaces:**
- Consumes de Task 2: `frigo_request_rescan()`, `frigo_ack_scan_event()`, `state->scan_event`, `FRIGO_SCAN_HAY_NUEVA`, `FRIGO_SCAN_FALTA_UNA`, `FRIGO_SONDA_AUSENTE`.
- Produces: nada (hoja).

- [ ] **Step 1: Botón "Buscar sondas"**

En `main/ui/frigo_panel.c`, junto a los desplegables de asignación, añadir:

```c
static void buscar_sondas_cb(lv_event_t *e)
{
    (void)e;
    frigo_request_rescan();
}
```

y al construir el panel:

```c
    lv_obj_t *btn_buscar = lv_btn_create(fila_sondas);
    lv_obj_add_event_cb(btn_buscar, buscar_sondas_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_buscar = lv_label_create(btn_buscar);
    lv_label_set_text(lbl_buscar, "Buscar sondas");
    lv_obj_center(lbl_buscar);
```

- [ ] **Step 2: Modal de aviso**

**No usar `lv_msgbox`: no se usa en ningún sitio del repo.** El patrón de la casa es
construir el modal a mano sobre `lv_layer_top()`, como `ui_show_new_trip_dialog()`
en `main/ui/settings_panel.c:1535`. Copiar esa estructura (fondo negro al 70 %,
tarjeta 600x280 con borde de color, título + mensaje + botón).

En `main/ui/frigo_panel.c`:

```c
/* Aviso de que el conjunto de sondas ha cambiado. Se crea desde el hilo de LVGL
 * (lo llama ui_frigo_panel_update). Mismo aspecto que el dialogo de nuevo viaje. */
static lv_obj_t *s_aviso_sondas_modal = NULL;

static void aviso_sondas_cerrar_cb(lv_event_t *e)
{
    (void)e;
    frigo_ack_scan_event();
    if (s_aviso_sondas_modal) {
        lv_obj_del(s_aviso_sondas_modal);
        s_aviso_sondas_modal = NULL;
    }
}

static void mostrar_aviso_sondas(uint8_t flags)
{
    if (s_aviso_sondas_modal) return;   /* guard anti doble-apertura */

    const char *txt;
    if ((flags & FRIGO_SCAN_HAY_NUEVA) && (flags & FRIGO_SCAN_FALTA_UNA))
        txt = "Se ha cambiado una sonda del frigo.\n"
              "Comprueba cual es cual antes de fiarte de las temperaturas.";
    else if (flags & FRIGO_SCAN_HAY_NUEVA)
        txt = "Hay una sonda nueva sin asignar.\n"
              "Dile si es aletas, congelador o exterior.";
    else
        txt = "Una sonda del frigo ha dejado de responder.\n"
              "Revisa el cable o sustituyela.";

    lv_obj_t *modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    s_aviso_sondas_modal = modal;

    lv_obj_t *dlg = lv_obj_create(modal);
    lv_obj_set_size(dlg, 600, 280);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0xFFBB33), 0);  /* ambar: atencion */
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_set_layout(dlg, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(dlg);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28_es, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFBB33), 0);
    lv_label_set_text(title, LV_SYMBOL_WARNING "  Sondas del frigo");

    lv_obj_t *msg = lv_label_create(dlg);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(msg, txt);

    lv_obj_t *btn = lv_btn_create(dlg);
    lv_obj_set_size(btn, 240, 60);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFBB33), 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, "Entendido");
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_24_es, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(0x0A0A0A), 0);
    lv_obj_center(lb);
    lv_obj_add_event_cb(btn, aviso_sondas_cerrar_cb, LV_EVENT_CLICKED, NULL);
}
```

- [ ] **Step 3: Disparar el aviso desde el refresco**

La función que recibe el estado es `ui_frigo_panel_update(ui_state_t *ui, const frigo_state_t *state)`, en `main/ui/frigo_panel.c:840`. Añadir al final de su cuerpo:

```c
    if (state->scan_event != 0) mostrar_aviso_sondas(state->scan_event);
```

`ui_frigo_panel_update` la llama `frigo_update_cb` de `main/main.c:215` dentro de
`lvgl_port_lock(50)` (verificado): crear objetos aquí es seguro.

**Por qué `scan_event` se queda puesto hasta que la UI lo baja:** ese
`lvgl_port_lock(50)` puede agotar el tiempo y saltarse la llamada. Si el aviso
fuese un disparo único se perdería justo cuando la pantalla está ocupada. Al ser
una bandera que solo limpia `frigo_ack_scan_event()`, el aviso aparece en el
siguiente refresco que sí consiga el lock.

- [ ] **Step 4: Que el desplegable enseñe los roles ausentes**

Donde hoy se hace `lv_dropdown_set_selected(s_dd_aletas, st->assignment[FRIGO_SLOT_ALETAS])`, proteger el caso ausente para no seleccionar el índice 0xFF:

```c
    #define SEL(dd, slot) \
        if (dd) lv_dropdown_set_selected((dd), \
            st->assignment[(slot)] < st->n_sensors ? st->assignment[(slot)] : 0)
    SEL(s_dd_aletas,     FRIGO_SLOT_ALETAS);
    SEL(s_dd_congelador, FRIGO_SLOT_CONGELADOR);
    SEL(s_dd_exterior,   FRIGO_SLOT_EXTERIOR);
    #undef SEL
```

- [ ] **Step 5: Compilar**

```bash
cd /home/jc/joint/victron && . ~/.espressif/esp-idf-5.4/export.sh && idf.py build
```

Esperado: `Project build complete`.

- [ ] **Step 6: Commit**

```bash
cd /home/jc/joint/victron
git add main/ui/frigo_panel.c
git commit -m "feat(frigo): boton Buscar sondas y aviso al cambiar el conjunto"
```

---

## Prueba final (requiere estar en la autocaravana)

Estos pasos **no se pueden hacer desde el PC**: hace falta la pantalla montada y las sondas conectadas.

1. Arrancar con las sondas puestas → deben aparecer las 3 y conservar la asignación de antes (migración).
2. Desenchufar una sonda → en ≤5 min salta el aviso "ha dejado de responder", y su rol queda ausente.
3. Volver a enchufarla → recupera su rol **sola**, sin aviso.
4. Soltar las tres y reconectarlas en otro orden → cada una vuelve a su rol.
5. Pulsar "Buscar sondas" con una sonda nueva → salta el aviso de sonda sin asignar.
6. Con la sonda de aletas ausente y modo AUTO → el ventilador no cambia solo; respeta el % manual.
