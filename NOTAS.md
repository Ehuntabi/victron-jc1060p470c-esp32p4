## v1.4.5

**Coredump automático a SD.** ESP-IDF vuelca ELF a una partición dedicada
(`coredump`, nueva en `partitions.csv`) justo al panicar/TWDT/INT_WDT, ANTES
del reset — sobrevive exactamente los cuelgues que el ring buffer en RAM no
puede. En el siguiente arranque se exporta a
`/sdcard/crash_<motivo>_<fecha>.txt` (tarea, PC de la excepción, registros
RISC-V, stackdump) para analizar a posteriori sin monitor serie conectado en
el momento del crash.

**Acceso a SD centralizado en `sd_safe`.** Nuevo componente con wrappers
(`sd_stat`/`sd_fopen`/`sd_mkdir`/`sd_unlink`/`sd_rename`) que incluyen el
cerrojo `camera_sd_bus_lock()` por dentro — estructuralmente imposible
olvidarlo en una llamada suelta, que es exactamente el bug real detectado en
v1.4.4 (un `stat()` sin cerrojo en 3 sitios: `datalogger`, `battery_history`,
`ne185_vlog`). De paso se encontró y arregló un cuarto hueco en
`battery_history` que no se había pillado antes (`stat("/sdcard")` + `mkdir`
sin cerrojo en el flush de 60 s).

Revisado sistemáticamente el resto de módulos que tocan `/sdcard`
(`solar_daily`, `config_backup`, `log_cleanup`, `log_browser`, `gallery`,
`config_server`): ya estaban todos correctos.
