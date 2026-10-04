/* config.ini / region.ini access. */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>

#include "../core/regions.h"
#include "shell.h"

#define MAX_FILE (64 * 1024)

char *vjo_file_read(VjoArena *a, const char *path, size_t *len)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size;
    char *buf;
    int n;
    if (fd < 0)
        return NULL;
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (size < 0 || size > MAX_FILE) {
        sceIoClose(fd);
        return NULL;
    }
    buf = (char *)vjo_arena_alloc(a, (size_t)size + 1);
    if (!buf) {
        sceIoClose(fd);
        return NULL;
    }
    n = sceIoRead(fd, buf, (SceSize)size);
    sceIoClose(fd);
    if (n < 0)
        return NULL;
    buf[n] = '\0';
    if (len)
        *len = (size_t)n;
    return buf;
}

int vjo_file_write(const char *path, const void *data, size_t len)
{
    /* Write to a temp file and rename, so a crash never leaves a torn file. */
    char tmp[128];
    SceUID fd;
    int n;
    sceClibSnprintf(tmp, sizeof(tmp), "%s.tmp", path);
    sceIoMkdir(VJO_DATA_DIR, 0777);
    fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return fd;
    n = sceIoWrite(fd, data, len);
    sceIoClose(fd);
    if (n != (int)len)
        return -1;
    sceIoRemove(path);
    return sceIoRename(tmp, path);
}

static int file_exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

void vjo_config_load(VjoConfig *cfg, VjoArena *scratch)
{
    size_t mark = vjo_arena_mark(scratch), len = 0;
    char *text = vjo_file_read(scratch, VJO_CONFIG_PATH, &len);
    vjo_config_defaults(cfg);
    if (text) {
        vjo_config_parse(cfg, text, len);
    } else if (!file_exists(VJO_CONFIG_PATH)) {
        /* First run only: never overwrite a file we merely failed to read. */
        const char *def = vjo_config_default_text();
        vjo_file_write(VJO_CONFIG_PATH, def, sceClibStrnlen(def, 4096));
    }
    vjo_arena_release(scratch, mark);
}

/* region.ini: see core/regions.h. */
int vjo_region_load(VjoRect *out, VjoArena *scratch)
{
    size_t mark = vjo_arena_mark(scratch), len = 0;
    char *text = vjo_file_read(scratch, VJO_REGION_PATH, &len);
    int found = text ? vjo_region_parse(text, len, out) : 0;
    vjo_arena_release(scratch, mark);
    return found;
}

int vjo_region_save(const VjoRect *r)
{
    char line[64];
    int n;
    if (!r) {
        sceIoRemove(VJO_REGION_PATH);
        return 0;
    }
    n = vjo_region_format(line, sizeof(line), r);
    return n < 0 ? n : vjo_file_write(VJO_REGION_PATH, line, (size_t)n);
}
