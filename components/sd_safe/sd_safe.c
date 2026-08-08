#include "sd_safe.h"
#include "camera.h"

#include <errno.h>
#include <unistd.h>

bool sd_stat(const char *path, struct stat *st, uint32_t timeout_ms)
{
    if (!camera_sd_bus_lock(timeout_ms)) { errno = EBUSY; return false; }
    int r = stat(path, st);
    camera_sd_bus_unlock();
    return r == 0;
}

FILE *sd_fopen(const char *path, const char *mode, uint32_t timeout_ms)
{
    if (!camera_sd_bus_lock(timeout_ms)) { errno = EBUSY; return NULL; }
    FILE *f = fopen(path, mode);
    camera_sd_bus_unlock();
    return f;
}

int sd_mkdir(const char *path, mode_t mode, uint32_t timeout_ms)
{
    if (!camera_sd_bus_lock(timeout_ms)) { errno = EBUSY; return -1; }
    int r = mkdir(path, mode);
    camera_sd_bus_unlock();
    return r;
}

int sd_unlink(const char *path, uint32_t timeout_ms)
{
    if (!camera_sd_bus_lock(timeout_ms)) { errno = EBUSY; return -1; }
    int r = unlink(path);
    camera_sd_bus_unlock();
    return r;
}

int sd_rename(const char *old_path, const char *new_path, uint32_t timeout_ms)
{
    if (!camera_sd_bus_lock(timeout_ms)) { errno = EBUSY; return -1; }
    int r = rename(old_path, new_path);
    camera_sd_bus_unlock();
    return r;
}
