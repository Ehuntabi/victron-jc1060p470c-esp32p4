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
   un token (con nombre y motivo), no un literal.
2. **Un solo tono por significado.** Medido en pantalla el 4-oct-2026, hoy
   conviven varios tonos del mismo color semántico y hay que colapsarlos:

   | significado | tonos en pantalla hoy | queda |
   |---|---|---|
   | naranja | `FF9800`, `FF9A00`, `FFAA00`, `FF7043`, `FF7142` | `FF9800` |
   | verde | `00C851`, `00CA52`, `4CD964`, `297D31` | `00C851` |
   | cian | `4FC3F7`, `4AC2F7`, `008AD6`, `10456B` | `4FC3F7` |
   | rojo | `FF4444`, `FF595A`, `B51C19`, `8C2021`, `CC3333` | `FF4444` / `CC3333` |
   | grises | `313131`, `292831`, `424542`, `3A414A`, `081019`, `444444`, `666666`, `888888`, `AAAAAA` | `CARD` / `CARD_BORDER` / `TEXT_DIM` |
   | blanco cálido | `E6EBF7` | `TEXT_SOFT` |
3. **Acento por sección como máximo**: una pantalla puede tener un color de
   sección; el resto de colores son de **estado** (bien/atención/mal). Hoy hay
   pantallas con 6 acentos a la vez (Ajustes, Frigo, Sonido): eso es lo que hay
   que reducir.

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
| `UI_ROW_H` | 44 | alto de fila de ajuste |
| `UI_HEADER_H` | 48 | alto de cabecera |

Reglas: nada de 2, 3, 6, 10, 14 px sueltos; los márgenes laterales son los
mismos en vistas y en ajustes (ya unificado en v3.6, se mantiene); **la misma
banda superior** en todas las pantallas (pendiente del briefing del 22-sep: en
Ajustes el contenido empezaba en y=103 y en las vistas en y=23-31).

## 4. Componentes

| Componente | Ficha |
|---|---|
| **Cabecera** | icono (20) + título (`UI_FONT_TITLE`) + subtítulo (`UI_FONT_SMALL`, `TEXT_DIM`), alto `UI_HEADER_H`, fondo `BG` |
| **Tarjeta** | `CARD` + borde `CARD_BORDER` 1 px + radio 16 + pad 20; título en `UI_FONT_VALUE` con icono; nunca sin icono |
| **Cifra** | etiqueta `UI_FONT_SMALL` `TEXT_DIM` + valor `UI_FONT_VALUE`/`DISPLAY` `TEXT` + unidad `UI_FONT_SMALL` `TEXT_DIM`; las cifras de una fila comparten línea base |
| **Fila de ajuste** | alto `UI_ROW_H`, icono en cuadro radio `RADIUS_CTRL`, etiqueta `UI_FONT_TEXT`, control a la derecha |
| **Estado vacío** | `TEXT_DIM`, misma caja que el dato real (nunca el color del dato) |
| **Barra inferior** | chips con pad 4 y radio 4; alta fija; mismo `pad_bottom` en las dos pestañas (hoy 50 y 62) |
| **Aviso** | fondo `RED_DARK`/`ORANGE` con texto `TEXT`; nada de rojo puro a pantalla completa |

## 5. Cómo se comprueba

- **Reglas automáticas**: `test/auditar.sh` sección 12 (fuentes del SDK
  prohibidas, literales de color fuera de la paleta, escala de medidas).
  Cada regla se prueba **al revés** antes de darla por buena.
- **Verificación visual**: `/captura?n=<i>` navega a la pantalla *i* y devuelve
  un BMP (0-23; índice en `/capturas`). Toda migración se compara antes/después
  con esas capturas; no se toca la UI "a ojo".

## 6. Plan de aplicación (por pasos, cada uno publicable)

1. **Tokens y reglas** (esta ronda): `ui_style.h`, arreglo de los acentos, reglas
   de auditoría para fuentes y para colores *nuevos*.
2. **Colores**: colapsar los tonos duplicados de la tabla del punto 2, pantalla a
   pantalla, comparando capturas.
3. **Medidas y componentes**: llevar pads/radios/alturas a la rejilla y unificar
   las dos bandas superiores y el patrón de fila de ajustes.

Lo que **no** entra: cambiar la distribución de las pantallas ni la información
que muestran; esto es coherencia, no rediseño.
