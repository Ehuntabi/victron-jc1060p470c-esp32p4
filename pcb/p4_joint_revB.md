# p4_joint rev B — placa de expansión del P4 (ampliable)

Rediseño de la placa del ventilador del frigo → **placa de expansión** con canales de sobra.
El pedido de la rev A (2264409A) se anuló por el footprint de D2. La rev B añade capacidad y arregla D2.

## Bloques
| Bloque | GPIO / JP1 pin | Circuito |
|---|---|---|
| **PWM 1** (ventilador frigo) | GPIO5 / pin 15 | MOSFET lado bajo + flyback + borna |
| **PWM 2** (nuevo, universal) | GPIO3 / pin 11 | idéntico → 2º ventilador o tira LED |
| **Relé 1** (excedente solar) | GPIO1 / pin 7 | driver + relé JS + COM/NO |
| **Relé 2** (nuevo) | GPIO2 / pin 9 | idéntico |
| **Entrada analógica** (nueva) | GPIO20 / pin 17 | divisor configurable + filtro + clamp → ADC |
| **DS18B20** | GPIO4 / pin 13 | 1-Wire, pullup 4.7k |
| **Alim** | 12V (pin 2/4), GND (5/6/16), 3V3 (1/3/18) | del header JP1 |

GPIO libres para futuro: 32, 45, 46.

## Diseño por bloque

### PWM ×2 (universales — valen para ventilador o tira LED 12V)
Cada canal (idéntico al ventilador ya probado):
- **Q (MOSFET N lado bajo)**: **LR7843** (TO-252, logic-level 30V, LCSC C9900160403). Aguanta ventilador
  (~300 mA) o tira LED (~60 mA). *Alt. compacta: AO3400 SOT-23 (C20917) si quieres ahorrar espacio.*
- Puerta ← GPIO por **R serie 150 Ω**; **R pulldown 10 k** (C25804) puerta→GND (apagado en boot).
- **D flyback SS14** (SMA, C2480) sobre la carga (cátodo→+12V). Necesario para ventilador; con LEDs no
  conduce (inofensivo) → por eso el canal es universal.
- **Borna de salida** (KK-156) → negativo de la carga; positivo de la carga → +12V.
- Firmware: LEDC (como el frigo). Ventilador 18 kHz; LED ~1 kHz vale.

### Relé ×2
Cada canal (idéntico al del excedente solar):
- **Q driver = 2N7002** (SOT-23, C8545). GPIO → R serie → puerta; R5 10k pulldown; source→GND;
  drain→bobina del relé.
- **Relé K = Panasonic JS1aPF-B-12V** (1 Form A; bobina 12V/400Ω/30mA; contactos 10A). THT, a mano.
  Pinout (no estándar): bobina 2/5, **COM 1** (entre bobina), NO 3. Footprint verificado del PC board pattern.
- **D flyback SS14** sobre la bobina (cátodo→+12V).
- **Bornas**: COM(pin1) y NO(pin3) a borna de salida → carga del relé (p.ej. bobina relé tocho / D+).
- Relé 1: COM→+12V del rail; interlock 230V (shore) lo hace el firmware vía NE185.

### Entrada analógica (1 footprint, 2 modos por poblado)
```
 borna ─[R1]─┬─[R3 1k]─ GPIO20 (ADC)
             ├─[R2]─ GND         (divisor)
             ├─[C 100nF]─ GND    (filtro)
             └─[clamp BAT54→3V3] (protección del ADC)
```
- **Modo 13-14 V** (tensión batería/rail): R1=**100k**, R2=**22k** → ×0.18 → 14V≈2.5V ADC, margen hasta
  ~18V. Consumo 0.1 mA (no drena batería).
- **Modo sensor 3.3 V**: R1=**0 Ω**, R2=**sin poblar** → directo al ADC.
- **Clamp BAT54** (SOT-23) nodo→3V3: protege el pin del ADC pase lo que pase. **C** 100nF (C14663) filtro.

### Fix D2 (flyback del ventilador PWM 1)
El problema de la rev A era el footprint. Cambiar D2 a **SS14 en SMA/DO-214AC** (C2480), pads grandes.
Igual para todos los flyback nuevos (D3, D4, D5…) → un solo diodo en el BOM.

### U1 (MOSFET ventilador existente)
→ **LR7843** (C9900160403), TTO-252 (mismo footprint). Asignar LCSC.

## BOM / LCSC (resumen)
| Parte | Uso | Footprint | LCSC |
|---|---|---|---|
| LR7843 | Q PWM ×2 (U1 + PWM2) | TO-252 | C9900160403 |
| 2N7002 | driver relé ×2 | SOT-23 | C8545 |
| JS1aPF-B-12V | relé ×2 | THT (a mano) | — |
| SS14 | flyback (todos) | SMA | C2480 |
| BAT54 | clamp entrada | SOT-23 | (asignar) |
| R 150Ω / 10k / 100k / 22k / 1k / 4.7k | varios | 0603 | (10k = C25804) |
| C 100nF / 100µF | filtro / bulk | 0603 / SMD | 100nF C14663; 100µF C4747964 |
| C2 100µF 25V | bulk (existía sin LCSC) | 6.3×5.4 | C4747964 |
| KK-156 | bornas salida | THT | — |

## A tener en cuenta
- La placa **crece** (2 relés JS de 22×16 + 2 PWM + entrada) → PCB más grande y cara que la rev A.
- **Masa común**: si alimentas alguna carga de otra fuente distinta al rail 12V del P4, unir masas
  (gotcha del ventilador: el PWM no varía sin masa común).
- Editar/rutar = **EAGLE GUI (Wine)**. Librería `p4_joint_add.lbr` (SMA + relé JS ya hechos); falta añadir
  el BAT54 (SOT-23 estándar) si no está. Script `p4_joint_add.scr` (ampliar para los canales duplicados).
- Cierre: DRC → colocar/rutar → DFM de JLCPCB → regenerar Gerbers/BOM/CPL → re-pedir.
