v4.32 — la pastilla de 230 V sin LED, y el icono de aguas grises cuadrado

## Qué cambia

- **230 V**: se quita el **LED redondo** — la propia pastilla ya se pone verde
  cuando detecta 230 V, así que el redondo sobraba (lo dijo el usuario). La onda
  (corriente alterna) se queda pegada al texto, como se escribe "230 V ~".
- **Aguas grises**: fuera la **"tapa"** (una línea horizontal suelta encima de las
  ondas; el usuario la vio como una raya rara). El **agua** (el rectángulo gris)
  llena el ancho útil del depósito y llega hasta abajo — antes 62×26 con 3 px de
  aire y parecía flotar —, las **tres ondas van centradas** sobre el agua y el
  **pitorro** pasa a la esquina inferior DERECHA, como el dibujo del usuario.

## Trampa medida (6-oct-2026)

`overview_align_grey()` seguía **forzando el ancho del icono gris al de la
pastilla de 230 V** (119 px en vez de 76): era herencia de cuando el indicador era
un rectángulo liso que se alineaba con la pastilla. Con el icono dibujado eso lo
estirada y, por dentro, las ondas se iban a la izquierda ("no están centradas").
Ahora el icono tiene su medida y se centra solo; esa función únicamente lo baja
para igualar su base con la del nivel de aguas limpias.

## Verificado (6-oct-2026)

- Captura de la pantalla principal: "230 V ~" sin LED y el icono de grises
  centrado, con las ondas sobre el agua y el pitorro abajo a la derecha.
- `AUDITORIA OK` (11 reglas de cámara + 5 de estilo).

v4.31 — los botones de la pantalla principal, como los pictogramas del panel

## Qué cambia

- **Luz INT / Luz EXT y Bomba** dejan de ser pastillas con icono+texto y pasan a
  ser el pictograma del panel físico del usuario: **óvalo de contorno fino y SIN
  relleno**, el dibujo dentro y el **LED de estado dentro, a la izquierda**, más
  grande (20 px) en los tres.
- **Bombilla con rayos** (glifo FontAwesome + seis trazos dibujados) y rótulo corto
  "INT"/"EXT". La **Bomba va sin texto**: el grifo ya lo dice.
- **Grifo dibujado** (64×48, más ancho que alto): maneta de dos barras, cuello,
  cuerpo largo, bajante en el extremo derecho y boca de salida hacia la izquierda.
- **230 V**: la pastilla pasa a fila [texto][LED] — al ser de tamaño contenido, un
  hijo alineado fuera la hacía crecer y el LED acababa montado sobre la "V" — y
  lleva el **simbolito de onda** (corriente alterna) encima del LED.
- **Aguas limpias**: el depósito lleva **tapón arriba a la derecha** (a caballo del
  borde) y una **onda en la superficie del agua**, que se pega a la barra encendida
  más alta (el nivel ES el agua). En reserva o sin dato, la onda no se enseña.

## Trampas medidas (5-6 oct-2026, para el que siga)

- **LVGL 8.4 recorta los hijos contra el padre** (`lv_obj_redraw`, `clip_coords_for_children`)
  salvo que el padre lleve `LV_OBJ_FLAG_OVERFLOW_VISIBLE`. Por eso el tapón (que
  asoma por arriba del depósito) no se veía, y por eso el pitorro de las grises es
  hijo de la CAJA y no del cuerpo. Ahora el tapón cuelga de la caja y `ui_tank_set`
  lo recoloca en cada refresco; la columna de barras lleva `OVERFLOW_VISIBLE` para
  que la onda no desaparezca con el depósito lleno (4/4).
- `lv_obj_align()` **no aplica las alineaciones `OUT_*`**: caen en el caso por
  defecto de `lv_obj_refr_pos` y el objeto se va a la esquina superior izquierda
  (el tapón salió en la esquina contraria). Los `OUT_*` solo los resuelve
  `lv_area_align()`, o sea `lv_obj_align_to()`.
- `lv_obj_align_to()` calcula la posición **una sola vez**, y al crear el tanque la
  caja todavía mide 1 px (el alto se le pone después, al colocarla en la tarjeta):
  ahí no vale; `align` es de estilo y se reaplica en cada reflujo.
- El LED con `lv_obj_align(..., LEFT_MID/TOP_MID, ...)` se alinea contra el **área
  de contenido** del botón (que lleva el relleno del tema): quedaba 8 px dentro del
  óvalo y en la Bomba se leía como un punto del dibujo.

## Verificado (6-oct-2026)

- Captura con **estado inyectado** (agua limpia 3/4, grises llenas, Luz INT y Bomba
  encendidas, 230 V conectado): los tres LEDs en verde, la bombilla con rayos, el
  grifo y la onda de la superficie en su sitio. La inyección era temporal (llama a
  `ne185_sim_inject`, no a `sim_overview`: así no escribe CSV inventados en la SD) y
  **se quitó antes de publicar**.
- Captura final con el firmware de producción (tapón y dibujos en su sitio) y
  `AUDITORIA OK` (11 reglas de cámara + 5 de estilo).

v4.30 — icono de aguas grises (depósito, ondas y pitorro) en la pantalla principal

## Qué cambia

- El indicador de **aguas grises** deja de ser un **rectángulo liso** y pasa a ser
  el **icono** que propuso el usuario: depósito con **tapa**, **tres ondas**
  dentro y **pitorro** abajo. Dibujado con formas (como la batería o el
  ventilador), sin meter imágenes.
- Se mantiene lo que hacía: el "agua" (hijo 0) se pone **ROJO cuando está lleno**
  (es un aviso; el NE185 solo da lleno/no lleno) y el **toque silencia** la alarma.
- El icono tiene su propia medida: ya no se le fuerza el ancho del pill de 230 V,
  que lo estiraba.

## Verificado (5-oct-2026)

- Captura de la pantalla principal: se ve el depósito con las tres ondas y el
  pitorro, en el sitio del rectángulo.
- `AUDITORIA OK` (11 reglas de cámara + 5 de estilo). De paso saltó una: había
  puesto un radio de 2 px en el pitorro, fuera de la rejilla → `UI_RADIUS_TAG`.

v4.29 — todos los desplegables de la app, con la fuente de la casa (20)

## Qué cambia

- Los selectores (`lv_dropdown`) iban con la **fuente del tema** (14 px, la del SDK,
  solo ASCII) mientras la etiqueta de al lado iba a 20 y los títulos a 24. Ahora
  **todos** (los 9 de la app: Pantalla, Wi-Fi, Sonido, Frigo…) usan `UI_FONT_TEXT`
  (20), en el botón **y** en la lista al abrirse.
- Un solo sitio: ayudante `ui_dd_create(parent)` en `ui_card.c/h`; se sustituyen
  las 9 llamadas a `lv_dropdown_create()`. Regla: **usar siempre `ui_dd_create()`**.

## Verificado (5-oct-2026)

- Captura de la página Pantalla: "Modo: [Atenuar]" con el texto al tamaño de su
  etiqueta.
- `AUDITORIA OK` (11 reglas de cámara + 5 de estilo).

v4.27 — Pantalla: la disposición que pidió el usuario (a mano, con foto de la placa delante)

## Qué cambia

- **Switch de Salvapantallas pegado al texto**: se creaba DESPUÉS del separador
  elástico de la fila, así que el separador lo empujaba a la derecha. Ahora se crea
  antes y el hueco es `UI_PAD_8`.
- **"Brillo en reposo: X%" vuelve a su propia línea**, con su deslizador (lo había
  subido a la línea del título en un intento anterior).
- **Modo nocturno**: los grupos **Inicio** y **Fin** van separados (`pad_column`
  48) y centrados.
- **Las cuatro tarjetas caben**: el contenedor de la página ocupa el alto completo
  y reparte el hueco (`SPACE_BETWEEN`); la tarjeta de brillo y la de modo nocturno
  van más compactas. La fila de abajo (Vista por defecto / Pantalla de bienvenida)
  ya no queda cortada.

## Cómo se cerró (importante para la próxima vez)

El usuario mandó una **foto de la pantalla física** (`08:52`) que fue lo que
resolvió el malentendido: mis capturas por `/captura` y sus imágenes se parecían
demasiado y estuve tres intentos adivinando. Con la foto delante, la diferencia
salto a la vista. **Para cambios de disposición, pedir foto de la pantalla.**

v4.26 — Pantalla: el switch de Salvapantallas vuelve junto al título y el modo nocturno centrado

## Qué cambia

- **Salvapantallas**: el switch vuelve a la **misma línea que el texto** (el intento
  anterior lo movió a la fila del porcentaje y quedaba el texto solo arriba). El
  usuario lo vio en la placa y dio el visto bueno ("mejor así").
- **Modo nocturno**: el bloque **Inicio / Fin** va **centrado** (se quita el
  espaciador flexible que lo empujaba al borde derecho).

## Verificado (5-oct-2026)

- Captura de la página Pantalla con el usuario delante: `Salvapantallas [switch] …
  Tiempo (min): − 1 +` en una línea y `Inicio − 22:00 +   Fin − 07:00 +` centrado.
- `AUDITORIA OK` (11 reglas de cámara + 5 de estilo).

v4.25 — el gráfico de la batería vuelve, y las páginas de Ajustes ya no se cortan

## Qué cambia

- **El gráfico de la batería de la pantalla principal había DESAPARECIDO** (visto
  por el usuario). Causa: al cerrar la paleta (v4.18) colapsé los grises de su
  **carcasa** (`4A4A55`), los separadores de celda (`70707C`), el contorno
  (`2A2A30`) y la franja (`2E2E36`) a `UI_COLOR_CARD`… que es **el fondo de la
  propia tarjeta**: la batería se dibujaba, pero invisible. Ahora usan colores de
  la paleta **con contraste** (`CARD_BORDER` carcasa y franja, `TEXT_DIM`
  separadores, `BG` contorno). Lo mismo pasaba con un **separador** en Pantalla y
  con los fondos de dos **botones de modo** (histórico de batería y de solar):
  corregidos.
- **Las páginas de Ajustes se cortaban por abajo** (Tarjeta SD y Pantalla, visto
  por el usuario). Dos causas: (1) yo había **subido** los rellenos y huecos en la
  migración a la rejilla, y (2) el menú de Ajustes ocupaba los 540 px de debajo de
  las pestañas, pero los últimos ~48 los tapa la **barra inferior**. Ahora el menú
  se dimensiona descontando la barra (`LV_VER_RES - 60 - UI_BAR_H`), las páginas
  van a `UI_PAD_4`/`UI_PAD_4` de relleno y hueco, y el relleno de tarjeta baja de
  20 a 16 (`UI_PAD_CARD`).
- El diagnóstico de "sobra" pasa a ser **recursivo (3 niveles)**: miraba solo la
  página y sus hijos directos, y daba 0 mientras la última tarjeta se veía
  cortada.
- Pantalla: el **switch de Salvapantallas** pasa a la línea del porcentaje
  (`[switch] Brillo en reposo: 25% [slider]`), como pidió el usuario.

## Verificado (5-oct-2026)

- Batería: comparada con la captura de referencia (v4.16), el dibujo vuelve a
  verse.
- Tarjeta SD: la última tarjeta ya muestra su borde inferior completo.
- `AUDITORIA OK` (11 reglas de cámara + 5 de estilo).

v4.24 — un solo diagnóstico de "sobra" por página (el que mide la página ya colocada)

## Qué cambia

- Había **dos** líneas midiendo lo mismo en momentos distintos y daban números
  distintos para la misma página (`alto=430 sobra=+82` y `alto=540 sobra=-28`):
  la primera medía **antes** de que el menú colocase la página, con el alto que se
  fuerza un momento antes. Se queda **solo la de `ui_settings_panel_show_page()`**,
  que mide la página ya colocada — lo que ve el usuario.

## Verificado (4-oct-2026)

- Las páginas siguen cabiendo: victron_keys **-7**, logs 0, display -9,
  tarjeta_sd -11, wifi -28, frigo -39, gps -52, sonido -71, autocaravana -75.
- `AUDITORIA OK` (11 reglas de cámara + 5 de estilo).

v4.23 — ninguna página de Ajustes obliga ya a desplazar en vertical

## Qué cambia

- **Victron Keys** era la única que desbordaba, y mucho: **819 px de más** sobre
  los 540 visibles (una tarjeta grande por dispositivo, hasta 8). Ahora:
  - **paginador** `◀ n/m ▶` en la tarjeta de controles: se ve **un dispositivo a
    la vez** (los botones + y − de añadir/quitar siguen igual);
  - los tres campos (**Nombre / Dirección MAC / Clave AES**) pasan a **una fila
    cada uno** (etiqueta a la izquierda, caja a la derecha): antes la etiqueta iba
    encima y la columna medía ~70 px de más;
  - la nota de dos líneas se queda en una.
  Medido: de **+819 a −7** (cabe con 7 px de sobra).
- **Diagnóstico permanente** en `settings_panel.c`: al entrar en cada página
  escribe `sobra=<px>` (negativo = cabe). Así se comprueba sin adivinar, y queda
  como red para futuros cambios.

## Verificado (4-oct-2026)

- Las 10 páginas de Ajustes, medidas por el log: victron_keys **-7**, logs 0,
  display -9, tarjeta_sd -11, wifi -28, frigo -39, gps -52, sonido -71,
  autocaravana -75. Ninguna necesita scroll.
- Las vistas (overview, batería, solar, DC/DC, detalle…) y los históricos: se
  comprobó que su última tarjeta termina por encima de la barra inferior (539-548
  de 552 disponibles). La **galería** sí desplaza, y es lo correcto: crece con las
  fotos.
- `AUDITORIA OK`: 11 reglas de cámara + 5 de estilo.

v4.22 — el rectángulo de aguas grises vuelve a tener su borde (regresión de las medidas) y una sola etiqueta

## Qué cambia

- **Regresión arreglada**: al subir los rellenos a la rejilla de 4 px (14 → 16), el
  indicador de **aguas grises** de la pantalla principal salía **sin el borde
  inferior** (el "rectángulo cortado"). Causa: su alto se pasaba a
  `ui_tank_create` (90) y **después se forzaba el widget a 88**; el cuerpo no cabía
  y perdía el borde de abajo. Ahora el alto se pasa **una sola vez**
  (`UI_TANK_GREY_BODY_H`, 92) y el encaje del indicador está **acotado** para que no
  pueda salirse de su columna ni de la tarjeta (antes solo se bajaba "a ojo" hasta
  alinear con el depósito de agua limpia).
- La última etiqueta que espaciaba letras a mano (el "SATÉLITES" del GPS) pasa al
  estilo de las demás: era el resto del estilo viejo de versalitas.
- Regla nueva en `test/auditar.sh`: prohibido `letter_space` a mano (probada al
  revés).

## Verificado (4-oct-2026)

- El borde inferior se comprueba **con la captura ampliada 4×** (`/captura?n=0`):
  antes faltaba la línea horizontal y quedaban solo los rabillos laterales; ahora
  está completa. Comparado además con la v4.18 (la última buena) en la pantalla
  entera.
- `AUDITORIA OK`: 11 reglas de cámara + 5 de estilo.

v4.21 — altos a la rejilla de 4 px y tokens de papel (fila, pestaña, slider)

## Qué cambia

- Segunda mitad de las medidas: `lv_obj_set_height` tenía **26** (sliders), **38**,
  **42**, **46**, **50**, **54**, **58** y **90**, todos fuera de la rejilla.
  Redondeados a `24`, `40`, `44`, `48`, `56` y `88`.
- Tokens nuevos para los papeles que se repiten: `UI_TAB_H` 56 (filas de menú y
  pestañas), `UI_SLIDER_H` 24 (volumen y brillo), y `UI_ROW_H`/`UI_HEADER_H` para
  filas y botones. El `0` (objeto oculto) y el `1` (línea separadora) se quedan:
  son estructurales.
- Regla nueva en `test/auditar.sh` (3b): los altos escritos a mano son múltiplos
  de 4 salvo 0 y 1. Probada al revés.

## Verificado (4-oct-2026)

- Las 24 capturas: 0,4-0,7% de diferencia en la mayoría (la hora y los datos en
  vivo) y donde toca más — Pantalla **10,7%** (dos sliders y el campo de texto),
  Wi-Fi 4,9%, Tarjeta SD 4,1%. Los sliders quedan algo más finos; el resto igual.
- El binario publicado renderiza igual que el de trabajo (0,42%).
- `AUDITORIA OK`: 11 reglas de cámara + 4 de estilo.

v4.20 — la banda superior de Ajustes: medida, unificada dentro de la familia y razonada entre familias

## Qué cambia

- El punto 8 del briefing del 22-sep decía "en Ajustes el contenido empieza en
  y=103 y en las vistas en y=23-31". **Medido pantalla a pantalla** con
  `/captura?n=<i>`, no era un padding mal puesto: son **dos familias con cabecera
  distinta a propósito**:

  | familia | cabecera | primera tarjeta |
  |---|---|---|
  | vistas (overview, batería, solar, DC/DC, detalle…) | ninguna: el título va dentro de cada tarjeta | y=26-34 |
  | Ajustes (wifi, pantalla, tarjeta SD, sonido, GPS, about…) | miga de pan + título centrado | y=84 (el aviso del GPS, que es banner, en 72) |
  | históricos (log_*, logs) | barra con Hoy/Semana y Cerrar | y=112-185 |

  Igualarlas sería quitarle a Ajustes el saber dónde estás y cómo volver.
- Lo que **sí** era incoherencia, dentro de Ajustes: cada página ponía su propio
  relleno (`12`, `8` y `4`), así que el contenido empezaba en 84, 80 o 72 según la
  página. Ahora todas usan `UI_PAD_PAGE` y las **cinco páginas de tarjeta empiezan
  exactamente en y=84**.

## Verificado (4-oct-2026)

- 24 capturas antes/después: solo desplazamientos de 4-8 px, contenido idéntico,
  nada cortado (wifi y pantalla 80 → 84, GPS 72 → 84 en el contenido).
- El binario publicado renderiza igual que el de trabajo (0,35-0,40% = hora y
  datos en vivo).
- `AUDITORIA OK`: 11 reglas de cámara + 3 de estilo.

v4.19 — medidas a la rejilla de 4 px: 93 valores sueltos, redondeados (y una sola banda para la barra)

## Qué cambia

- **93 de 328** pads/radios estaban fuera de la escala (39 con valor `10`, 20 con
  `6`, 9 con `14`, 7 con `3`, y sueltos `1/2/5/7/28/42`). Redondeo aplicado:
  `1/2/3/5 → 4`, `6/7 → 8`, `10 → 12` en pads y `10 → 8` en radios, `14 → 16`,
  `28 → 24`. Ahora **0 fuera de la rejilla**.
- Las **píldoras** usan `LV_RADIUS_CIRCLE` en vez de un `42` calculado a mano
  (era la mitad de la altura, escrito a pelo).
- Las **dos pestañas reservan lo mismo** para la barra inferior (`UI_BAR_H`, 48):
  antes 50 en las vistas y 62 en ajustes, y esa diferencia dejaba una banda de más
  abajo en Ajustes.
- Regla nueva en `test/auditar.sh` (sección 12, regla 3), probada al revés.

## Verificado (4-oct-2026)

- Las 24 capturas antes/después: cambios de espaciado del **2-5%** en la mayoría
  (más aire, sin nada cortado), y las pantallas con más filas (Pantalla, Frigo,
  Tarjeta SD) reacomodadas. `log_bateria`, `log_solar` y `galeria` salen
  **idénticas**.
- El binario publicado renderiza igual que el de trabajo (0,25-0,27% = hora y
  datos en vivo).
- `AUDITORIA OK`: 11 reglas de cámara + 3 de estilo.

v4.18 — paleta cerrada: los 59 colores sueltos de la UI se colapsan a los 14 tokens

## Qué cambia

- **El único fichero de la UI con `lv_color_hex()` es `ui_style.h`** (15, la
  definición de los tokens). Antes: **203 literales en 18 ficheros, 59 colores
  distintos**, con el mismo significado en 3-4 tonos.
- Criterios (tabla completa en `docs/GUIA_ESTILO.md` §2):
  - negros y casi-negros → `BG`. La **misma barra inferior** llevaba `#000408` en
    una zona y `#000808` en otra: ahora es un solo color.
  - grises de superficie → `CARD`; bordes → `CARD_BORDER`; texto secundario →
    `TEXT_DIM`; blancos cálidos → `TEXT_SOFT`.
  - naranjas/ámbares (`FFAA00`, `FFBB33`, `FFA726`, `E0900A`, `FF7043`, `F57C00`)
    → `ORANGE`; verdes (`00CA52`, `4CD964`, `2E7D32`) → `GREEN`; cianes
    (`4AC2F7`, `00BFFF`, `29B6F6`, `42A5F5`, `26C6DA`, `008AD6`) → `CYAN`.
  - rojos oscuros (`882222`, `B51C19`, `8C2021`) → `RED_DARK`; morados
    (`9C27B0`, `BA68C8`, `BB66FF`) → `VIOLET`.
  - el rosa `E91E63` era el acento de los **diálogos de aviso** ("¿Borrar
    carpeta?", "Atención") → `ORANGE`, y queda como regla del componente.
- La regla de auditoría pasa de **tope** (203) a **cero literales fuera de
  `ui_style.h`**, probada al revés.

## Verificado (4-oct-2026)

- Las 24 pantallas capturadas antes y después (`/captura?n=<i>`): cambian solo las
  zonas de color previstas — Ajustes 33% (fondos de fila), overview 21%, y la
  barra inferior en todas (de dos negros a uno). La **tarjeta Solar sale
  idéntica** al píxel.
- El botón "Cerrar" de las gráficas pasa de `#882222` a `RED_DARK` y ahora se lee
  (antes casi no se distinguía del fondo).
- El binario publicado renderiza idéntico al de trabajo (0,41% de diferencia =
  la hora y los datos en vivo).
- `AUDITORIA OK` (11 reglas de cámara + 3 de estilo).

v4.17 — guía de estilo de la UI: una sola familia tipográfica, paleta cerrada y reglas que la defienden

## Qué cambia

- **`docs/GUIA_ESTILO.md`**: la guía, decidida midiendo las 24 pantallas con
  `/captura?n=<i>` (esa ruta navega a la pantalla y devuelve un BMP, así que la UI
  se verifica con capturas, no a ojo).
  - **Tipografía**: una familia (**Inter**) y cinco papeles con nombre:
    `UI_FONT_DISPLAY` (46), `TITLE` (28), `VALUE` (24), `TEXT` (20), `SMALL` (14).
  - **Color**: paleta cerrada, sin literales fuera de `ui_style.h`.
  - **Medidas**: rejilla de 4 px (4/8/12/16/20/24), radios 16/8/4, fila 44,
    cabecera 48.
- **`main/ui/widgets/ui_style.h`**: los tokens en un solo sitio (los colores salen
  de `ui_card.h`, que ahora incluye este; los nombres no cambian y nada se rompe).
- **41 usos** de `lv_font_montserrat_*` en el código de UI pasan a los papeles.
  El binario adelgaza **56 KB** (0x320af0 → 0x312ab0): `lv_font_montserrat_36` no
  estaba aliasado y mantenía su fuente compilada para dos estilos que no usaba
  nadie.
- **Sección 12 nueva en `test/auditar.sh`**: prohibido el nombre de una fuente del
  SDK en la UI, y **tope** a los literales de color (203 hoy, solo puede bajar).

## Corrección de un diagnóstico mío (importante)

Dije que había un bug de acentos (`"Sin señal"` / `"SATÉLITES"` con un hueco) y
**no era cierto**: `fonts_es.h` **ya aliasaba** los nombres sin `_es` a Inter, y
todos los ficheros de UI que los usaban incluían esa cabecera. Las capturas antes
y después salen **idénticas** (las diferencias medidas son la hora, los gráficos
en vivo y la versión). El motivo real de la regla es otro: el alias es una trampa
silenciosa (un fichero que use el nombre plano sin incluir `fonts_es.h` sí recibe
la fuente del SDK, que solo tiene ASCII) y el 36 engordaba el binario sin usarse.

## Verificado (4-oct-2026)

- Las 23 pantallas capturadas antes y después: **sin cambios estructurales**
  (solo barra inferior con la hora, gráficos y "Acerca de" con la versión nueva).
- Reglas probadas al revés: metiendo una fuente del SDK y 250 literales de color,
  la auditoría falla.
- `AUDITORIA OK` con las 11 reglas de cámara y las 2 de estilo.

v4.16 — la recuperación de la cámara deja de reiniciar la placa (se rearma el ISP en caliente)

## Qué cambia

- Tras recuperar el sensor por I2C, el ISP se rearma **EN CALIENTE** con
  `esp_video_isp_pipeline_set_ipa_config()` sobre la configuración del sensor
  (`esp_ipa_pipeline_get_config("OV02C10")`). Esa API **crea un pipeline IPA nuevo
  y lo intercambia con el vivo**, así que el AWB/AE arrancan de cero y se
  reaplican los parámetros iniciales al ISP: exactamente lo que hacía el
  reinicio, pero en el sitio.
- El **reinicio queda de respaldo**: si el rearme falla, o si la imagen sigue
  roja tras recuperar (detector de color, ventana de 10 min), se reinicia la
  placa como hasta ahora, con el tope de 2 por enchufe.

## Verificado (con la placa, 4-oct-2026)

- Forzando la corrupción a propósito, cadena completa en el log, **sin reinicio**:
  `imagen corrupta (grano 99)` → `recuperando el sensor (pauso captura...)` →
  `sensor reseteado y reconfigurado` → `IPA rearmado EN CALIENTE (0), sin
  reiniciar la placa` (11 ms después) → `imagen normal otra vez (grano 0)`.
- Medido con el **techo blanco** como referencia (que es lo que delata el
  magenta): antes de la corrupción R/G 0,97; después de recuperar **1,03-1,04**,
  estable durante los 2,5 min de sondeo. Cero reinicios (`MOTIVO ULTIMO REINICIO`
  siguió siendo el del cable USB, no `ESP_RST_SW`).
- Frente al reinicio: **~85 s sin AP y sin vigilancia → 11 ms**. La vigilancia no
  se corta y no hay que rearmarla.
- Reglas nuevas en `test/auditar.sh`, **probadas al revés** (borrando la llamada y
  el tope, la auditoría falla). Ojo al detalle: la primera versión de la regla
  buscaba el nombre de la función y pasaba con la llamada borrada (el nombre
  aparecía en un comentario); ahora busca la llamada con su argumento.

v4.15 — la cámara de noche: se va el magenta y la cara sale de la sombra

## Qué cambia

- **AWB: la ventana de puntos blancos vuelve a estar CENTRADA en el neutro real
  del sensor** (rg 0,53 / bg 0,64, calibrado con el techo blanco de referencia).
  Queda en rg 0,35-0,80 y bg 0,40-0,90. La v4.8 la había ensanchado a 0,2-1,6
  para arreglar el rojo de día, y eso **devolvió el magenta de noche**.
- **AE: modo `low_light_priority`** (protege las sombras) en lugar de
  `high_light_priority`, que daba 5× de peso a las zonas brillantes.
- Lo que arreglaba de verdad el rojo de día era el **objetivo del AE** (55 → 100,
  de la v4.8), no ensanchar la ventana: con la ventana centrada y el objetivo en
  100 no hay rojo saturado (0,0% en la lámpara cálida).

## Verificado (mismo salón y misma luz, 4-oct-2026)

| medida | antes (v4.14) | después |
|---|---|---|
| techo blanco, R/G | 1,99 (magenta) | **0,98-1,03** |
| imagen completa, R/G | 4,10 | **1,38-1,58** |
| cara (luma Y) | 46 | **93** |
| lámpara (luma Y) | 215 (quemando) | 159 |
| píxeles quemados con luz fuerte | (85% en la v4.7) | 2,9-4,0% (la propia luz) |

- Comprobado en **dos luces**: lámpara cálida (el caso que salía magenta) y luz
  blanca fuerte (para descartar el fallo contrario, pasarse de exposición).
- Reglas nuevas en `test/auditar.sh`: ventana del AWB centrada en el neutro real
  y AE de sombras. **Probadas al revés**: rompiendo cada ajuste, la auditoría falla.
- Lo que **no** está medido: luz de día natural (a las 17:32 la misma cámara daba
  R/G 1,46 y la cara bien, pero con la configuración anterior; conviene repetir la
  foto a plena luz para confirmar).

v4.14 — la cámara atascada se recupera SOLA, por software (ya no hay que cortar la corriente)

## Qué cambia

- **Recuperación del sensor por I2C**, en tres pasos y con el bucle de captura
  **en pausa** para que el AE del ISP no escriba a la vez:
  1. parar el stream del sensor (`0x0100=0x00`);
  2. reset por software (`0x0103=0x01`) y reescritura de su **tabla de modo
     completa** por I2C, respetando `REG_END`/`REG_DELAY` como el driver;
  3. **reiniciar la placa** para rearmar el ISP del P4.
- Mientras la imagen esté corrupta **no se sirven fotos**: se acabó guardar en la
  galería ~350 KB de ruido.
- El paso 3 no es un adorno: el reset del sensor deja el **AWB del ISP del P4
  desbocado** (imagen magenta saturada) y **no vuelve sola**. Medido en **3 de 3**
  recuperaciones: R/G (×100) pasaba de 138-195 a 269-666, con hasta el 78% de los
  píxeles con R≥250. Tras reiniciar, el color vuelve al de siempre (comprobado).
- **Tope de 2 reinicios por enchufe** (contador en memoria RTC: sobrevive al
  reinicio de software —que es lo que hay que contar— y el corte de corriente lo
  borra). Si se agotan, deja de reiniciar y lo dice: nada de bucles.
- **Aviso de imagen roja** en el log y en el portal (`salud`), sin reiniciar por
  color: solo informa. Si la cámara lleva 3 recuperaciones sin obedecer, el
  mensaje sigue siendo **cortar la corriente** (entonces el sensor no acepta
  órdenes y no hay software que lo arregle).

## Verificado (con la placa, 4-oct-2026)

- Cadena completa en el log, forzando la corrupción a propósito:
  `imagen corrupta (grano 99)` → `recuperando el sensor (pauso captura...)` →
  `sensor reseteado y reconfigurado` → `REINICIO la placa ... intento 2/2` →
  arranque con `MOTIVO ULTIMO REINICIO: 3` (= `ESP_RST_SW`, reinicio por software,
  no un cuelgue) → **cámara sana y neutra** (R/G 1,22 en la foto siguiente).
- El contador RTC sobrevivió a dos reinicios de software (marcó `intento 2/2`), que
  es justo lo que tiene que hacer para no entrar en bucle.
- Reglas nuevas en `test/auditar.sh` (sección 11): orden de la recuperación, pausa
  del bucle, reinicio con tope y no servir fotos corruptas. **Probadas al revés**:
  rompiendo cada cosa a propósito, la auditoría falla.
- Trampas del banco, documentadas en `documentacion/CONTINUAR_AQUI.md`: abrir
  `/dev/ttyACM0` **reinicia la placa** (un lector que se reconecta la deja
  arrancando en bucle: 341 arranques en 4 minutos), y el JPEG del portal **estira
  el color** respecto a la miniatura en crudo (R/G 335 vs 201 en la misma escena),
  así que las medidas de una vía y de la otra no se pueden comparar.

v4.7 — el watchdog vigila también el enlace con la cabina (y dice qué tarea se colgó)

## Qué cambia

- **Dos tareas más en el watchdog**, las del enlace con la cabina:
  - `udp_tx` (telemetría P4 → cabina): si se cuelga, la cabina deja de recibir
    datos **en silencio**.
  - `udp_latido` (latido cabina → P4): si se cuelga, **la P4 cree que la cabina se
    ha muerto** — su latido deja de llegar — y empieza a reiniciar su propio AP en
    cascada. Un cuelgue se convertía en un problema de radio.
- **El reset controlado dice el nombre de la tarea**: antes
  `Tarea 7 sin latido — reset controlado`, ahora
  `Tarea 'udp_tx (telemetria)' sin latido — reset controlado`.
- Las dos laten en su bucle (y también mientras reintentan el socket, para que un
  AP raro no provoque un reset en bucle: el watchdog vigila que la tarea esté
  viva, no que el socket esté listo).

## Verificado

- **Sin falsos positivos** en 75 s de arranque, y con la cabina **ausente** (el
  caso peligroso: `udp_latido` late por su timeout de 2 s, no por los paquetes que
  recibe; si latiera con los paquetes, una cabina apagada provocaría un reset).
- **Caza de verdad**: quitando el latido de `udp_tx` salvo en las 3 primeras
  vueltas, a los ~35 s salió
  `Tarea 'udp_tx (telemetria)' sin latido — reset controlado`, la placa se
  reinició de forma controlada y el arranque siguiente lo explicó
  (`Reset FORZADO por watchdog SW: tarea sin latido`).
- Regla nueva en `test/auditar.sh` (2e) para que no se caigan de la tabla.

## Hueco cerrado el 3-oct-2026 (v4.10): aviso de las que nunca laten

Una tarea que **nunca** llega a latir no se vigilaba: el monitor ignora las entradas
con `s_last_beat == 0`, así que si `xTaskCreate` fallara (o la tarea muriera antes
del primer latido) su cuelgue no se detectaba — solo quedaba el error del arranque
en el log. **Ya no**: pasado **2× el plazo de ESA entrada** (nunca un plazo global:
`bh_flush` late cada 600 s y `udp_tx` cada 1 s) el monitor escribe un aviso por
cada entrada que no haya latido nunca. No se resetea: un reset ahí sería
injustificado, y por eso mismo el aviso es la respuesta correcta.

Probado quitando a propósito los latidos de `udp_latido`: sale su aviso con el
plazo que le toca (16 s) y **no** reinicia. La primera versión avisaba nada más
pasar la ventana de gracia de 30 s y saltaba con tareas sanas que aún no habían
llegado a su primer latido (`datalogger` 60 s, `viaje` 30 s, `bh_flush` 600 s):
se vio en el banco y se corrigió antes de publicar. Regla 2e de `test/auditar.sh`,
comprobada rompiéndola a propósito.
