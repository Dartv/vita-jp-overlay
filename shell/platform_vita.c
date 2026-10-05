/* VjoPlatform for SceShell: SceNet sockets (SceShell has already initialized
 * the network stack), resolver DNS, RTC time and the kernel RNG; plus the
 * LAN probing used to find Anki. */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/rtc.h>

#include "shell.h"

#define NET_TIMEOUT_US (20 * 1000 * 1000)
#define CONNECT_POLL_US 20000

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

static void ipv4_addr(SceNetSockaddrIn *sin, uint32_t ip, int port)
{
    sceClibMemset(sin, 0, sizeof(*sin));
    sin->sin_len = sizeof(*sin);
    sin->sin_family = SCE_NET_AF_INET;
    sin->sin_port = sceNetHtons((unsigned short)port);
    sin->sin_addr.s_addr = sceNetHtonl(ip);
}

/* Non-blocking connects (SceNet has no connect timeout option): start one,
 * then repeat it, which reports EISCONN once the first one is done (or its
 * error). Both return 1 = connected, 0 = in progress, < 0 = failed. */
static int connect_start(int fd, const SceNetSockaddrIn *sin)
{
    int on = 1, ret;
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));
    ret = sceNetConnect(fd, (const SceNetSockaddr *)sin, sizeof(*sin));
    return ret == 0 ? 1 : (unsigned)ret == SCE_NET_ERROR_EINPROGRESS ? 0 : ret;
}

static int connect_poll(int fd, const SceNetSockaddrIn *sin)
{
    int ret = sceNetConnect(fd, (const SceNetSockaddr *)sin, sizeof(*sin));
    if (ret == 0 || (unsigned)ret == SCE_NET_ERROR_EISCONN)
        return 1;
    if ((unsigned)ret == SCE_NET_ERROR_EALREADY || (unsigned)ret == SCE_NET_ERROR_EINPROGRESS)
        return 0;
    return ret;
}

static int connect_timed(int fd, const SceNetSockaddrIn *sin, int timeout_us)
{
    int off = 0, ret = connect_start(fd, sin);
    int64_t end = (int64_t)sceKernelGetProcessTimeWide() + timeout_us;
    while (ret == 0) {
        if ((int64_t)sceKernelGetProcessTimeWide() >= end) {
            ret = SCE_NET_ERROR_ETIMEDOUT;
            break;
        }
        sceKernelDelayThread(CONNECT_POLL_US);
        ret = connect_poll(fd, sin);
    }
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &off, sizeof(off));
    return ret > 0 ? 0 : ret;
}

static int vita_connect(void *ud, const char *host, int port, int timeout_us, VjoConn *out)
{
    SceNetSockaddrIn sin;
    SceNetInAddr addr;
    int state = 0, fd, ret, timeout = NET_TIMEOUT_US;
    (void)ud;

    if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED) {
        vjo_log("net: not connected (state %d)", state);
        return VJO_E_NET;
    }
    /* An IP address (Anki on the LAN) needs no DNS. */
    if (sceNetInetPton(SCE_NET_AF_INET, host, &addr) != 1 && (ret = resolve(host, &addr)) < 0) {
        vjo_log("net: resolve %s failed 0x%08X", host, ret);
        return VJO_E_NET;
    }
    ipv4_addr(&sin, sceNetNtohl(addr.s_addr), port);
    fd = sceNetSocket("VjoSocket", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (fd < 0) {
        vjo_log("net: socket failed 0x%08X", fd);
        return VJO_E_NET;
    }
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout, sizeof(timeout));
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (timeout_us > 0)
        ret = connect_timed(fd, &sin, timeout_us);
    else
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

int vjo_net_probe_port(const uint32_t *ips, int n, int port, int window_us, uint8_t *open)
{
    int fds[VJO_NET_PROBE_MAX], pending = 0, n_fail = 0;
    int64_t end;
    if (n > VJO_NET_PROBE_MAX)
        n = VJO_NET_PROBE_MAX;
    for (int i = 0; i < n; i++) {
        SceNetSockaddrIn sin;
        int st;
        open[i] = 0;
        fds[i] = sceNetSocket("VjoProbe", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
        if (fds[i] < 0) {
            n_fail++;
            continue;
        }
        ipv4_addr(&sin, ips[i], port);
        st = connect_start(fds[i], &sin);
        if (st == 1)
            open[i] = 1;
        else if (st == 0)
            pending++;
    }
    if (n_fail)
        vjo_log("net: probe: %d of %d sockets failed", n_fail, n);
    end = (int64_t)sceKernelGetProcessTimeWide() + window_us;
    while (pending > 0 && (int64_t)sceKernelGetProcessTimeWide() < end) {
        sceKernelDelayThread(CONNECT_POLL_US);
        for (int i = 0; i < n; i++) {
            SceNetSockaddrIn sin;
            int st;
            if (fds[i] < 0 || open[i])
                continue;
            ipv4_addr(&sin, ips[i], port);
            st = connect_poll(fds[i], &sin);
            if (st == 0)
                continue;
            open[i] = st == 1;
            pending--;
            if (st < 0) {
                sceNetSocketClose(fds[i]);
                fds[i] = -1;
            }
        }
    }
    for (int i = 0; i < n; i++)
        if (fds[i] >= 0)
            sceNetSocketClose(fds[i]);
    return n;
}

static uint32_t netctl_ipv4(int code)
{
    SceNetCtlInfo info;
    SceNetInAddr a;
    sceClibMemset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(code, &info) < 0)
        return 0;
    if (sceNetInetPton(SCE_NET_AF_INET, code == SCE_NETCTL_INFO_GET_NETMASK ? info.netmask : info.ip_address, &a) != 1)
        return 0;
    return sceNetNtohl(a.s_addr);
}

int vjo_net_local_ipv4(uint32_t *ip, uint32_t *mask)
{
    *ip = netctl_ipv4(SCE_NETCTL_INFO_GET_IP_ADDRESS);
    *mask = netctl_ipv4(SCE_NETCTL_INFO_GET_NETMASK);
    return *ip ? 0 : -1;
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
