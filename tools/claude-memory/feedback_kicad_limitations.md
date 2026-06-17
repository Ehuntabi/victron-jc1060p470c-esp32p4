---
name: feedback-kicad-limitations
description: Generar PCB completo via S-expressions a mano es fragil. kiutils Python lib es viable para schematic con wires/labels. PCB routing es trabajo GUI ineludible
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

Generar archivos KiCad sin abrir la GUI tiene limitaciones:

**Lo que SI funciona programáticamente** (con `kiutils` pip):
- Crear `.kicad_pro` (JSON config)
- Crear `.kicad_sch` con lib_symbols embebidos, posicionar componentes, anadir labels de net (KiCad considera pins conectados si tienen el mismo label)
- Generar PDF/SVG del schematic via `kicad-cli sch export pdf/svg`
- Anadir wires/junctions con `Connection` y `Junction` items
- Generar BOM CSV

**Lo que NO funciona bien sin GUI**:
- PCB layout con routing automatico (autoplacer + autorouter de KiCad solo via GUI)
- Update PCB from Schematic (Tools menu, solo GUI)
- Verificacion DRC visual (necesita placement manual primero)
- Generacion correcta de `.kicad_pcb` a mano (formato muy fragil con UUIDs, layers, footprint refs)

**Workflow recomendado para PCB:**
1. Yo genero schematic completo con kiutils + lib_symbols embebidos + labels conectados
2. User abre KiCad GUI: File -> Open Project
3. Eeschema: ERC (validar conexiones)
4. Eeschema: Tools -> Update PCB from Schematic (F8). Genera el .kicad_pcb con footprints colocados en pila
5. Pcbnew: drag&drop footprints, autoroute o route manual
6. File -> Plot -> gerbers/

**Why:** El 2026-05-26 intente disenar una PCB NE185 adapter desde cero generando .kicad_sch y .kicad_pcb a mano con S-expressions. El schematic salio razonable pero sin wires (41 errores ERC "pin not connected"). El PCB no abria por errores sintacticos. Tras instalar kiutils consegui pasar a 2 errores ERC reales + 13 warnings (footprints Flatpak path), pero el PCB final requiere GUI.

**How to apply:**
- Si user pide "diseña PCB en KiCad": primero plantear que el schematic se entrega completo (kiutils) pero el layout PCB requiere GUI. NO prometer PCB renderizado completo sin GUI
- kiutils 1.4.8 funciona en Python 3.12 con `pip install kiutils`. Modulos clave: `kiutils.schematic.Schematic`, `kiutils.items.schitems` (Connection, Junction, LocalLabel, NoConnect)
- Para validar: `flatpak run --command=kicad-cli org.kicad.KiCad sch erc <file>`
- Para PDF: `flatpak run --command=kicad-cli org.kicad.KiCad sch export pdf <file>`
- Alternativas mejores si user necesita PCB completo: SKiDL (define circuito en Python -> netlist -> KiCad import) o sugerir abrir KiCad GUI y guiar paso a paso
