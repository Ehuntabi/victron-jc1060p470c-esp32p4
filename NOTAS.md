## v1.4.6

Refactor de estructura, sin cambio de comportamiento: `config_server.c` baja
de 2828 a 1785 líneas. Dos bloques grandes y autocontenidos salen a su
propio fichero:

- `charts_svg.c` — gráficos SVG con auto-escala (históricos frigo/batería).
- `data_export_tar.c` — streaming `.tar` de las carpetas de `/sdcard`
  (frigo, batería, solar, capturas, vigilancia, config, logs, viaje).

El resto (auth, ciclo de vida del AP/portal, handlers core) se queda en
`config_server.c` por estar más entrelazado con su estado interno.
`check_basic_auth`/`REQUIRE_AUTH` se mueven a `config_server_internal.h`
(costura interna, no API pública) para que los tres ficheros compartan la
misma lógica de auth sin duplicarla.
