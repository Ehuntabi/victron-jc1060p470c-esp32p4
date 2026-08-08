#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Envoltorios de UNA sola llamada libc a la SD que incluyen POR DENTRO el
 * cerrojo camera_sd_bus_lock()/camera_sd_bus_unlock(): es estructuralmente
 * imposible tocar la tarjeta sin pasar por el cerrojo, porque no hay otra
 * forma de llamar a estas funciones. Nacio del bug real de v1.4.4: un
 * stat() suelto se colaba FUERA del cerrojo en 3 sitios distintos del
 * proyecto (datalogger, battery_history, ne185_vlog — mismo patron
 * copiado), y provocaba contencion SDMMC con la camara -> INT WDT.
 *
 * Si el cerrojo no se consigue en timeout_ms, devuelven el mismo "no se
 * pudo" que ya devolvia el codigo existente cuando camera_sd_bus_lock
 * fallaba (NULL / -1 con errno=EBUSY) — no es un comportamiento nuevo.
 *
 * Para mantener el cerrojo durante VARIAS llamadas seguidas (volcar muchas
 * lineas de un flush, escanear un directorio entero con opendir/readdir)
 * seguir usando camera_sd_bus_lock()/camera_sd_bus_unlock() directamente
 * alrededor, como ya se hace en el resto del proyecto — estos wrappers son
 * para la llamada SUELTA, no sustituyen ese patron. */

bool  sd_stat(const char *path, struct stat *st, uint32_t timeout_ms);
FILE *sd_fopen(const char *path, const char *mode, uint32_t timeout_ms);
int   sd_mkdir(const char *path, mode_t mode, uint32_t timeout_ms);
int   sd_unlink(const char *path, uint32_t timeout_ms);
int   sd_rename(const char *old_path, const char *new_path, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
