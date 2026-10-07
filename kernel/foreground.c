/* Foreground state transitions (see foreground.h). */
#include "foreground.h"

static uint32_t set_foreground(VjoForeground *f, int pid, uint32_t iev)
{
    f->game_active = 0;
    f->game_pid = pid;
    return iev;
}

uint32_t fg_process_event(VjoForeground *f, int pid, int ev)
{
    switch (ev) {
    case PROCEV_STARTUP:
    case PROCEV_RESUME:
        if (pid == f->shell_pid || pid == f->game_pid)
            return 0;
        if (f->game_active)
            f->prev = f->game_pid;
        if (pid == f->prev)
            f->prev = 0;
        return set_foreground(f, pid, IEV_GAME_START);
    case PROCEV_EXIT:
    case PROCEV_SUSPEND:
        if (pid == f->prev)
            f->prev = 0;
        if (pid == f->game_pid) {
            int prev = f->prev;
            f->prev = 0;
            return set_foreground(f, prev, prev > 0 ? IEV_GAME_EXIT | IEV_GAME_START : IEV_GAME_EXIT);
        }
        return 0;
    }
    return 0;
}

int fg_set_game_active(VjoForeground *f, int pid, int mode, uint32_t *iev)
{
    *iev = 0;
    if (pid <= 0 || pid != f->game_pid)
        return -1;
    if (mode) {
        f->game_active = mode;
        *iev = IEV_ACTIVATE;
    } else {
        f->game_active = 0;
        if (f->prev > 0) {
            *iev = set_foreground(f, f->prev, IEV_GAME_START);
            f->prev = 0;
        }
    }
    return 0;
}
