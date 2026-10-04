/* vjo-cli: runs the Vita JP Overlay pipeline on the Mac.
 *
 *   vjo-cli IMAGE.jpg [--dict jpdb|jiten] [--api-key KEY | --config config.ini]
 *           [--filter lines|none] [--record DIR] [--nav] [-v]
 *   vjo-cli --text "日本語" --api-key KEY ...
 *   vjo-cli --replay DIR [--dict jpdb|jiten] [--filter lines|none] [--nav]
 *   vjo-cli --print-default-config
 *
 * The API key may also come from $VJO_JPDB_KEY / $VJO_JITEN_KEY. --replay runs
 * the same pipeline on a fixture dir's recorded responses (see replay.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "net_posix.h"
#include "render.h"
#include "replay.h"

typedef struct {
    const uint8_t *data;
    size_t len;
} MemJpeg;

static int mem_read(void *ud, uint32_t off, void *dst, uint32_t len)
{
    MemJpeg *m = (MemJpeg *)ud;
    if ((size_t)off + len > m->len)
        return -1;
    memcpy(dst, m->data + off, len);
    return 0;
}

/* Width/height from the first SOFn marker. */
static int jpeg_size(const uint8_t *p, size_t n, uint32_t *w, uint32_t *h)
{
    size_t i = 2;
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8)
        return -1;
    while (i + 9 < n) {
        uint8_t m;
        size_t seg;
        if (p[i] != 0xFF)
            return -1;
        m = p[i + 1];
        seg = ((size_t)p[i + 2] << 8) | p[i + 3];
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = ((uint32_t)p[i + 5] << 8) | p[i + 6];
            *w = ((uint32_t)p[i + 7] << 8) | p[i + 8];
            return 0;
        }
        i += 2 + seg;
    }
    return -1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: vjo-cli IMAGE.jpg [--dict jpdb|jiten] [--api-key KEY | --config FILE]\n"
            "               [--filter lines|none] [--record DIR] [--nav] [--stats] [-v]\n"
            "       vjo-cli --text TEXT [options]\n"
            "       vjo-cli --replay DIR [options]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    static uint8_t mem[8u << 20], filemem[8u << 20];
    VjoArena a, fa; /* fa holds input files so a.peak is what the Vita needs */
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoPlatform plat;
    VjoOverlayData d;
    const char *image = NULL, *text = NULL, *replay = NULL, *config = NULL, *key = NULL, *dict = NULL;
    int nav = 0, stats = 0, rc;

    memset(mem, 0xA5, sizeof(mem)); /* like a reused arena on the Vita */
    vjo_arena_init(&a, mem, sizeof(mem));
    vjo_arena_init(&fa, filemem, sizeof(filemem));
    vjo_config_defaults(&cfg);
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(s, "--api-key") && more)
            key = argv[++i];
        else if (!strcmp(s, "--config") && more)
            config = argv[++i];
        else if (!strcmp(s, "--dict") && more)
            dict = argv[++i];
        else if (!strcmp(s, "--filter") && more)
            cfg.non_japanese_filter = strcmp(argv[++i], "none") ? 1 : 0;
        else if (!strcmp(s, "--record") && more)
            pp.record_dir = argv[++i];
        else if (!strcmp(s, "--text") && more)
            text = argv[++i];
        else if (!strcmp(s, "--replay") && more)
            replay = argv[++i];
        else if (!strcmp(s, "--nav"))
            nav = 1;
        else if (!strcmp(s, "--stats"))
            stats = 1;
        else if (!strcmp(s, "-v"))
            pp.verbose = 1;
        else if (!strcmp(s, "--print-default-config")) {
            fputs(vjo_config_default_text(), stdout);
            return 0;
        }
        else if (s[0] != '-' && !image)
            image = s;
        else
            usage();
    }
    if (config) {
        size_t len;
        char *ini = vjo_read_file(&fa, config, &len);
        if (!ini) {
            fprintf(stderr, "cannot read %s\n", config);
            return 1;
        }
        vjo_config_parse(&cfg, ini, len);
        for (int i = 0; i < cfg.n_warnings; i++)
            fprintf(stderr, "config: %s\n", cfg.warnings[i]);
    }
    if (replay)
        vjo_replay_config(&cfg, replay);
    if (dict) {
        cfg.dictionary = vjo_dict_find(dict);
        if (cfg.dictionary < 0)
            usage();
    }
    if (!key)
        key = getenv(vjo_dict_info(cfg.dictionary)->key_env); /* VJO_JPDB_KEY, VJO_JITEN_KEY */
    if (key)
        snprintf(cfg.api_key[cfg.dictionary], sizeof(cfg.api_key[cfg.dictionary]), "%s", key);

    posix_platform_init(&pp, &plat);
    memset(&d, 0, sizeof(d));

    if (replay) {
        vjo_replay_overlay(&a, &fa, replay, &cfg, &d);
    } else if (text) {
        vjo_overlay_from_text(&a, &plat, &cfg, text, &d);
    } else if (image) {
        size_t len;
        char *jpg = vjo_read_file(&fa, image, &len);
        MemJpeg m;
        VjoJpegSource src;
        if (!jpg) {
            fprintf(stderr, "cannot read %s\n", image);
            return 1;
        }
        m.data = (const uint8_t *)jpg;
        m.len = len;
        memset(&src, 0, sizeof(src));
        if (jpeg_size(m.data, len, &src.width, &src.height) < 0) {
            fprintf(stderr, "%s: not a baseline/progressive JPEG\n", image);
            return 1;
        }
        src.ud = &m;
        src.read = mem_read;
        src.size = (uint32_t)len;
        vjo_overlay_from_jpeg(&a, &plat, &cfg, &src, &d);
    } else {
        usage();
    }

    rc = d.err.rc ? 1 : 0;
    fputs(vjo_render_overlay(&a, &d), stdout);
    if (nav) {
        printf("\n[navigation]\n");
        for (int i = 0; i < d.list.n_entries; i++)
            printf("%d: %s\n", i + 1, vjo_render_highlight(&a, &d.list, i));
    }
    if (stats)
        fprintf(stderr, "arena peak: %lu bytes\n", (unsigned long)a.peak);
    return rc;
}
