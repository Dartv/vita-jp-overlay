#include "replay.h"

#include <stdio.h>
#include <string.h>

char *vjo_read_file(VjoArena *a, const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *p;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (char *)vjo_arena_alloc(a, (size_t)n + 1);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n)
        p = NULL;
    fclose(f);
    if (!p)
        return NULL;
    p[n] = '\0';
    if (len)
        *len = (size_t)n;
    return p;
}

static int mem_send(void *ctx, const void *p, size_t n)
{
    VjoMemConn *m = (VjoMemConn *)ctx;
    if (m->out) {
        size_t k = m->out_cap - m->out_len < n ? m->out_cap - m->out_len : n;
        memcpy(m->out + m->out_len, p, k);
        m->out_len += k;
    }
    return (int)n;
}

static int mem_recv(void *ctx, void *p, size_t n)
{
    VjoMemConn *m = (VjoMemConn *)ctx;
    size_t k = m->len - m->pos;
    if (k > n)
        k = n;
    if (m->step && k > m->step)
        k = m->step;
    if (!k)
        return m->end_rc;
    memcpy(p, m->in + m->pos, k);
    m->pos += k;
    return (int)k;
}

void vjo_memconn_init(VjoMemConn *m, VjoConn *c)
{
    c->ctx = m;
    c->send = mem_send;
    c->recv = mem_recv;
}

const char *vjo_fixture_file(const char *host)
{
    return strstr(host, "jpdb") ? "jpdb.json" : strstr(host, "jiten") ? "jiten.json" : "lens.pb";
}

static int has_file(const char *dir, const char *name)
{
    char path[1024];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

void vjo_replay_config(VjoConfig *cfg, const char *dir)
{
    if (has_file(dir, "jiten.json"))
        cfg->dictionary = VJO_DICT_JITEN;
    else if (has_file(dir, "jpdb.json"))
        cfg->dictionary = VJO_DICT_JPDB;
    for (int i = 0; i < VJO_DICT_COUNT; i++)
        snprintf(cfg->api_key[i], sizeof(cfg->api_key[i]), "replay");
}

typedef struct {
    const char *dir;
    VjoArena *files;
    VjoMemConn conn;
} Replay;

static int replay_connect(void *ud, const char *host, int port, VjoConn *out)
{
    Replay *r = (Replay *)ud;
    char path[1024];
    size_t len;
    const char *body;
    VjoBuf resp;
    (void)port;
    snprintf(path, sizeof(path), "%s/%s", r->dir, vjo_fixture_file(host));
    body = vjo_read_file(r->files, path, &len);
    if (!body)
        return VJO_E_NET;
    vjo_buf_init(&resp, r->files);
    vjo_buf_printf(&resp, "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n", (unsigned long)len);
    vjo_buf_append(&resp, body, len);
    if (resp.oom)
        return VJO_E_NET;
    memset(&r->conn, 0, sizeof(r->conn));
    r->conn.in = (const char *)resp.data;
    r->conn.len = resp.len;
    vjo_memconn_init(&r->conn, out);
    return VJO_OK;
}

static void replay_disconnect(void *ud, VjoConn *c)
{
    (void)ud;
    (void)c;
}

static void replay_random(void *ud, void *buf, size_t n)
{
    (void)ud;
    memset(buf, 0, n);
}

static uint64_t replay_time(void *ud)
{
    (void)ud;
    return 0;
}

static int empty_jpeg(void *ud, uint32_t off, void *dst, uint32_t len)
{
    (void)ud;
    (void)off;
    (void)dst;
    (void)len;
    return -1;
}

int vjo_replay_overlay(VjoArena *a, VjoArena *files, const char *dir, const VjoConfig *cfg,
                       VjoOverlayData *out)
{
    Replay r;
    VjoPlatform p;
    VjoJpegSource src;
    memset(&r, 0, sizeof(r));
    r.dir = dir;
    r.files = files;
    memset(&p, 0, sizeof(p));
    p.ud = &r;
    p.connect = replay_connect;
    p.disconnect = replay_disconnect;
    p.random = replay_random;
    p.unix_time = replay_time;
    p.plain_http = 1;
    memset(&src, 0, sizeof(src));
    src.read = empty_jpeg;
    src.width = src.height = 1;
    return vjo_overlay_from_jpeg(a, &p, cfg, &src, out);
}
