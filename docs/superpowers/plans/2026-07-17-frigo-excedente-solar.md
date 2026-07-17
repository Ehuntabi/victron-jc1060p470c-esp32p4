# Modo "Aprovechar excedente solar" — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Que el P4 conmute el frigo trivalente a 12V los días de sol (batería llena + PV suficiente, sin 230V) para ahorrar gas, con corte de seguridad por SoC/red y un indicador en la vista principal.

**Architecture:** La lógica de decisión es una **función pura** (`frigo_solar_eval`) sin dependencias de hardware, testeable en host. Vive en el componente `frigo`, que además posee la salida GPIO del relé piloto, la config NVS y la API. `main` **empuja** la telemetría (SoC/PV de Victron, `shore`/`fresh` del NE185) hacia el componente cada ~1 s (el componente no depende de `main`). La UI de ajustes va en `frigo_panel.c` y el indicador en `ui.c`.

**Tech Stack:** ESP-IDF v5.4.4 (C), FreeRTOS, LVGL, NVS, esp_timer. Sin framework de tests en el proyecto: el único test automático es un binario host (gcc) sobre la función pura; el resto se verifica con `idf.py build` + banco por inyección.

## Global Constraints

- ESP-IDF **v5.4.4** exacto. Entorno: `. ~/.espressif/esp-idf-5.4/export.sh` antes de compilar.
- Compilar siempre con `idf.py build` (NO `fullclean`).
- Persistencia NVS **por componente** con namespace propio (`"frigo"`).
- Textos y logs **en español, sin acentos ni emojis** (salvo `LV_SYMBOL_*`).
- Salida del relé piloto: **GPIO1** (JP1, liberado al quitar el PZEM). Nivel **bajo = relé OFF** al arrancar. Se asume relé **activo a nivel alto** (GPIO high → bobina relé tocho energizada → frigo 12V); si el módulo de relé fuera activo-bajo, invertir el nivel en un único punto (`frigo_solar_tick`).
- Constantes fijas en código: `PV_MIN=80 W`, `MIN_ON=30 min`, `ACT_DEBOUNCE=60 s`, ventana de frescura `30 s`.
- Defaults NVS ajustables: `sol_en=0`, `sol_on=95`, `sol_off=80`.
- El commit A (quitar PZEM) es **prerrequisito** y va antes que la feature.

Spec de referencia: `docs/superpowers/specs/2026-07-17-frigo-excedente-solar-design.md`.

## File Structure

- `main/pzem004t.{c,h}` — **BORRAR** (Task 1).
- `main/main.c` — Task 1 (quitar init PZEM), Task 4 (timer de feed).
- `main/watchdog.h` — Task 1 (quitar `WD_TASK_PZEM`).
- `main/dashboard_state.{c,h}` — Task 1 (quitar objeto `"ac"`), Task 4 (exponer `pv_w` + frescura).
- `main/ui.c` — Task 1 (quitar indicador AC), Task 6 (indicador excedente solar).
- `main/ui/ui_state.h` — Task 1 (quitar `lbl_ac`), Task 6 (añadir `lbl_solar`).
- `main/config_server.c` — Task 1 (quitar HTML/JS del bloque AC).
- `components/frigo/frigo_solar.{c,h}` — **CREAR** (Task 2): función pura + tipos.
- `components/frigo/test/test_frigo_solar.c` — **CREAR** (Task 2): test host gcc.
- `components/frigo/frigo.{c,h}` — Task 3 (GPIO + NVS + API + tick).
- `main/ui/frigo_panel.c` — Task 5 (toggle + ajustes +/-).

---

### Task 1: Retirar el PZEM-004T (prerrequisito)

Elimina el medidor AC PZEM que no se instalará. Libera GPIO1/GPIO2 y UART2, y quita el indicador "AC" de la UI (su hueco lo reutilizará el indicador solar en Task 6).

**Files:**
- Delete: `main/pzem004t.c`, `main/pzem004t.h`
- Modify: `main/main.c` (include + bloque init), `main/watchdog.h:29-34` (enum), `main/dashboard_state.c` (include, `pzem_get`, objeto JSON `"ac"`), `main/ui.c` (include, `ac_indicator_timer_cb`, creación `lbl_ac`, `lv_timer_create`), `main/ui/ui_state.h` (campo `lbl_ac`), `main/config_server.c` (div `acst` + JS que lee `j.ac`)

**Interfaces:**
- Consumes: nada.
- Produces: enum `wd_task_t` sin `WD_TASK_PZEM` (queda `WD_TASK_NE185=0, WD_TASK_FRIGO, WD_TASK_COUNT`); JSON `/api/state` sin objeto `"ac"`; GPIO1 libre.

- [ ] **Step 1: Borrar los ficheros del módulo**

```bash
cd /home/jc/joint/victron
git rm main/pzem004t.c main/pzem004t.h
```

- [ ] **Step 2: Quitar init en `main/main.c`**

Elimina la línea `#include "pzem004t.h"` (~línea 37) y el bloque completo de configuración/arranque del PZEM (~líneas 445-456: comentario + `pzem_config_t pzem_cfg = {...}` con UART_NUM_2/GPIO_NUM_1/GPIO_NUM_2 + `pzem_init(&pzem_cfg);`).

- [ ] **Step 3: Quitar `WD_TASK_PZEM` del enum en `main/watchdog.h`**

Deja el enum así (líneas 29-34):

```c
typedef enum {
    WD_TASK_NE185 = 0,
    WD_TASK_FRIGO,
    WD_TASK_COUNT
} wd_task_t;
```

(`WD_TASK_COUNT` se reindexa solo; no hay tablas indexadas por valor literal fuera del propio watchdog — verificar con grep en Step 8.)

- [ ] **Step 4: Quitar el PZEM de `main/dashboard_state.c`**

- Borra `#include "pzem004t.h"` (~línea 4).
- Borra `pzem_data_t pz; pzem_get(&pz);` (~línea 157).
- Borra del `snprintf` de `dashboard_state_to_json()` el objeto JSON `"ac":{...}` completo (~líneas 196-237 del formato) **y sus argumentos `pz.*` correspondientes** al final del `snprintf`. Cuida las comas: el objeto anterior debe cerrar correctamente el JSON.

- [ ] **Step 5: Quitar el indicador AC de `main/ui.c` y `main/ui/ui_state.h`**

- En `ui.c`: borra `#include "pzem004t.h"` (~línea 36), la función `ac_indicator_timer_cb()` completa (~142-166), la creación del label `ui->lbl_ac` (~488-501) y la línea `lv_timer_create(ac_indicator_timer_cb, 2000, ui);` (~524).
- En `ui/ui_state.h`: borra el campo `lv_obj_t *lbl_ac;` (~línea 113).

- [ ] **Step 6: Quitar el bloque AC de la web en `main/config_server.c`**

- Borra `<div id='acst' ...>` (~línea 1076) del HTML del dashboard.
- Borra el JS que lee `j.ac.freq_hz/pf/energy_wh/alarm` y el texto `"PZEM-004T no conectado"` (~1118-1124).

- [ ] **Step 7: Compilar**

```bash
. ~/.espressif/esp-idf-5.4/export.sh
cd /home/jc/joint/victron
idf.py build
```

Expected: build **OK**, sin referencias colgadas a `pzem`. Si el linker se queja de `pzem_get`/`pzem_init`, quedó una referencia; búscala.

- [ ] **Step 8: Verificar que no queda rastro en firmware**

```bash
cd /home/jc/joint/victron
grep -rn "pzem\|PZEM\|WD_TASK_PZEM\|lbl_ac\|j\.ac\|UART_NUM_2" main/ components/ | grep -v build/
```

Expected: sin resultados en código activo (pueden quedar menciones en `docs/`, `README.md`, `scripts/` — opcional, no afectan al build).

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "refactor: retirar PZEM-004T (no se instala); libera GPIO1/2 y UART2

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Función pura de decisión `frigo_solar_eval` + test host

Núcleo de la lógica: máquina de estados sin hardware, testeable con gcc en host. Aquí se validan histéresis, debounce, bloque mínimo y prioridad de los cortes de seguridad.

**Files:**
- Create: `components/frigo/frigo_solar.h`, `components/frigo/frigo_solar.c`
- Test: `components/frigo/test/test_frigo_solar.c`

**Interfaces:**
- Consumes: nada (solo stdint/stdbool).
- Produces: `bool frigo_solar_eval(const frigo_solar_in_t *in, frigo_solar_sm_t *sm)` y los tipos `frigo_solar_in_t`, `frigo_solar_sm_t`, más las constantes `FRIGO_SOLAR_PV_MIN_W`, `FRIGO_SOLAR_MIN_ON_MS`, `FRIGO_SOLAR_ACT_DEBOUNCE_MS`.

- [ ] **Step 1: Escribir el test que falla — `components/frigo/test/test_frigo_solar.c`**

```c
/* Test host de la maquina de estados del modo excedente solar.
 * Compilar: gcc -I. frigo_solar.c test/test_frigo_solar.c -o /tmp/tfs && /tmp/tfs */
#include "frigo_solar.h"
#include <stdio.h>
#include <assert.h>

/* Entradas por defecto: modo ON, todo fresco, sin red, SoC 96%, PV 120W,
 * umbrales 95/80. now_ms lo fija cada caso. */
static frigo_solar_in_t base(uint32_t now_ms) {
    frigo_solar_in_t in = {
        .enabled = true, .soc_deci = 960, .pv_w = 120, .shore = false,
        .fresh = true, .soc_on_pct = 95, .soc_off_pct = 80, .now_ms = now_ms,
    };
    return in;
}

int main(void) {
    /* 1) Modo OFF: nunca activa aunque las condiciones sean buenas. */
    {
        frigo_solar_sm_t sm = {0};
        frigo_solar_in_t in = base(0); in.enabled = false;
        assert(frigo_solar_eval(&in, &sm) == false);
        in.now_ms = 200000; assert(frigo_solar_eval(&in, &sm) == false);
    }
    /* 2) Activacion tras debounce de 60 s de condiciones sostenidas. */
    {
        frigo_solar_sm_t sm = {0};
        frigo_solar_in_t in = base(1000);
        assert(frigo_solar_eval(&in, &sm) == false);      /* arma */
        in.now_ms = 1000 + 59000; assert(frigo_solar_eval(&in, &sm) == false);
        in.now_ms = 1000 + 60000; assert(frigo_solar_eval(&in, &sm) == true);
    }
    /* 3) Con 230V (shore) nunca activa; y si estaba ON, corta al instante. */
    {
        frigo_solar_sm_t sm = {0};
        frigo_solar_in_t in = base(1000); in.shore = true;
        in.now_ms = 1000 + 120000; assert(frigo_solar_eval(&in, &sm) == false);
        /* forzar ON y luego meter shore */
        sm = (frigo_solar_sm_t){0}; in = base(1000);
        frigo_solar_eval(&in, &sm); in.now_ms = 61000;
        assert(frigo_solar_eval(&in, &sm) == true);
        in.shore = true; in.now_ms = 61100;
        assert(frigo_solar_eval(&in, &sm) == false);      /* inmediato, sin MIN_ON */
    }
    /* 4) SoC bajo el suelo: corte inmediato. */
    {
        frigo_solar_sm_t sm = {0};
        frigo_solar_in_t in = base(1000);
        frigo_solar_eval(&in, &sm); in.now_ms = 61000;
        assert(frigo_solar_eval(&in, &sm) == true);
        in.soc_deci = 790; in.now_ms = 61100;             /* 79% < 80% */
        assert(frigo_solar_eval(&in, &sm) == false);
    }
    /* 5) Sin datos frescos: corte/off. */
    {
        frigo_solar_sm_t sm = {0};
        frigo_solar_in_t in = base(1000); in.fresh = false;
        in.now_ms = 200000; assert(frigo_solar_eval(&in, &sm) == false);
    }
    /* 6) Histeresis + MIN_ON: aguanta nube antes de 30 min; corta despues. */
    {
        frigo_solar_sm_t sm = {0};
        frigo_solar_in_t in = base(1000);
        frigo_solar_eval(&in, &sm); in.now_ms = 61000;
        assert(frigo_solar_eval(&in, &sm) == true);       /* ON en t=61000 */
        in.pv_w = 40; in.now_ms = 61000 + 100000;         /* nube, <MIN_ON */
        assert(frigo_solar_eval(&in, &sm) == true);       /* aguanta */
        in.now_ms = 61000 + FRIGO_SOLAR_MIN_ON_MS + 1;    /* pasado MIN_ON, sin sol */
        assert(frigo_solar_eval(&in, &sm) == false);      /* corta */
    }
    printf("ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Crear la cabecera — `components/frigo/frigo_solar.h`**

```c
/* frigo_solar.h — Maquina de estados PURA del modo "aprovechar excedente solar".
 * Sin dependencias de hardware ni FreeRTOS: testeable en host (gcc). */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Constantes fijas (no ajustables por UI). */
#define FRIGO_SOLAR_PV_MIN_W        80u              /* W minimos de panel = "hay sol real" */
#define FRIGO_SOLAR_MIN_ON_MS       (30u*60u*1000u)  /* bloque minimo encendido (30 min) */
#define FRIGO_SOLAR_ACT_DEBOUNCE_MS (60u*1000u)      /* condiciones sostenidas antes de activar */

/* Entradas de una evaluacion (valores ya escalados como en dashboard_state/ne185). */
typedef struct {
    bool     enabled;      /* toggle maestro del modo */
    uint16_t soc_deci;     /* SoC en 0.1 %  (960 = 96.0 %) */
    uint16_t pv_w;         /* potencia del panel en W */
    bool     shore;        /* hay 230 V (NE185) */
    bool     fresh;        /* telemetria Victron Y NE185 recientes */
    uint8_t  soc_on_pct;   /* umbral de activacion (%) */
    uint8_t  soc_off_pct;  /* suelo de corte (%) */
    uint32_t now_ms;       /* reloj monotonico en ms */
} frigo_solar_in_t;

/* Estado persistente entre evaluaciones (RAM, no NVS). Inicializar a {0}. */
typedef struct {
    bool     active;           /* salida actual del rele */
    uint32_t active_since_ms;  /* instante de entrada al estado actual */
    bool     arming;           /* condiciones de activacion cumpliendose */
    uint32_t arming_since_ms;  /* desde cuando */
} frigo_solar_sm_t;

/* Evalua la maquina de estados. Actualiza *sm y devuelve el nivel del rele
 * (true = ON = frigo a 12V por excedente). Funcion pura. */
bool frigo_solar_eval(const frigo_solar_in_t *in, frigo_solar_sm_t *sm);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Implementar — `components/frigo/frigo_solar.c`**

```c
#include "frigo_solar.h"

/* Precondiciones de seguridad: si cualquiera falla, el rele va a OFF de
 * inmediato (sin esperar al bloque de MIN_ON). */
static bool preconds_ok(const frigo_solar_in_t *in)
{
    if (!in->enabled) return false;
    if (!in->fresh)   return false;
    if (in->shore)    return false;                       /* hay 230V -> no 12V */
    if (in->soc_deci < (uint16_t)in->soc_off_pct * 10)    /* suelo de SoC */
        return false;
    return true;
}

bool frigo_solar_eval(const frigo_solar_in_t *in, frigo_solar_sm_t *sm)
{
    if (!preconds_ok(in)) {
        if (sm->active) { sm->active = false; sm->active_since_ms = in->now_ms; }
        sm->arming = false;
        return false;
    }

    /* Condiciones de activacion (ademas de precondiciones). */
    bool act_cond = (in->soc_deci >= (uint16_t)in->soc_on_pct * 10) &&
                    (in->pv_w >= FRIGO_SOLAR_PV_MIN_W);

    if (!sm->active) {
        if (act_cond) {
            if (!sm->arming) { sm->arming = true; sm->arming_since_ms = in->now_ms; }
            if ((uint32_t)(in->now_ms - sm->arming_since_ms) >= FRIGO_SOLAR_ACT_DEBOUNCE_MS) {
                sm->active = true;
                sm->active_since_ms = in->now_ms;
                sm->arming = false;
            }
        } else {
            sm->arming = false;
        }
    } else {
        /* ON: aguanta MIN_ON salvo seguridad (ya cubierta arriba). Corte por
         * perdida de sol solo tras cumplir el bloque minimo. */
        bool min_on_done = (uint32_t)(in->now_ms - sm->active_since_ms) >= FRIGO_SOLAR_MIN_ON_MS;
        if (min_on_done && in->pv_w < FRIGO_SOLAR_PV_MIN_W) {
            sm->active = false;
            sm->active_since_ms = in->now_ms;
        }
    }
    return sm->active;
}
```

- [ ] **Step 4: Compilar y correr el test en host**

```bash
cd /home/jc/joint/victron/components/frigo
gcc -I. frigo_solar.c test/test_frigo_solar.c -o /tmp/tfs && /tmp/tfs
```

Expected: `ALL PASS` (código de salida 0). Si un `assert` falla, corrige `frigo_solar.c` hasta que pase.

- [ ] **Step 5: Commit**

```bash
cd /home/jc/joint/victron
git add components/frigo/frigo_solar.h components/frigo/frigo_solar.c components/frigo/test/test_frigo_solar.c
git commit -m "feat(frigo): maquina de estados pura del modo excedente solar + test host

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Integrar en el componente `frigo` (GPIO1 + NVS + API + tick)

El componente pasa a poseer la salida del relé, la config persistente y la evaluación periódica dentro de `frigo_task`.

**Files:**
- Modify: `components/frigo/frigo.h` (defines + API pública), `components/frigo/frigo.c` (include, estáticos, NVS load/save, GPIO init, `frigo_solar_tick` en la task, setters/getters/feed)

**Interfaces:**
- Consumes (de Task 2): `frigo_solar_eval`, `frigo_solar_in_t`, `frigo_solar_sm_t`.
- Produces (para Tasks 4/5/6):
  - `void frigo_solar_feed(uint16_t soc_deci, uint16_t pv_w, bool shore, bool fresh)`
  - `esp_err_t frigo_solar_set_enabled(bool on)`
  - `esp_err_t frigo_solar_set_soc_on(uint8_t pct)`
  - `esp_err_t frigo_solar_set_soc_off(uint8_t pct)`
  - `bool frigo_solar_get_enabled(void)`
  - `uint8_t frigo_solar_get_soc_on(void)`
  - `uint8_t frigo_solar_get_soc_off(void)`
  - `bool frigo_solar_get_active(void)`

- [ ] **Step 1: Añadir defines y API en `components/frigo/frigo.h`**

Tras la línea `#define FRIGO_MAX_SENSORS 3` añade:

```c
/* ── Modo "aprovechar excedente solar" (rele 12V del frigo) ──────
 * GPIO1 (JP1, liberado del PZEM). Rele piloto que inyecta 13V a la bobina del
 * rele tocho que hoy activa el D+ en marcha. Nivel alto = rele energizado. */
#define FRIGO_SOLAR_RELAY_GPIO   1
```

Antes del cierre `#ifdef __cplusplus`/`}` final (o junto al resto de prototipos) añade:

```c
/* main empuja telemetria Victron + NE185 (~1 Hz). shore = hay 230V.
 * fresh = false si falta dato reciente de cualquiera de los dos buses. */
void frigo_solar_feed(uint16_t soc_deci, uint16_t pv_w, bool shore, bool fresh);

/* Config del modo (persiste en NVS namespace "frigo"). */
esp_err_t frigo_solar_set_enabled(bool on);
esp_err_t frigo_solar_set_soc_on(uint8_t pct);   /* clamp 80..100 */
esp_err_t frigo_solar_set_soc_off(uint8_t pct);  /* clamp 50..(soc_on-5) */
bool     frigo_solar_get_enabled(void);
uint8_t  frigo_solar_get_soc_on(void);
uint8_t  frigo_solar_get_soc_off(void);
/* Estado ON real (rele activado por excedente). Para el indicador principal. */
bool     frigo_solar_get_active(void);
```

- [ ] **Step 2: En `components/frigo/frigo.c`, añadir include y estáticos**

Tras `#include "frigo.h"` añade `#include "frigo_solar.h"`.

Tras `#define NVS_KEY_FANMIN "fanmin"` añade:

```c
#define NVS_KEY_SOL_EN   "sol_en"
#define NVS_KEY_SOL_ON   "sol_on"
#define NVS_KEY_SOL_OFF  "sol_off"
```

Tras la definición de `s_state` / `s_sim_mode` añade (protegidos por `s_mutex`):

```c
/* Modo excedente solar: config + estado + ultima telemetria empujada por main. */
static bool     s_sol_en      = false;
static uint8_t  s_sol_on_pct  = 95;
static uint8_t  s_sol_off_pct = 80;
static frigo_solar_sm_t s_sol_sm = {0};
static uint16_t s_sol_soc_deci = 0;
static uint16_t s_sol_pv_w     = 0;
static bool     s_sol_shore    = false;
static bool     s_sol_fresh    = false;
static uint32_t s_sol_feed_ms  = 0;
```

- [ ] **Step 3: Cargar y guardar la config en NVS**

En `nvs_load()`, antes de `nvs_close(h);`, añade:

```c
    uint8_t sv;
    if (nvs_get_u8(h, NVS_KEY_SOL_EN,  &sv) == ESP_OK) s_sol_en      = sv ? true : false;
    if (nvs_get_u8(h, NVS_KEY_SOL_ON,  &sv) == ESP_OK) s_sol_on_pct  = sv;
    if (nvs_get_u8(h, NVS_KEY_SOL_OFF, &sv) == ESP_OK) s_sol_off_pct = sv;
```

En `nvs_save_task()`, dentro del bloque protegido por el mutex (junto a los otros `nvs_set_u8`), añade:

```c
            nvs_set_u8(h, NVS_KEY_SOL_EN,  s_sol_en ? 1 : 0);
            nvs_set_u8(h, NVS_KEY_SOL_ON,  s_sol_on_pct);
            nvs_set_u8(h, NVS_KEY_SOL_OFF, s_sol_off_pct);
```

- [ ] **Step 4: Inicializar el GPIO del relé en `frigo_init`**

En `frigo_init()`, tras `nvs_load();`, añade:

```c
    /* Rele piloto del modo excedente solar: salida, apagado al arrancar. */
    gpio_config_t sol_cfg = {
        .pin_bit_mask = 1ULL << FRIGO_SOLAR_RELAY_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&sol_cfg);
    gpio_set_level(FRIGO_SOLAR_RELAY_GPIO, 0);
```

- [ ] **Step 5: Evaluar en `frigo_task` (tick a cada iteración)**

Añade la función interna antes de `frigo_task`:

```c
/* Un tick de la maquina de estados del modo excedente solar. Se llama en cada
 * iteracion de frigo_task (~1-3 s; MIN_ON=30min tolera esa cadencia). */
static void frigo_solar_tick(void)
{
    bool relay;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    /* Frescura efectiva: main debe refrescar; si deja de hacerlo, no fresco. */
    bool fresh = s_sol_fresh && ((uint32_t)(now - s_sol_feed_ms) < 3000u);
    frigo_solar_in_t in = {
        .enabled = s_sol_en, .soc_deci = s_sol_soc_deci, .pv_w = s_sol_pv_w,
        .shore = s_sol_shore, .fresh = fresh, .soc_on_pct = s_sol_on_pct,
        .soc_off_pct = s_sol_off_pct, .now_ms = now,
    };
    bool prev = s_sol_sm.active;
    relay = frigo_solar_eval(&in, &s_sol_sm);
    xSemaphoreGive(s_mutex);

    gpio_set_level(FRIGO_SOLAR_RELAY_GPIO, relay ? 1 : 0);
    if (relay != prev)
        ESP_LOGI(TAG, "Excedente solar: frigo 12V %s (SoC=%.1f%% PV=%dW)",
                 relay ? "ON" : "OFF", s_sol_soc_deci / 10.0f, s_sol_pv_w);
}
```

Dentro de `frigo_task`, justo tras `if (s_hb_cb) s_hb_cb();`, añade:

```c
        frigo_solar_tick();
```

- [ ] **Step 6: Implementar feed, setters y getters (API pública)**

Al final del fichero (junto al resto de la API pública) añade:

```c
void frigo_solar_feed(uint16_t soc_deci, uint16_t pv_w, bool shore, bool fresh)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    s_sol_soc_deci = soc_deci;
    s_sol_pv_w     = pv_w;
    s_sol_shore    = shore;
    s_sol_fresh    = fresh;
    s_sol_feed_ms  = (uint32_t)(esp_timer_get_time() / 1000);
    xSemaphoreGive(s_mutex);
}

esp_err_t frigo_solar_set_enabled(bool on)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_sol_en = on;
    xSemaphoreGive(s_mutex);
    nvs_save();
    return ESP_OK;
}

esp_err_t frigo_solar_set_soc_on(uint8_t pct)
{
    if (pct < 80)  pct = 80;
    if (pct > 100) pct = 100;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_sol_on_pct = pct;
    if (s_sol_off_pct > (uint8_t)(pct - 5)) s_sol_off_pct = pct - 5;  /* mantener histeresis */
    xSemaphoreGive(s_mutex);
    nvs_save();
    return ESP_OK;
}

esp_err_t frigo_solar_set_soc_off(uint8_t pct)
{
    if (pct < 50) pct = 50;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (pct > (uint8_t)(s_sol_on_pct - 5)) pct = s_sol_on_pct - 5;
    s_sol_off_pct = pct;
    xSemaphoreGive(s_mutex);
    nvs_save();
    return ESP_OK;
}

bool    frigo_solar_get_enabled(void) { return s_sol_en; }
uint8_t frigo_solar_get_soc_on(void)  { return s_sol_on_pct; }
uint8_t frigo_solar_get_soc_off(void) { return s_sol_off_pct; }
bool    frigo_solar_get_active(void)  { return s_sol_sm.active; }
```

- [ ] **Step 7: Compilar**

```bash
. ~/.espressif/esp-idf-5.4/export.sh
cd /home/jc/joint/victron
idf.py build
```

Expected: build **OK**.

- [ ] **Step 8: Verificación en banco (flash + monitor, opcional pero recomendado)**

Con el modo activado por defecto en frío no arranca (default OFF). Para probar sin sol real se usa el feed desde main (Task 4) más `ne185_sim_inject`. La verificación funcional completa se hace al terminar Task 4 (ver Task 4, Step 6). Aquí basta con que compile y que el GPIO1 arranque en 0.

- [ ] **Step 9: Commit**

```bash
git add components/frigo/frigo.h components/frigo/frigo.c
git commit -m "feat(frigo): salida GPIO1 + NVS + API del modo excedente solar

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Alimentar telemetría desde `main` + exponer `pv_w` y frescura

`main` lee SoC/PV (Victron) y `shore`/`fresh` (NE185) cada 1 s y los empuja al componente. Se añade `pv_w` y frescura al snapshot de `dashboard_state`.

**Files:**
- Modify: `main/dashboard_state.h` (campos nuevos en `dashboard_snapshot_t`), `main/dashboard_state.c` (timestamps + copia en snapshot), `main/main.c` (timer periódico de feed)

**Interfaces:**
- Consumes: `frigo_solar_feed(...)` (Task 3), `ne185_get(ne185_data_t*)` y `ne185_data_t.{shore,fresh}` (existente), `dashboard_state_snapshot(...)`.
- Produces: `dashboard_snapshot_t` con `uint16_t pv_w; bool bat_fresh; bool solar_fresh;`.

- [ ] **Step 1: Añadir campos al snapshot en `main/dashboard_state.h`**

Dentro de `typedef struct { ... } dashboard_snapshot_t;`, tras `int32_t bat_i_milli;` añade:

```c
    bool     solar_has;
    uint16_t pv_w;
    bool     bat_fresh;    /* SoC recibido en los ultimos 30 s */
    bool     solar_fresh;  /* PV recibido en los ultimos 30 s */
```

- [ ] **Step 2: Rastrear timestamps en `main/dashboard_state.c`**

Añade `#include "esp_timer.h"` arriba si no está. En el `static struct s` añade dos campos:

```c
    uint32_t bat_ms;
    uint32_t solar_ms;
```

En `dashboard_state_on_record()`: en los `case VICTRON_BLE_RECORD_BATTERY_MONITOR` y `case VICTRON_BLE_RECORD_LYNX_SMART_BMS` añade (dentro del bloque, junto a `s.bat_has = true;`):

```c
            s.bat_ms = (uint32_t)(esp_timer_get_time() / 1000);
```

En el `case VICTRON_BLE_RECORD_SOLAR_CHARGER` (junto a `s.solar_has = true;`):

```c
            s.solar_ms = (uint32_t)(esp_timer_get_time() / 1000);
```

- [ ] **Step 3: Rellenar los campos nuevos en `dashboard_state_snapshot`**

Dentro de `dashboard_state_snapshot()`, bajo el `lock()`, añade tras las asignaciones existentes:

```c
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    out->solar_has   = s.solar_has;
    out->pv_w        = s.pv_w;
    out->bat_fresh   = s.bat_has   && ((uint32_t)(now - s.bat_ms)   < 30000u);
    out->solar_fresh = s.solar_has && ((uint32_t)(now - s.solar_ms) < 30000u);
```

- [ ] **Step 4: Timer de feed en `main/main.c`**

Añade includes si faltan: `#include "dashboard_state.h"`, `#include "ne185/ne185.h"`, `#include "frigo.h"`, `#include "esp_timer.h"`.

Añade la callback (a nivel de fichero, antes de `app_main`):

```c
/* Empuja telemetria (Victron SoC/PV + NE185 shore/fresh) al modo excedente
 * solar del frigo, cada 1 s. fresh = todos los buses recientes. */
static void frigo_solar_feed_cb(void *arg)
{
    dashboard_snapshot_t snap;
    dashboard_state_snapshot(&snap);
    ne185_data_t ne; ne185_get(&ne);
    bool fresh = snap.bat_fresh && snap.solar_fresh && ne.fresh;
    frigo_solar_feed(snap.soc_deci, snap.pv_w, ne.shore, fresh);
}
```

Al final de `app_main` (tras inicializar frigo, ne185 y dashboard_state), arranca el timer periódico:

```c
    const esp_timer_create_args_t sol_feed = {
        .callback = frigo_solar_feed_cb,
        .name     = "sol_feed",
    };
    esp_timer_handle_t sol_feed_timer;
    if (esp_timer_create(&sol_feed, &sol_feed_timer) == ESP_OK)
        esp_timer_start_periodic(sol_feed_timer, 1000000);  /* 1 s */
```

- [ ] **Step 5: Compilar**

```bash
. ~/.espressif/esp-idf-5.4/export.sh
cd /home/jc/joint/victron
idf.py build
```

Expected: build **OK**.

- [ ] **Step 6: Verificación en banco (flash + monitor)**

```bash
idf.py -p /dev/ttyACM0 flash monitor   # o ttyACM1
```

Con datos Victron reales (o inyectados) y el modo activado desde el menú (Task 5) o temporalmente con `frigo_solar_set_enabled(true)`:
- Inyecta 230V con `ne185_sim_inject(...,/*shore=*/true)` → el log NO debe pasar a "ON" aunque SoC/PV sean altos (interlock).
- Con `shore=false`, SoC≥95% y PV≥80W durante 60 s → log `Excedente solar: frigo 12V ON`.
- Baja SoC bajo el suelo (o `ne185` deja de dar tramas → `fresh=false`) → `... 12V OFF` inmediato.

Expected: las transiciones ON/OFF cumplen los criterios de éxito 2, 3, 4 del spec.

- [ ] **Step 7: Commit**

```bash
git add main/dashboard_state.h main/dashboard_state.c main/main.c
git commit -m "feat(frigo): alimentar modo excedente solar con telemetria Victron+NE185

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: UI de ajustes en el menú frigo (toggle + SoC ajustables)

Switch ON/OFF del modo y dos ajustes +/− (SoC activación y suelo), replicando el patrón existente de `fanmin`.

**Files:**
- Modify: `main/ui/frigo_panel.c` (nuevos controles en la página del frigo)

**Interfaces:**
- Consumes (Task 3): `frigo_solar_get_enabled/set_enabled`, `frigo_solar_get_soc_on/set_soc_on`, `frigo_solar_get_soc_off/set_soc_off`.
- Produces: nada (UI hoja).

- [ ] **Step 1: Leer el patrón existente de `fanmin`**

Abre `main/ui/frigo_panel.c` y localiza el bloque de `fanmin` (aprox. líneas 212-240: `refresh_fanmin_label`, `btn_fanmin_minus_cb`, `btn_fanmin_plus_cb` y su creación de fila con label + botones +/−). Los nuevos controles se construyen **igual**, en la misma página, debajo de los de `fanmin`.

- [ ] **Step 2: Añadir el switch ON/OFF del modo**

Siguiendo el estilo de la página, añade una fila con un `lv_switch`:

```c
/* Callback del switch "Aprovechar excedente solar". */
static void sw_solar_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    frigo_solar_set_enabled(on);
}
```

Y en la función que puebla la página del frigo, tras los controles de fanmin:

```c
    lv_obj_t *row_sol = lv_obj_create(parent);   /* usar el mismo helper de fila que fanmin si existe */
    lv_obj_t *lbl_sol = lv_label_create(row_sol);
    lv_label_set_text(lbl_sol, "Excedente solar a 12V");
    lv_obj_t *sw_sol = lv_switch_create(row_sol);
    if (frigo_solar_get_enabled()) lv_obj_add_state(sw_sol, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_sol, sw_solar_cb, LV_EVENT_VALUE_CHANGED, NULL);
```

(Ajusta la creación de la fila/estilos al helper concreto que use `frigo_panel.c` para las filas de ajustes; el patrón de `fanmin` es la referencia.)

- [ ] **Step 3: Añadir los ajustes +/− de SoC activación y suelo**

Replica el patrón de `fanmin` (label + botón menos + botón más) dos veces:

```c
/* SoC de activacion (paso 1 %, rango 80..100). */
static lv_obj_t *s_lbl_solon;
static void refresh_solon_label(void)
{
    lv_label_set_text_fmt(s_lbl_solon, "Activar a SoC: %d%%", frigo_solar_get_soc_on());
}
static void btn_solon_minus_cb(lv_event_t *e)
{ frigo_solar_set_soc_on(frigo_solar_get_soc_on() - 1); refresh_solon_label(); }
static void btn_solon_plus_cb(lv_event_t *e)
{ frigo_solar_set_soc_on(frigo_solar_get_soc_on() + 1); refresh_solon_label(); }

/* Suelo de corte (paso 1 %, rango 50..soc_on-5). */
static lv_obj_t *s_lbl_soloff;
static void refresh_soloff_label(void)
{
    lv_label_set_text_fmt(s_lbl_soloff, "Cortar a SoC: %d%%", frigo_solar_get_soc_off());
}
static void btn_soloff_minus_cb(lv_event_t *e)
{ frigo_solar_set_soc_off(frigo_solar_get_soc_off() - 1); refresh_soloff_label(); }
static void btn_soloff_plus_cb(lv_event_t *e)
{ frigo_solar_set_soc_off(frigo_solar_get_soc_off() + 1); refresh_soloff_label(); }
```

Crea las dos filas con sus botones (igual que `fanmin`), asigna `s_lbl_solon`/`s_lbl_soloff` a los labels creados y llama a `refresh_solon_label()` / `refresh_soloff_label()` una vez tras crearlos para pintar el valor inicial. (Los setters ya hacen clamp y mantienen la histéresis, así que los `refresh` reflejan el valor efectivo tras cada pulsación.)

- [ ] **Step 4: Compilar**

```bash
. ~/.espressif/esp-idf-5.4/export.sh
cd /home/jc/joint/victron
idf.py build
```

Expected: build **OK**.

- [ ] **Step 5: Verificación en pantalla (flash)**

```bash
idf.py -p /dev/ttyACM0 flash
```

En Ajustes → frigo: aparece el switch "Excedente solar a 12V" y los dos ajustes de SoC. Cambiar el switch y los %; reiniciar; los valores persisten (criterio 7).

- [ ] **Step 6: Commit**

```bash
git add main/ui/frigo_panel.c
git commit -m "feat(ui): controles del modo excedente solar en el menu frigo

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Indicador en la vista principal

Señal visual en la barra inferior cuando el frigo está tirando de excedente solar (reutiliza el hueco del antiguo indicador AC).

**Files:**
- Modify: `main/ui/ui_state.h` (campo `lbl_solar`), `main/ui.c` (creación del label + timer que lo refresca)

**Interfaces:**
- Consumes (Task 3): `frigo_solar_get_active()`.
- Produces: nada.

- [ ] **Step 1: Añadir el campo al struct de UI**

En `main/ui/ui_state.h`, dentro de `ui_state_t` (donde estaba `lbl_ac`), añade:

```c
    lv_obj_t *lbl_solar;   /* indicador "frigo con excedente solar" en barra inferior */
```

- [ ] **Step 2: Crear el label en la barra inferior**

En `main/ui.c`, donde se crean los indicadores de la barra inferior (donde estaba `ui->lbl_ac`), crea el label oculto por defecto:

```c
    ui->lbl_solar = lv_label_create(/* contenedor de la barra inferior */);
    lv_label_set_text(ui->lbl_solar, LV_SYMBOL_CHARGE " 12V sol");
    lv_obj_add_flag(ui->lbl_solar, LV_OBJ_FLAG_HIDDEN);   /* oculto hasta que este activo */
```

(Usa el mismo contenedor/estilo que usaban los demás indicadores de la barra.)

- [ ] **Step 3: Refrescar con un timer**

Añade la callback y su timer (donde estaba `ac_indicator_timer_cb` / `lv_timer_create`):

```c
static void solar_indicator_timer_cb(lv_timer_t *t)
{
    ui_state_t *ui = (ui_state_t *)lv_timer_get_user_data(t);
    if (!ui || !ui->lbl_solar) return;
    if (frigo_solar_get_active())
        lv_obj_clear_flag(ui->lbl_solar, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(ui->lbl_solar, LV_OBJ_FLAG_HIDDEN);
}
```

Y su registro (junto al resto de timers de la UI):

```c
    lv_timer_create(solar_indicator_timer_cb, 2000, ui);
```

Añade `#include "frigo.h"` en `ui.c` si no está.

- [ ] **Step 4: Compilar**

```bash
. ~/.espressif/esp-idf-5.4/export.sh
cd /home/jc/joint/victron
idf.py build
```

Expected: build **OK**.

- [ ] **Step 5: Verificación en pantalla (flash)**

```bash
idf.py -p /dev/ttyACM0 flash
```

Forzando el modo a ON (SoC/PV altos, sin shore, tras el debounce), el indicador " 12V sol" aparece en la barra inferior; al pasar a OFF, se oculta (criterio 6).

- [ ] **Step 6: Commit**

```bash
git add main/ui/ui_state.h main/ui.c
git commit -m "feat(ui): indicador de excedente solar activo en la vista principal

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:**
- Disparador SoC+PV con histéresis → Task 2 (eval) + Task 3 (config).
- Interlock 230V vía NE185 → Task 2 (`shore` en preconds) + Task 4 (feed de `ne185.shore`).
- Fail-safe sin datos → Task 2 (`fresh`) + Task 3 (frescura del feed) + Task 4 (`bat_fresh && solar_fresh && ne.fresh`).
- Suelo de seguridad inmediato → Task 2 (`preconds_ok`).
- Bloque mínimo/anti-castañeteo → Task 2 (`MIN_ON`, `ACT_DEBOUNCE`).
- Relé GPIO1 + arranque OFF + diodo (HW) → Task 3 (init) + Global Constraints.
- Toggle + SoC ajustables en menú frigo → Task 5.
- Indicador vista principal → Task 6.
- Persistencia NVS → Task 3 (load/save) + verificación Task 5 Step 5.
- Prerrequisito PZEM → Task 1.
- Prueba por inyección sin sol/Victron/230V → Task 2 (host) + Task 4 Step 6 (banco).

**Placeholder scan:** El código novel (Tasks 1-4) está completo. Las Tasks 5-6 (UI LVGL) dan el código de callbacks/labels/setters exacto y remiten al patrón `fanmin`/barra inferior existente para el andamiaje de filas/estilos (la única parte que depende de helpers concretos del fichero); es intencional en código existente, no un placeholder de lógica.

**Type consistency:** Nombres y firmas de `frigo_solar_*` coinciden entre Task 3 (definición), Task 4 (`frigo_solar_feed`), Task 5 (setters/getters) y Task 6 (`frigo_solar_get_active`). `dashboard_snapshot_t` gana `pv_w/solar_has/bat_fresh/solar_fresh` en Task 4 y se consumen en el mismo Task. `ne185_data_t.{shore,fresh}` ya existen en `ne185.h`.
