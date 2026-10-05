/* vjo-cli: runs the Vita JP Overlay pipeline on the Mac.
 *
 *   vjo-cli IMAGE.jpg [--dict jpdb|jiten] [--api-key KEY | --config config.ini]
 *           [--filter lines|none] [--record DIR] [--nav] [-v]
 *   vjo-cli --text "日本語" --api-key KEY ...
 *   vjo-cli --replay DIR [--dict jpdb|jiten] [--filter lines|none] [--nav]
 *   vjo-cli --print-default-config
 *   ... --anki HOST[:PORT] [--anki-deck DECK] [--anki-add N [--picture FILE.jpg]]
 *
 * --anki checks every entry against AnkiConnect (duplicates in the deck)
 * and --anki-add adds entry N (1-based, as --nav numbers them), with the
 * picture if given; the other anki_* settings come from --config.
 *
 * The API key may also come from $VJO_JPDB_KEY / $VJO_JITEN_KEY. --replay runs
 * the same pipeline on a fixture dir's recorded responses (see replay.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "anki.h"
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

/* --anki: duplicate check of every entry, then --anki-add's entry. */
static int run_anki(VjoArena *a, VjoArena *fa, const VjoPlatform *p, const VjoConfig *cfg, const char *endpoint,
                    const VjoEntryList *l, int add, const char *picture)
{
    char host[64];
    int port, rc = 0;
    uint8_t *marks;
    const char *request;
    VjoErr err;
    if (vjo_anki_endpoint(endpoint, host, sizeof(host), &port) != VJO_ANKI_MANUAL) {
        fprintf(stderr, "--anki: expected HOST[:PORT]\n");
        return 1;
    }
    if (vjo_anki_probe(a, p, host, port, &err)) {
        printf("\n[anki] %s:%d: %s\n", host, port, vjo_anki_err_text(a, &err));
        return 1;
    }
    printf("\n[anki] AnkiConnect at %s:%d, deck \"%s\", note type \"%s\"\n", host, port, cfg->anki_deck,
           cfg->anki_note_type);
    marks = (uint8_t *)vjo_arena_zalloc(a, (size_t)l->n_entries + 1);
    request = vjo_anki_can_add_request(a, cfg, l, l->n_entries);
    if (request && (rc = vjo_anki_check(a, p, host, port, request, marks, l->n_entries, &err)) != VJO_OK)
        printf("check: %s\n", vjo_anki_err_text(a, &err));
    for (int i = 0; i < l->n_entries; i++)
        printf("%d: %s%s\n", i + 1, l->entries[i].vocab->spelling, marks[i] ? " ✓ in Anki" : "");
    if (add) {
        VjoAnkiNote n;
        char name[40] = "";
        size_t jlen = 0;
        const uint8_t *jpeg = NULL;
        if (vjo_anki_note_from_entry(a, l, add - 1, &n) < 0) {
            fprintf(stderr, "--anki-add: no entry %d\n", add);
            return 1;
        }
        if (picture) {
            struct timeval tv;
            if (!(jpeg = (const uint8_t *)vjo_read_file(fa, picture, &jlen))) {
                fprintf(stderr, "cannot read %s\n", picture);
                return 1;
            }
            gettimeofday(&tv, NULL);
            vjo_anki_picture_name(name, sizeof(name), (uint32_t)tv.tv_sec, (uint32_t)(tv.tv_usec / 1000));
        }
        if (vjo_anki_add(a, p, host, port, cfg, &n, jpeg, jlen, name, &err)) {
            printf("add %s: %s\n", n.spelling, vjo_anki_err_text(a, &err));
            return 1;
        }
        printf("Added: %s%s%s\n", n.spelling, jpeg ? " with " : "", name);
    }
    return rc ? 1 : 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: vjo-cli IMAGE.jpg [--dict jpdb|jiten] [--api-key KEY | --config FILE]\n"
            "               [--filter lines|none] [--record DIR] [--nav] [--stats] [-v]\n"
            "       vjo-cli --text TEXT [options]\n"
            "       vjo-cli --replay DIR [options]\n"
            "       ... --anki HOST[:PORT] [--anki-deck DECK] [--anki-add N [--picture FILE.jpg]]\n");
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
    const char *anki = NULL, *anki_deck = NULL, *picture = NULL;
    int nav = 0, stats = 0, anki_add = 0, rc;

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
        else if (!strcmp(s, "--anki") && more)
            anki = argv[++i];
        else if (!strcmp(s, "--anki-deck") && more)
            anki_deck = argv[++i];
        else if (!strcmp(s, "--anki-add") && more) {
            anki_add = atoi(argv[++i]);
            if (anki_add < 1)
                usage();
        }
        else if (!strcmp(s, "--picture") && more)
            picture = argv[++i];
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
    if (anki_deck)
        snprintf(cfg.anki_deck, sizeof(cfg.anki_deck), "%s", anki_deck);
    if ((anki_add || picture) && !anki)
        usage();
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
    if (anki && run_anki(&a, &fa, &plat, &cfg, anki, &d.list, anki_add, picture))
        rc = 1;
    if (stats)
        fprintf(stderr, "arena peak: %lu bytes\n", (unsigned long)a.peak);
    return rc;
}
