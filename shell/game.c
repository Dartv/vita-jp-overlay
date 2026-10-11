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

/* The game's name for MiscInfo: its title (app parameter 9, else 10);
 * "PSP" for the PSP emulator, which every Adrenaline game shares; "" when
 * neither is readable. Both parameters are logged to tell them apart. */
void vjo_title_name(SceUID pid, const char *tid, char *name, int size)
{
    char p9[128], p10[128];
    int r9, r10;
    sceClibMemset(p9, 0, sizeof(p9));
    sceClibMemset(p10, 0, sizeof(p10));
    r9 = sceAppMgrAppParamGetString(pid, 9, p9, sizeof(p9) - 1);
    r10 = sceAppMgrAppParamGetString(pid, 10, p10, sizeof(p10) - 1);
    vjo_log("title %s: param 9 = \"%s\" (0x%08X), param 10 = \"%s\" (0x%08X)", tid, p9, r9, p10, r10);
    if (!sceClibStrcmp(tid, "NPXS10028"))
        sceClibSnprintf(name, size, "PSP");
    else if (r9 >= 0 && p9[0])
        sceClibSnprintf(name, size, "%s", p9);
    else if (r10 >= 0 && p10[0])
        sceClibSnprintf(name, size, "%s", p10);
    else
        name[0] = '\0';
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
