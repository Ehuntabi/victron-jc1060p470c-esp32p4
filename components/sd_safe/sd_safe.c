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

int sd_mkdir(const char *path, mode_t mode, uint32_t timeout_ms)
{
    if (!camera_sd_bus_lock(timeout_ms)) { errno = EBUSY; return -1; }
    int r = mkdir(path, mode);
    camera_sd_bus_unlock();
    return r;
}
