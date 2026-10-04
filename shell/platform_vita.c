/* VjoPlatform for SceShell: SceNet sockets (SceShell has already initialized
 * the network stack), resolver DNS, RTC time and the kernel RNG. */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/rtc.h>

#include "shell.h"

#define NET_TIMEOUT_US (20 * 1000 * 1000)

static int sock_send(void *ctx, const void *p, size_t n)
{
    int r = sceNetSend((int)(intptr_t)ctx, p, n, 0);
    return r > 0 ? r : -1;
}

static int sock_recv(void *ctx, void *p, size_t n)
{
    int r = sceNetRecv((int)(intptr_t)ctx, p, n, 0);
    return r >= 0 ? r : -1;
}

static int resolve(const char *host, SceNetInAddr *addr)
{
    int rid, ret;
    rid = sceNetResolverCreate("VjoResolver", NULL, 0);
    if (rid < 0)
        return rid;
    ret = sceNetResolverStartNtoa(rid, host, addr, 5 * 1000 * 1000, 2, 0);
    sceNetResolverDestroy(rid);
    return ret;
}

static int vita_connect(void *ud, const char *host, int port, VjoConn *out)
{
    SceNetSockaddrIn sin;
    int state = 0, fd, ret, timeout = NET_TIMEOUT_US;
    (void)ud;

    if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED) {
        vjo_log("net: not connected (state %d)", state);
        return VJO_E_NET;
    }
    sceClibMemset(&sin, 0, sizeof(sin));
    if ((ret = resolve(host, &sin.sin_addr)) < 0) {
        vjo_log("net: resolve %s failed 0x%08X", host, ret);
        return VJO_E_NET;
    }
    sin.sin_len = sizeof(sin);
    sin.sin_family = SCE_NET_AF_INET;
    sin.sin_port = sceNetHtons((unsigned short)port);
    fd = sceNetSocket("VjoSocket", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (fd < 0) {
        vjo_log("net: socket failed 0x%08X", fd);
        return VJO_E_NET;
    }
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout, sizeof(timeout));
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &timeout, sizeof(timeout));
    ret = sceNetConnect(fd, (SceNetSockaddr *)&sin, sizeof(sin));
    if (ret < 0) {
        vjo_log("net: connect %s failed 0x%08X", host, ret);
        sceNetSocketClose(fd);
        return VJO_E_NET;
    }
    out->ctx = (void *)(intptr_t)fd;
    out->send = sock_send;
    out->recv = sock_recv;
    return VJO_OK;
}

static void vita_disconnect(void *ud, VjoConn *c)
{
    (void)ud;
    sceNetSocketClose((int)(intptr_t)c->ctx);
}

static void vita_random(void *ud, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    (void)ud;
    /* sceKernelGetRandomNumber returns at most 64 bytes per call. */
    while (n) {
        size_t k = n > 64 ? 64 : n;
        sceKernelGetRandomNumber(p, k);
        p += k;
        n -= k;
    }
}

static uint64_t vita_time(void *ud)
{
    SceRtcTick t;
    int ret;
    (void)ud;
    ret = sceRtcGetCurrentTick(&t); /* microseconds since 0001-01-01 UTC */
    if (ret < 0) {
        /* 1970: certificate validation then fails with a TLS error. */
        vjo_log("rtc: sceRtcGetCurrentTick failed 0x%08X", ret);
        return 0;
    }
    return t.tick / 1000000ull - 62135596800ull;
}

static void vita_log(void *ud, const char *msg)
{
    (void)ud;
    vjo_log("%s", msg);
}

void vjo_platform_vita(VjoPlatform *p)
{
    sceClibMemset(p, 0, sizeof(*p));
    p->connect = vita_connect;
    p->disconnect = vita_disconnect;
    p->random = vita_random;
    p->unix_time = vita_time;
    p->log = vita_log;
}
