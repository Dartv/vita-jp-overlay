/* Logging: UDP datagrams to log_host:9999 (tools/udp_log_listener.py) and,
 * if enabled, ux0:data/VitaJPOverlay/log.txt capped at 256 KB with one
 * rotation (log.old.txt), so at most 512 KB is ever used. */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <stdarg.h>

#include "shell.h"

#define LOG_PORT 9999
#define LOG_FILE_CAP (256 * 1024)
#define STATUS_CAP (64 * 1024)

static SceUID log_mutex = -1;
static int udp_fd = -1;
static SceNetSockaddrIn udp_addr;
static int file_on;
static SceOff file_size = -1;

void vjo_log_configure(const VjoConfig *cfg)
{
    if (log_mutex < 0)
        log_mutex = sceKernelCreateMutex("VjoLog", 0, 0, NULL);
    sceKernelLockMutex(log_mutex, 1, NULL);
    if (udp_fd >= 0) {
        sceNetSocketClose(udp_fd);
        udp_fd = -1;
    }
    if (cfg->log_host[0]) {
        sceClibMemset(&udp_addr, 0, sizeof(udp_addr));
        udp_addr.sin_len = sizeof(udp_addr);
        udp_addr.sin_family = SCE_NET_AF_INET;
        udp_addr.sin_port = sceNetHtons(LOG_PORT);
        if (sceNetInetPton(SCE_NET_AF_INET, cfg->log_host, &udp_addr.sin_addr) == 1)
            udp_fd = sceNetSocket("VjoLogUdp", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    }
    file_on = cfg->log_file;
    file_size = -1;
    sceKernelUnlockMutex(log_mutex, 1);
}

static void file_append(const char *line, int len)
{
    SceUID fd;
    if (file_size < 0) {
        SceIoStat st;
        file_size = sceIoGetstat(VJO_LOG_PATH, &st) >= 0 ? st.st_size : 0;
    }
    if (file_size + len > LOG_FILE_CAP) {
        sceIoRemove(VJO_LOG_OLD_PATH);
        sceIoRename(VJO_LOG_PATH, VJO_LOG_OLD_PATH);
        file_size = 0;
    }
    fd = sceIoOpen(VJO_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    if (sceIoWrite(fd, line, len) == len)
        file_size += len;
    sceIoClose(fd);
}

/* status.txt: always on, rewritten at every boot and capped at 64 KB, so the
 * plugin's state can be checked even when networking or config is broken. */
static SceUID status_mutex = -1;
static int status_size = -1;

void vjo_status_reset(void)
{
    SceUID fd;
    status_mutex = sceKernelCreateMutex("VjoStatus", 0, 0, NULL);
    sceIoMkdir(VJO_DATA_DIR, 0777);
    sceIoRemove(VJO_DATA_DIR "/status.prev.txt");
    sceIoRename(VJO_STATUS_PATH, VJO_DATA_DIR "/status.prev.txt");
    fd = sceIoOpen(VJO_STATUS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoClose(fd);
        status_size = 0;
    }
}

/* Undoes vjo_status_reset when the plugin does not stay resident. */
void vjo_status_close(void)
{
    if (status_mutex >= 0)
        sceKernelDeleteMutex(status_mutex);
    status_mutex = -1;
    status_size = -1;
}

static void status_append(const char *line, int len)
{
    SceUID fd;
    if (status_size < 0 || status_size + len > STATUS_CAP)
        return;
    if (status_mutex >= 0)
        sceKernelLockMutex(status_mutex, 1, NULL);
    fd = sceIoOpen(VJO_STATUS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        if (sceIoWrite(fd, line, len) == len)
            status_size += len;
        sceIoClose(fd);
    }
    if (status_mutex >= 0)
        sceKernelUnlockMutex(status_mutex, 1);
}

void vjo_log_raw(const char *line, int len)
{
    if (len <= 0)
        return;
    status_append(line, len);
    if (log_mutex < 0)
        return;
    sceKernelLockMutex(log_mutex, 1, NULL);
    if (udp_fd >= 0)
        sceNetSendto(udp_fd, line, len, 0, (SceNetSockaddr *)&udp_addr, sizeof(udp_addr));
    if (file_on)
        file_append(line, len);
    sceKernelUnlockMutex(log_mutex, 1);
}

void vjo_log(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = sceClibVsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    vjo_log_raw(line, n);
}
