# p4_joint rev B — añadir relé excedente solar + fix D2

Cambios para el RE-PEDIDO (el pedido anterior 2264409A se anuló por el footprint de D2).

## 1. Fix del diodo del ventilador (D2)

**Problema:** JLCPCB avisó de que los pines de D2 no casaban con los pads (footprint incorrecto,
área de soldadura pequeña → riesgo de mala conexión). En el `.sch` estaba como device
`1N4148W-7-F` con footprint `SOD123-M`, pero el BOM pedía 1N5819 sin LCSC.

**Fix:** sustituir D2 por **SS14** (Schottky 1 A / 40 V) en **SMA / DO-214AC**, LCSC **C2480**
(parte básica JLCPCB). Pads grandes → sin riesgo de soldadura débil. Misma función (flyback del
ventilador): cátodo → +12 V, ánodo → nodo drain del MOSFET del ventilador.

## 2. Relé excedente solar (nuevo)

Relé piloto **Panasonic JS1aPF-B-12V** (1 Form A). Datasheet:
- Bobina 12 V: **400 Ω**, **30 mA**, 360 mW. Pick-up 8.4 V, drop-out 1.2 V.
- Contactos: 1 Form A (NA), 10 A AC / 5 A DC. Patillaje: bobina **1-2**, COM **5**, NA **3**.

### Driver (low-side, control desde GPIO1 del P4)
```
   +12V ──┬───────────────┐
          │             ▲ cátodo
      [bobina K1 1-2]    D3 SS14 (flyback)
          │(pin 2)       │ ánodo
                         │
 GPIO1 ─[R6 100Ω]─ gate  │
   Q1 2N7002 (SOT-23)    │
   drain ────────────────┴──> pin 2 de la bobina
   source ── GND
   R5 10k: gate → GND (pulldown: relé OFF en boot/GPIO flotante)
```
### Contactos (inyectan 12 V a la bobina del relé tocho / D+)
- COM (pin 5) ── **+12 V** (rail de la placa, el mismo de DC_IN).
- NA (pin 3) ── borna de salida nueva **RELE_OUT** → bobina del relé tocho.
- (RELE_OUT 2 pines: 12V-conmutado + GND, por si el tocho necesita retorno en la placa.)

## 3. BOM — componentes nuevos y LCSC a asignar

| Ref | Valor | Footprint | LCSC | Montaje |
|-----|-------|-----------|------|---------|
| Q1  | 2N7002 N-MOSFET lógico | SOT-23 | C8545 (básica) | SMD (JLCPCB) |
| D3  | SS14 Schottky 1A/40V | SMA/DO-214AC | C2480 (básica) | SMD (JLCPCB) |
| R5  | 10 kΩ | 0603 | C25804 (= R4) | SMD (JLCPCB) |
| R6  | 100 Ω (opcional) | 0603 | (asignar, p.ej. C22775) | SMD (JLCPCB) |
| K1  | JS1aPF-B-12V | JS 1a PCB (THT) | — | A MANO (THT) |
| RELE_OUT | borna 2P | KK-156 / bornero | — | A MANO (THT) |

**LCSC pendientes también en piezas existentes (el pedido anterior no las llevaba) — VERIFICADO 19-jul:**
- **D2** → SS14 / **C2480** (básica, en stock). Ver §1.
- **C2** 100 µF 25 V → **C4747964** (RST100UF25V004, SMD 6.3×5.4 mm, casa el footprint actual).
- **U1** → **LR7843 (C9900160403)** [DECIDIDO 19-jul]. Pieza de la casa JLCPCB, TO-252 logic-level
  30V, clon del IRLR7843, stockeada/barata. Mismo footprint TO-252, sin cambio de layout.

## 4. Entrada GPIO1
La señal GPIO1 del P4 (3.3 V) entra por un pin de header (revisar en EAGLE qué JP libre usar o
añadir pin). Interlock 230 V (shore) lo hace el firmware vía NE185 — no requiere hardware extra.

## 5. Implementación en EAGLE (GUI/Wine)

Ficheros: `p4_joint_add.lbr` (footprints nuevos) + `p4_joint_add.scr` (añade piezas + U1).
Mapeo de footprints: **Q1** = `zetex/NMOSSOT23` (verificado) · **U1** = HEXFET TO-252 actual ·
**D2/D3** = `SMA` de `p4_joint_add.lbr` · **K1** = `JS_RELAY_1A` (⚠️ cotas provisionales, medir).

**Pasos:**
1. Abrir `p4_joint.sch` en EAGLE.
2. `File > Execute Script` → `p4_joint_add.scr` (ajusta la ruta del USE). Añade Q1/D3/R5/R6/K1/
   RELE_OUT y cambia U1→LR7843. Si algún ADD falla por nombre de device, usar autocompletado.
3. **Fix D2:** botón derecho sobre D2 → **Replace** → elegir **SS14** de `p4_joint_add.lbr`
   (mantiene las conexiones; cambia footprint SOD-123→SMA). Poner VALUE=SS14, LCSC=C2480.
4. **Asignar LCSC** a **C2** = `C4747964` (100 µF 25 V 6.3×5.4).
5. **Cablear** (lista abajo).
6. Ajustar en la librería las **cotas del footprint del relé** (medir el relé / datasheet).
7. `DRC` (0 errores) → colocar y **rutar** en el .brd → **DFM** de JLCPCB → regenerar
   Gerbers/BOM/CPL (CAM) → re-pedir.

### Lista de cableado (nets a conectar en el esquema)
| # | Desde | Hasta | Net |
|---|-------|-------|-----|
| 1 | GPIO1 (pin libre de JP2–JP5) | R6.1 | GPIO1 |
| 2 | R6.2 | Q1.G | — |
| 3 | Q1.G | R5 → **GND** | (R5 entre puerta y GND) |
| 4 | Q1.S | **GND** | GND |
| 5 | Q1.D | K1 bobina(pin 5) **y** D3.A | (nodo bobina−) |
| 6 | K1 bobina(pin 2) | **+12V** | 12V |
| 7 | D3.C (cátodo) | **+12V** | 12V (flyback) |
| 8 | K1 COM(pin 1) | **+12V** | 12V |
| 9 | K1 NO(3) | RELE_OUT.1 | salida al relé tocho |
| 10 | RELE_OUT.2 | **GND** | retorno bobina tocho |

Nota: GPIO1 entra por un pin de header (elige un JP2–JP5 libre y etiqueta el net `GPIO1`).
El interlock 230 V (shore) lo hace el firmware vía NE185 — sin hardware extra.
