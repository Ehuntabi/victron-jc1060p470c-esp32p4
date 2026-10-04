# Guía de estilo de la UI (P4 · LVGL 8.4 · 1024×600)

**Decidida el 4-oct-2026** con el usuario, tras medir el estado real de las 24
pantallas con `/captura?n=<i>` (captura LVGL + navegación) y auditar el código.
Alcance: **P4 primero**; sirve de referencia para la app Flutter y la cabina.

Regla de oro: **una guía que no se audita se abandona**. Todo lo de aquí que se
pueda comprobar con un `grep` está en `test/auditar.sh` (sección 12).

---

## 1. Tipografía — UNA familia, CINCO papeles

Familia única: **Inter** (regular + semibold), generada con rango ASCII + acentos
españoles + `ñ¿¡°` (`main/fonts/fonts_es.h`).

| Papel | Fuente | Uso |
|---|---|---|
| Cifra grande | `UI_FONT_DISPLAY` (46) | número protagonista de una tarjeta (temperatura, vatios) |
| Título de pantalla | `UI_FONT_TITLE` (28) | el título de la cabecera |
| Valor | `UI_FONT_VALUE` (24 / semibold 24) | datos de tarjeta, títulos de tarjeta |
| Texto | `UI_FONT_TEXT` (20 / semibold 20) | etiquetas, filas de ajustes |
| Pie | `UI_FONT_SMALL` (14) | unidades, notas, "no hay dato" |

Reglas:

1. **Prohibido `lv_font_montserrat_*` en el código de UI**: siempre `UI_FONT_*`.
   Ojo, que aquí me equivoqué al auditar: `fonts_es.h` **ya aliasaba** los nombres
   sin `_es` a Inter, así que los 41 usos que encontré **no** eran un bug visible
   (las capturas antes/después salen idénticas). El motivo real de la regla es
   otro: (a) el alias es una **trampa silenciosa** — un fichero que use el nombre
   plano sin incluir `fonts_es.h` recibe la fuente del SDK, que solo tiene ASCII;
   (b) `lv_font_montserrat_36` **no** estaba aliasado y mantenía 56 KB de fuente
   compilada para dos estilos que no usaba nadie (al migrar, el binario adelgazó
   eso); (c) en el código se lee el papel ("valor", "título") y no el fichero.
2. Un papel por función, no por tamaño disponible: si algo "no cabe", se acorta
   el texto, no se baja de papel.
3. Los nombres `lv_font_montserrat_*_es` siguen valiendo (son alias de Inter) pero
   **no se añaden usos nuevos**: al tocar un fichero se pasa a `UI_FONT_*`.

## 2. Color — paleta CERRADA

Los tokens viven en `main/ui/widgets/ui_style.h` (una sola fuente de verdad).

| Token | Valor | Para qué |
|---|---|---|
| `UI_COLOR_BG` | `#0A0D13` | fondo de página |
| `UI_COLOR_CARD` | `#1B2230` | superficie de tarjeta |
| `UI_COLOR_CARD_BORDER` | `#39424F` | borde de tarjeta |
| `UI_COLOR_TEXT` | `#FFFFFF` | dato principal |
| `UI_COLOR_TEXT_SOFT` | `#E4E9F0` | texto descriptivo |
| `UI_COLOR_TEXT_DIM` | `#8A93A6` | **solo** "no hay dato" (vacío, `--`) |
| `UI_COLOR_CYAN` | `#4FC3F7` | información / solar |
| `UI_COLOR_GREEN` | `#00C851` | correcto |
| `UI_COLOR_YELLOW` | `#FFD54F` | atención |
| `UI_COLOR_ORANGE` | `#FF9800` | aviso |
| `UI_COLOR_RED` | `#FF4444` | error |
| `UI_COLOR_RED_DARK` | `#CC3333` | error atenuado / fondo de aviso |
| `UI_COLOR_VIOLET` | `#B388FF` | zona habitable (camper) |
| `UI_COLOR_ICE` | `#64B5F6` | frío (congelador) |

Reglas:

1. **Cero `lv_color_hex()` fuera de `ui_style.h`.** Si falta un color, se añade
   un token (con nombre y motivo), no un literal. **Aplicado el 4-oct-2026**: los
   **59 colores distintos / 203 literales** que había repartidos en 18 ficheros se
   colapsaron a los 14 tokens (el único fichero con `lv_color_hex` es ahora
   `ui_style.h`), y la auditoría falla si vuelve a aparecer uno.
2. **Un solo tono por significado.** Tonos que se colapsaron (medidos en pantalla
   antes del cambio):

   | significado | tonos que había | queda |
   |---|---|---|
   | naranja (aviso, **y acento de diálogo**) | `FF9800`, `FF9A00`, `FFAA00`, `FFBB33`, `FFA726`, `E0900A`, `FF7043`, `FF7142`, `F57C00`, `E91E63` | `FF9800` |
   | verde (correcto) | `00C851`, `00CA52`, `4CD964`, `2E7D32`, `297D31`, `00E676`, `44FF44` | `00C851` |
   | cian (información/solar) | `4FC3F7`, `4AC2F7`, `29B6F6`, `00BFFF`, `42A5F5`, `26C6DA`, `008AD6`, `10456B`, `0288D1`, `1565C0`, `00897B` | `4FC3F7` |
   | rojo (error) | `FF4444`, `F44336`, `B51C19`, `882222`, `8C2021`, `B71C1C`, `801010` | `FF4444` / `CC3333` |
   | violeta (camper/habitable) | `B388FF`, `9C27B0`, `BA68C8`, `BB66FF` | `B388FF` |
   | superficies | `2A2A30`, `2E2E36`, `2A3340`, `2D3340`, `292831`, `313131`, `424542`, `4A4A55`, `4A5568`, `70707C`… | `CARD` |
   | bordes | `333333`, `444444`, `555555`, `37474F`, `5D4037` | `CARD_BORDER` |
   | texto secundario | `666666`, `888888`, `AAAAAA`, `9E9E9E`, `90A4AE`, `B0BEC5`, `BBBBBB`, `CCCCCC`, `DDDDDD`… | `TEXT_DIM` |
   | fondos (página, modales, gráficas) | `000000`, `0A0A0A`, `06080C`, `0A1018`, `0A1620`, `121212`… | `BG` |

   El rosa `E91E63` era el acento de los **diálogos de aviso** ("¿Borrar
   carpeta?", "Atención"): pasa a `UI_COLOR_ORANGE` (aviso) y así queda la regla.
3. **Acento por sección como máximo**: una pantalla puede tener un color de
   sección; el resto son de **estado** (bien/atención/mal). En Ajustes todavía
   conviven varios acentos a la vez (cada entrada del menú tiene el suyo): es lo
   siguiente que hay que reducir.

## 3. Medidas — rejilla de 4 px

| Token | Valor | Uso |
|---|---|---|
| `UI_PAD_*` | 4 / 8 / 12 / 16 / 20 / 24 | nada fuera de esta escala |
| `UI_PAD_CARD` | 20 | relleno interior de tarjeta |
| `UI_GAP_CARD` | 16 | separación entre tarjetas |
| `UI_PAD_PAGE` | 12 | margen lateral de página |
| `UI_RADIUS_CARD` | 16 | tarjeta |
| `UI_RADIUS_CTRL` | 8 | botón, chip, control |
| `UI_RADIUS_TAG` | 4 | etiqueta pequeña, LED |
| `UI_ROW_H` | 44 | alto de fila de ajuste y de botón |
| `UI_HEADER_H` | 48 | alto de cabecera de pantalla, y botón grande |
| `UI_TAB_H` | 56 | alto de fila de menú / pestaña |
| `UI_SLIDER_H` | 24 | alto de un slider (volumen, brillo) |
| `UI_BAR_H` | 48 | alto reservado para la barra inferior (igual en las dos pestañas) |

Reglas: nada de 2, 3, 6, 10, 14 px sueltos; los márgenes laterales son los
mismos en vistas y en ajustes (ya unificado en v3.6, se mantiene); los **altos**
escritos a mano también son múltiplos de 4 (salvo `0` = oculto y `1` = línea
separadora, que son estructurales). Aplicado el 4-oct-2026 (v4.21): había 26
(sliders), 38, 42, 46, 50, 54, 58 y 90 → `24`, `40`, `44` (`UI_ROW_H`), `48`
(`UI_HEADER_H`), `56` (`UI_TAB_H`), `88`.

**La banda superior, medida y corregida el 4-oct-2026 (v4.19/v4.20).** El punto 8
del briefing del 22-sep decía "en Ajustes el contenido empieza en y=103 y en las
vistas en y=23-31". Medido pantalla a pantalla con `/captura`, eso NO es un fallo
de padding: son **dos familias con cabecera distinta a propósito**:

| familia | cabecera | primera tarjeta |
|---|---|---|
| **vistas** (overview, batería, solar, DC/DC, detalle…) | ninguna: el título va **dentro** de cada tarjeta | y=26-34 |
| **Ajustes** (wifi, pantalla, tarjeta SD, sonido, GPS, about…) | miga de pan ("‹ Ajustes") + título centrado | y=84 (el aviso del GPS, que es banner y no tarjeta, en 72) |
| **históricos** (log_*, logs) | barra de herramientas con Hoy/Semana y Cerrar | y=112-185 |

Lo que sí era incoherencia **dentro** de la familia de Ajustes: cada página ponía
su propio relleno (12, 8 y 4), así que el contenido empezaba en 80, 84 o 72. Ahora
todas usan `UI_PAD_PAGE` y las cinco páginas de tarjeta empiezan **exactamente en
84**. Regla: **la banda es la misma dentro de cada familia**; entre familias no se
iguala, porque la cabecera de Ajustes (saber dónde estás y cómo volver) es
información, no adorno.

**Aplicado el 4-oct-2026 (v4.19)**: había **93 usos fuera de la rejilla** de 328
(39 de valor 10, 20 de 6, 9 de 14, 7 de 3, y sueltos de 1/2/5/7/28/42). Se
redondearon a la escala —`1/2/3/5→4`, `6/7→8`, `10→12` (pads) y `10→8` (radios),
`14→16`, `28→24`— y las píldoras pasan a `LV_RADIUS_CIRCLE` en vez de un 42 a
mano. Además las **dos pestañas reservan lo mismo** para la barra inferior
(`UI_BAR_H` 48): antes 50 en las vistas y 62 en ajustes, que dejaba una banda de
más abajo. La auditoría lo comprueba (regla 3 de la sección 12).

## 4. Componentes

| Componente | Ficha |
|---|---|
| **Cabecera** | icono (20) + título (`UI_FONT_TITLE`) + subtítulo (`UI_FONT_SMALL`, `TEXT_DIM`), alto `UI_HEADER_H`, fondo `BG` |
| **Tarjeta** | `CARD` + borde `CARD_BORDER` 1 px + radio 16 + pad 20; título en `UI_FONT_VALUE` con icono; nunca sin icono |
| **Cifra** | etiqueta `UI_FONT_SMALL` `TEXT_DIM` + valor `UI_FONT_VALUE`/`DISPLAY` `TEXT` + unidad `UI_FONT_SMALL` `TEXT_DIM`; las cifras de una fila comparten línea base |
| **Fila de ajuste** | alto `UI_ROW_H`, icono en cuadro radio `RADIUS_CTRL`, etiqueta `UI_FONT_TEXT`, control a la derecha |
| **Estado vacío** | `TEXT_DIM`, misma caja que el dato real (nunca el color del dato) |
| **Barra inferior** | chips con pad 4 y radio 4; alta fija; mismo `pad_bottom` en las dos pestañas (hoy 50 y 62). **Fondo `BG`** (antes cada zona llevaba su casi-negro: `#000408` y `#000808` en la misma barra) |
| **Aviso / diálogo** | acento `ORANGE` en borde, título y botón; fondo de error `RED_DARK`. Nada de rojo puro a pantalla completa |

## 5. Cómo se comprueba

- **Reglas automáticas**: `test/auditar.sh` sección 12 (fuentes del SDK
  prohibidas, literales de color fuera de la paleta, escala de medidas).
  Cada regla se prueba **al revés** antes de darla por buena.
- **Verificación visual**: `/captura?n=<i>` navega a la pantalla *i* y devuelve
  un BMP (0-23; índice en `/capturas`). Toda migración se compara antes/después
  con esas capturas; no se toca la UI "a ojo".

## 6. Plan de aplicación (por pasos, cada uno publicable)

1. **Tokens y reglas** (v4.17): `ui_style.h`, migración de los 41 usos de fuente
   a los papeles, reglas de auditoría para fuentes y colores.
2. **Colores** (v4.18): **hecho** — 59 colores distintos / 203 literales
   colapsados a los 14 tokens, verificado con las 24 capturas antes/después
   (solo cambian las zonas de color previstas; el resto, idéntico).
3. **Medidas y componentes** (v4.19 y v4.20): **hecho** — 93 usos fuera de la
   rejilla redondeados a la escala, píldoras con `LV_RADIUS_CIRCLE`, las dos
   pestañas reservando lo mismo para la barra (`UI_BAR_H`) y el relleno de página
   de Ajustes unificado a `UI_PAD_PAGE` (las cinco páginas de tarjeta empiezan ya
   exactamente en y=84; la banda superior entre familias **no** se iguala, y está
   razonado arriba). Verificado con las 24 capturas.
   Queda pendiente, y necesita decisión de diseño: el **acento por sección** (hoy
   cada entrada del menú de Ajustes lleva el suyo: azul, naranja, rosa, morado,
   amarillo, verde).

Lo que **no** entra: cambiar la distribución de las pantallas ni la información
que muestran; esto es coherencia, no rediseño.
