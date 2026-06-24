# PENDIENTE: difundir el protocolo NE185/NE187 a la comunidad

> Recordatorio personal. Todo el contenido ya está escrito y subido a GitHub.
> Solo falta **publicarlo externamente** (lo haces tú, vía web, copiando-pegando).
> Textos listos en: `docs/community/CONTRIB_outreach.md`.

## Mis enlaces (ya públicos)

- **Repo:** https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4
- **Doc EN (para leer):** https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4/blob/main/docs/community/NE185_NE187_RS485_protocol_EN.md
- **Doc ES (para leer):** https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4/blob/main/docs/community/NE185_NE187_RS485_protocolo_ES.md
- **Textos de difusión:** https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4/blob/main/docs/community/CONTRIB_outreach.md
- **Raw EN (para gist / embeber):** https://raw.githubusercontent.com/Ehuntabi/victron-jc1060p470c-esp32p4/main/docs/community/NE185_NE187_RS485_protocol_EN.md
- **Raw ES:** https://raw.githubusercontent.com/Ehuntabi/victron-jc1060p470c-esp32p4/main/docs/community/NE185_NE187_RS485_protocolo_ES.md

## Checklist de publicación (5 min)

- [ ] **Gist público.** Abrir https://gist.github.com → pegar el contenido del doc
      EN → nombre `NE185_NE187_RS485_protocol_EN.md` → "Create **public** gist".
      Da un enlace corto y aparece en buscadores. (Copia el enlace del gist.)
- [ ] **Issue en class142/ne-rs485.** Abrir
      https://github.com/class142/ne-rs485/issues/new → pegar el **Texto A** de
      `CONTRIB_outreach.md`. (Es la audiencia exacta: documentan el NE334 hermano.)
- [ ] **Foros autocaravana (ES).** Pegar el **Texto B**. Sitios: foro de
      ForoCaravanas / Autocaravanas / grupos de Facebook de tu modelo.
- [ ] **Reddit.** Pegar el **Texto C** en r/vandwellers, r/diyelectronics,
      r/esp32. (Título y cuerpo ya redactados.)

## Cómo "linkar a mi GitHub" en los posts

- Para que se vea **renderizado** (con tablas): usa el enlace `blob/main/...`
  (los "Doc EN/ES para leer" de arriba).
- Para **embeber o copiar el texto crudo** (gist, markdown de otra web): usa el
  `raw.githubusercontent.com/...`.
- Si quieres un enlace permanente que no cambie aunque edites el doc: en GitHub,
  abre el archivo, pulsa `y` para fijar el commit (URL con el hash) → "permalink".

## Si más adelante quiero automatizarlo

`gh` (GitHub CLI) **no está instalado** en el portátil. Para que Claude pueda crear
el gist / la issue por mí haría falta: `sudo apt install gh` + `gh auth login`.
Mientras tanto, la vía web de arriba es más rápida.

## Estado del proyecto (para no perder el hilo)

- P4 reemplaza al NE187 al 100% (tanques, baterías, luces, bomba, 230V).
- Auto-encendido luz int + bomba al arranque: toggle **AUTO ON/OFF** en
  Settings → Consola (default OFF, persiste en NVS).
- Protocolo documentado en `docs/ne185_protocolo_completo.md` (interno) y
  `docs/community/*` (público, EN+ES, CC0).
