# Modo "Aprovechar excedente solar" para el frigorífico trivalente

Fecha: 2026-07-17 · Rama: `feat/frigo-excedente-solar` · Proyecto: Joint SPL 145 (ESP32-P4)

## Objetivo

Los días de sol, cuando la batería está llena y sobra producción solar, conmutar
automáticamente el frigorífico trivalente a **12V** para **ahorrar gas**, sin
comprometer nunca la batería. Es una función **extra** (no sustituye al gas): si
las condiciones no son claramente favorables, el frigo sigue en gas/230V como
siempre.

Reutiliza el hardware existente: el mismo relé "tocho" que hoy alimenta el frigo
a 12V en marcha (disparado por el **D+** del vehículo). El P4 añade un segundo
mando a ese relé mediante un **relé piloto** que inyecta 13V a su bobina.

## Contexto de hardware del usuario

- Batería **litio 150 Ah** (colchón grande, tolera ciclado).
- Placa solar **150 W** hoy → se cambiará por **250 W**.
- Consumo del frigo en 12V: **desconocido** (típico trivalente ~100-130 W / ~8-11 A).

**Consecuencia de diseño asumida y aceptada por el usuario:** con la placa de
150 W apenas habrá excedente real (150 W ≈ lo que gasta el frigo), así que el
modo se activará poco; con 250 W (~180-200 W reales) sí sobran ~70-90 W y
funciona como se pretende. La lógica es idéntica en ambos casos; solo cambia
cuánto tiempo aguanta activo. **No es necesario conocer el consumo exacto del
frigo:** la seguridad la garantiza el **suelo de SoC**, no la medición de carga.

## Prerrequisito: eliminación del PZEM-004T

El usuario no instalará el medidor AC PZEM-004T. Su eliminación **libera GPIO1,
GPIO2 y UART_NUM_2**, y GPIO1/GPIO2 **sí están enrutados al conector JP1**, por
lo que GPIO1 será el pin del relé piloto. La eliminación del PZEM se hace como
**tarea/commit aparte** antes de la feature (footprint completo ya inventariado;
único punto de atención: el enum `WD_TASK_PZEM` reindexará `WD_TASK_COUNT`).

## Decisiones de diseño confirmadas

| Decisión | Elección |
|---|---|
| Disparador | Batería llena **Y** sol suficiente (SoC + PV con histéresis) |
| Interlock 230V | Bloquear el modo si hay red (`shore`), leído del **NE185** (sin hardware extra) |
| UI | Entrada en el menú **frigo**: toggle ON/OFF del modo |
| Ajustes | Toggle + **SoC de activación y suelo ajustables** (+/−); PV mínima y tiempo de bloque fijos en código |
| Pin del relé piloto | **GPIO1** (liberado del PZEM, en JP1) |

## Interlock de 230V (sin hardware extra)

Si el frigo funciona con **230V** (camping enchufado), meter 12V no tiene
sentido: cambiaría red gratis/eficiente por la resistencia de 12V, la más cara,
calentando doble. El modo solar **solo debe actuar sin red**.

El P4 **ya conoce** este estado: el componente `ne185` expone
`ne185_data_t.shore` ("conectado a red 230 V") y `ne185_data_t.fresh`, leídos por
RS-485 del derivador Nordelettronica. **No hace falta PZEM, detector AC ni señal
Victron de AC.** Regla: si `shore == true` (o el estado NE185 no es fiable) →
el modo solar **no activa / desactiva**.

## Arquitectura

La lógica vive **dentro del componente `frigo`** (`components/frigo/`), por
cohesión: ya contiene el control del frigorífico (ventilador, sondas), su propio
namespace NVS `"frigo"`, su tarea `frigo_task`, y su panel de UI
(`main/ui/frigo_panel.c`). El componente **no depende de `main/`**; la
telemetría Victron (SoC/PV) se le **inyecta desde main** mediante un setter,
igual que el patrón existente `frigo_sim_inject()` y los callbacks.

```
Victron BLE ─► dashboard_state (main/) ─┐  SoC, pv_w
                                        │
NE185 RS-485 ─► ne185_get() (main/) ────┤  shore (230V), fresh
                                        ▼
              main empuja hacia abajo (timer ~1 s):
              frigo_solar_feed(soc_deci, pv_w, shore, fresh)
                                        │
   componente frigo ─► máquina de estados (frigo_task) ─► gpio_set_level(GPIO1)
        │                                                     │
        └── NVS "frigo": sol_en, sol_on, sol_off      relé piloto ► bobina relé tocho ► frigo 12V
        └── frigo_panel.c: toggle + ajustes +/−
```

### Unidades y responsabilidades

- **`frigo` (componente):** dueño del estado del modo, los umbrales NVS, la
  máquina de estados y la salida GPIO1. Interfaz pública nueva:
  - `void frigo_solar_feed(uint16_t soc_deci, uint16_t pv_w, bool shore, bool fresh)`
    — main empuja telemetría periódicamente. `shore=true` si hay 230V.
    `fresh=false` cuando falta dato reciente de Victron **o** del NE185 (cualquiera
    de los dos caído invalida la evaluación → modo OFF).
  - `esp_err_t frigo_solar_set_enabled(bool on)` — toggle maestro (persiste NVS).
  - `esp_err_t frigo_solar_set_soc_on(uint8_t pct)` — SoC de activación (persiste).
  - `esp_err_t frigo_solar_set_soc_off(uint8_t pct)` — SoC de corte/suelo (persiste).
  - Getters para la UI: `frigo_solar_get_active()` (bool, estado ON real, para el
    indicador de la vista principal) y getters de config (enabled, soc_on,
    soc_off, motivo actual) para el panel de ajustes.
- **`main`:** un timer ~1 s lee `dashboard_state_snapshot()` (SoC/PV) y
  `ne185_get()` (shore + fresh), combina el `fresh` de ambos buses y llama a
  `frigo_solar_feed(...)`. Requiere **exponer `pv_w` en `dashboard_snapshot_t`**
  (hoy no está; añadir campo y copiarlo en `dashboard_state_snapshot()`).
- **`frigo_panel.c` (UI de ajustes):** añade el switch ON/OFF y dos ajustes +/−
  (soc_on, soc_off), al estilo de los controles de `fanmin` ya existentes; se
  registra en la página del frigo de `settings_panel.c`.
- **`ui.c` (vista principal):** **indicador visual** que se enciende cuando el
  frigo está tirando de excedente solar (estado ON real). Va en la barra inferior,
  reaprovechando el hueco/estilo del indicador "AC" del PZEM que se retira. Lee el
  getter `frigo_solar_get_active()` (refrescado por timer o por el callback de
  actualización del frigo). Apagado/oculto cuando el modo no está activo.

## Máquina de estados (evaluada en `frigo_task`, ~1 s)

Dos estados: **OFF** (P4 no energiza el piloto; el relé tocho solo responde al
D+, como hoy) y **ON** (P4 energiza el piloto → frigo a 12V).

Precondiciones para poder estar en ON (si cualquiera falla → OFF **inmediato**,
sin esperar bloque):
- modo `enabled == true`, y
- `fresh == true` (telemetría Victron **y** NE185 recientes), y
- `shore == false` (no hay 230V; ver interlock), y
- `soc >= soc_off` (suelo no violado).

Transiciones:
- **OFF → ON** cuando: `soc >= soc_on` **Y** `pv_w >= PV_MIN`, sostenido durante
  `ACT_DEBOUNCE` (evita activar por un golpe de sol breve).
- **ON → OFF inmediato (seguridad)** cuando: `enabled == false`, o `fresh ==
  false`, o `shore == true` (aparece 230V), o `soc < soc_off`. El suelo de SoC y
  la aparición de red **nunca** esperan al temporizador de bloque.
- **ON → OFF por pérdida de sol** (`pv_w < PV_MIN`) solo tras cumplir `MIN_ON`
  (aguanta nubes pasajeras; el colchón de litio y el suelo de SoC protegen).

Notas:
- Una vez en ON, se compromete a `MIN_ON` salvo disparo de seguridad. En litio
  150 Ah, 30 min a ~120 W ≈ 60 Wh ≈ ~3% de SoC: muy lejos del suelo, así que
  comprometer el bloque es seguro.
- Convivencia con D+: si el vehículo activa el relé por D+, que el P4 también lo
  pilote es **inofensivo** (redundante). El P4 nunca desactiva lo que hace el D+.

### Constantes fijas en código (valores iniciales)

| Constante | Valor | Motivo |
|---|---|---|
| `PV_MIN` | 80 W | Garantiza día con sol real, no amanecer/batería-llena-de-noche |
| `MIN_ON` | 30 min | Estilo Votronic AES; evita castañeteo del relé y ciclado |
| `ACT_DEBOUNCE` | 60 s | Condiciones sostenidas antes de activar |
| `FRESH_TIMEOUT` | 30 s | Sin telemetría Victron más reciente ⇒ `fresh=false` |

### Defaults ajustables (NVS namespace `"frigo"`)

| Clave | Default | Rango |
|---|---|---|
| `sol_en` (u8, bool) | 0 (OFF) | 0/1 |
| `sol_on` (u8, %) | 95 | p.ej. 80..100 paso 1 |
| `sol_off` (u8, %) | 80 | p.ej. 50..(soc_on−5) |

Validación: `sol_off < sol_on` con margen mínimo (p.ej. 5 puntos) para garantizar
histéresis.

## Hardware

`GPIO1` (JP1, salida digital) → base/entrada del **relé piloto** → sus contactos
inyectan **13V a la bobina del relé tocho**. Un **diodo** en el punto de
inyección impide que esos 13V retroalimenten la línea D+ del vehículo (backfeed).
Al arranque el GPIO se configura en **nivel bajo (relé OFF)** antes de cualquier
lógica.

## Estados de fallo / arranque

- **Boot:** GPIO1 = 0 (OFF) siempre. `sol_en` se carga de NVS (default OFF).
- **Sin datos Victron o NE185** (BLE o RS-485 caídos, o al arrancar antes de la
  primera trama): `fresh=false` ⇒ OFF. Si no se puede confirmar el estado de red
  del NE185, se asume el caso conservador (no activar).
- **Aparece 230V** estando ON (`shore` pasa a true): OFF inmediato.
- **Toggle OFF:** el P4 no toca GPIO1 jamás; comportamiento idéntico al actual.
- **Watchdog:** `frigo_task` ya reporta heartbeat; la lógica corre en esa tarea,
  sin tareas nuevas.

## Criterios de éxito (verificables)

1. **Modo OFF:** con `sol_en=0`, el P4 nunca pone GPIO1 a 1 (verificable por log
   y por medida en el pin). Comportamiento del frigo idéntico a hoy.
2. **Activación:** con `sol_en=1`, inyectando SoC=96% y PV=120W sostenidos,
   GPIO1 pasa a 1 tras `ACT_DEBOUNCE`; la UI muestra el modo activo.
3. **Suelo de seguridad:** estando ON, al inyectar SoC=79% (o `fresh=false`),
   GPIO1 vuelve a 0 **de inmediato**, sin esperar `MIN_ON`.
4. **Interlock 230V:** estando ON, al inyectar `shore=true` (NE185), GPIO1 vuelve
   a 0 de inmediato; y con `shore=true` el modo nunca activa aunque SoC/PV sean
   favorables.
5. **Anti-castañeteo:** con PV oscilando alrededor de `PV_MIN` estando ON, el
   relé no conmuta más de una vez por `MIN_ON` (salvo disparo de seguridad).
6. **Indicador vista principal:** cuando el modo está ON (real), la barra
   inferior muestra el indicador de excedente solar; se apaga/oculta al pasar a
   OFF.
7. **Persistencia:** `sol_en`, `sol_on`, `sol_off` sobreviven a reset.

### Estrategia de prueba

`frigo_solar_feed()` es el punto de inyección: una ruta de simulación (análoga a
`frigo_sim_inject()` y `ne185_sim_inject(...,shore)`) permite alimentar SoC/PV y
el flag `shore` falsos desde consola/UI y observar GPIO1 por log, **sin necesidad
de sol real, Victron ni 230V**. Esto cubre los criterios 1–5 en banco. El
criterio 6 se prueba con reset.

## Fuera de alcance (YAGNI)

- No se mide el consumo real del frigo (el suelo de SoC hace innecesario medirlo).
- No se usa el estado de "flotación" del MPPT (descartado: dato no confirmado en
  la trama BLE; SoC+PV cubre el caso).
- No se expone la feature por la web httpd en esta iteración (solo pantalla LVGL);
  puede añadirse después con el patrón de `config_server.c` si se desea.
- PV mínima y tiempo de bloque no se hacen ajustables por UI (fijos en código).

## Plan de commits

1. **Commit A — Eliminar PZEM-004T** (prerrequisito, quirúrgico): borra
   `main/pzem004t.{c,h}`, init en `main.c`, `WD_TASK_PZEM` (reindexa
   `WD_TASK_COUNT`), objeto JSON `"ac"` en `dashboard_state.c`, indicador AC en
   `ui.c`/`ui_state.h`, HTML/JS en `config_server.c`; scripts/docs opcionales.
   Compila y arranca limpio.
2. **Commit B — Modo excedente solar:** GPIO1 + máquina de estados + NVS +
   inyección de telemetría desde main + `pv_w` en snapshot + UI en frigo_panel.
