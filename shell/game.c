/* Which game the foreground process is: its title ID, and whether that is
 * a game at all. */
#include <psp2/appmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include "shell.h"

int sceKernelGetProcessTitleId(SceUID pid, char *titleid, SceSize len); /* SceProcessmgr, not in headers */

int vjo_title_id(SceUID pid, char *tid, int size)
{
    int ret;
    sceClibMemset(tid, 0, size);
    ret = sceKernelGetProcessTitleId(pid, tid, size);
    if (ret >= 0 && tid[0])
        return 0;
    sceClibMemset(tid, 0, size);
    ret = sceAppMgrAppParamGetString(pid, 12, tid, size); /* 12 = title ID */
    if (ret >= 0 && tid[0])
        return 0;
    return ret < 0 ? ret : -1;
}

/* vjoSetGameActive's mode for a title: system apps, SceShell and VitaShell
 * (Select starts its FTP server) are not games. The PSP emulator, where
 * Adrenaline's games run, is one with a static framebuffer; all its games
 * share its title ID, so one region. */
int vjo_title_game_mode(const char *tid)
{
    if (!sceClibStrcmp(tid, "NPXS10028"))
        return VJO_GAME_STATIC_FB;
    if (!sceClibStrncmp(tid, "NPXS", 4) || !sceClibStrncmp(tid, "main", 4) ||
        !sceClibStrncmp(tid, "VITASHELL", 9))
        return VJO_GAME_NONE;
    return VJO_GAME;
}
