/* AnkiConnect thread: the duplicate pre-check of each new result, × adds
 * (with a screenshot of the whole game frame and the word's audio), and
 * finding Anki on the network for anki_host = auto. Nothing here runs
 * while anki_host is empty.
 *
 * Its memory is a memblock allocated when work arrives and freed when the
 * mailbox is empty. The result shown in the overlay is only read under the
 * view lock, after checking that it is still the published one (the
 * control thread frees result memory under that lock). */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

#include "../core/anki.h"
#include "shell.h"

#define ANKI_MEM_SIZE (384 * 1024)
#define PICTURE_QUALITY 75
#define SCAN_WINDOW_US 350000 /* per batch of VJO_NET_PROBE_MAX hosts */

#define EV_WORK 1u
#define EV_QUIT 2u

static SceUID box_lock = -1, evf = -1, thread = -1;
static volatile int running;

/* mailbox (box_lock) */
static VjoConfig next_cfg;
static int want_check, want_add;
static unsigned check_seq, add_seq;
static int add_entry;

/* Anki thread only */
static VjoConfig acfg;
static VjoPlatform plat;
static SceUID mem_uid = -1;
static VjoArena arena;
static char saved_host[64]; /* anki_host.txt (auto) */
static int saved_port, saved_loaded;
static int scanned; /* searched the network since boot */

static int64_t now_us(void)
{
    return (int64_t)sceKernelGetProcessTimeWide();
}

/* ---------------- mailbox ---------------- */

void vjo_anki_configure(const VjoConfig *cfg)
{
    if (box_lock < 0)
        return;
    sceKernelLockMutex(box_lock, 1, NULL);
    next_cfg = *cfg;
    sceKernelUnlockMutex(box_lock, 1);
}

void vjo_anki_post_check(unsigned list_seq)
{
    if (box_lock < 0)
        return;
    sceKernelLockMutex(box_lock, 1, NULL);
    want_check = 1; /* replaces a queued one */
    check_seq = list_seq;
    sceKernelUnlockMutex(box_lock, 1);
    sceKernelSetEventFlag(evf, EV_WORK);
}

void vjo_anki_post_add(unsigned list_seq, int entry)
{
    if (box_lock < 0)
        return;
    sceKernelLockMutex(box_lock, 1, NULL);
    if (!want_add) { /* one at a time: a second × while adding is ignored */
        want_add = 1;
        add_seq = list_seq;
        add_entry = entry;
    }
    sceKernelUnlockMutex(box_lock, 1);
    sceKernelSetEventFlag(evf, EV_WORK);
}

/* ---------------- view ---------------- */

/* Status (and, with mark_entry >= 0, that entry's ✓) for list seq; dropped
 * when another list is shown by now. */
static void publish(unsigned seq, const char *msg, int kind, int mark_entry)
{
    vjo_view_lock();
    if (g_view.list_seq == seq) {
        if (mark_entry >= 0 && mark_entry < VJO_MAX_ENTRIES) {
            if (g_view.anki_marks_seq != seq) {
                sceClibMemset(g_view.anki_mark, 0, sizeof(g_view.anki_mark));
                g_view.anki_marks_seq = seq;
            }
            g_view.anki_mark[mark_entry] = 1;
            /* the word's other occurrences in the text too */
            if (g_view.list && mark_entry < g_view.list->n_entries) {
                const VjoVocab *v = g_view.list->entries[mark_entry].vocab;
                for (int i = 0; i < g_view.list->n_entries && i < VJO_MAX_ENTRIES; i++)
                    if (g_view.list->entries[i].vocab == v)
                        g_view.anki_mark[i] = 1;
            }
        }
        sceClibSnprintf(g_view.anki_status, sizeof(g_view.anki_status), "%s", msg);
        g_view.anki_status_kind = kind;
        g_view.anki_version++;
    }
    vjo_view_unlock();
}

static void publish_error(unsigned seq, const VjoErr *err)
{
    size_t mark = vjo_arena_mark(&arena);
    int offline = err->rc == VJO_E_NET || err->rc == VJO_E_NOT_FOUND;
    publish(seq, vjo_anki_err_text(&arena, err), offline ? VJO_ANKI_STATUS_DIM : VJO_ANKI_STATUS_ERROR, -1);
    vjo_arena_release(&arena, mark);
}

/* ---------------- memory ---------------- */

static int mem_get(void)
{
    void *base = NULL;
    if (mem_uid >= 0) {
        vjo_arena_reset(&arena);
        return 0;
    }
    mem_uid = sceKernelAllocMemBlock("VjoAnkiMem", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ANKI_MEM_SIZE, NULL);
    if (mem_uid < 0) {
        vjo_log("anki: memblock (%d KiB) failed 0x%08X", ANKI_MEM_SIZE >> 10, mem_uid);
        return -1;
    }
    sceKernelGetMemBlockBase(mem_uid, &base);
    vjo_arena_init(&arena, base, ANKI_MEM_SIZE);
    return 0;
}

static void mem_put(void)
{
    if (mem_uid >= 0) {
        vjo_log("anki: arena peak %u", (unsigned)arena.peak);
        sceKernelFreeMemBlock(mem_uid);
    }
    mem_uid = -1;
    vjo_arena_init(&arena, NULL, 0);
}

/* ---------------- finding Anki ---------------- */

static void load_saved(void)
{
    size_t mark, len = 0;
    char *t;
    if (saved_loaded)
        return;
    saved_loaded = 1;
    mark = vjo_arena_mark(&arena);
    t = vjo_file_read(&arena, VJO_ANKI_HOST_PATH, &len);
    if (t) {
        while (len && (uint8_t)t[len - 1] <= ' ')
            t[--len] = '\0';
        if (vjo_anki_endpoint(t, saved_host, sizeof(saved_host), &saved_port) != VJO_ANKI_MANUAL)
            saved_host[0] = '\0';
    }
    vjo_arena_release(&arena, mark);
}

static void save_host(const char *host, int port)
{
    char line[80];
    int n = sceClibSnprintf(line, sizeof(line), "%s:%d\n", host, port);
    sceClibSnprintf(saved_host, sizeof(saved_host), "%s", host);
    saved_port = port;
    vjo_file_write(VJO_ANKI_HOST_PATH, line, (size_t)n);
}

static void ip_text(uint32_t ip, char *out, size_t cap)
{
    sceClibSnprintf(out, cap, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 255,
                    (unsigned)(ip >> 8) & 255, (unsigned)ip & 255);
}

/* Searches the local /24 for AnkiConnect; the first that answers wins. */
static int scan(char *host, size_t cap, int *port)
{
    uint32_t ip, mask, cand[254];
    uint8_t open[VJO_NET_PROBE_MAX];
    int n, n_open = 0;
    int64_t t0 = now_us();
    char net[16];
    if (vjo_net_local_ipv4(&ip, &mask) < 0) {
        vjo_log("anki: scan: no IP address");
        return -1;
    }
    n = vjo_anki_scan_candidates(ip, mask, cand, 254);
    ip_text(ip, net, sizeof(net));
    for (int b = 0; b < n; b += VJO_NET_PROBE_MAX) {
        int k = vjo_net_probe_port(cand + b, n - b, VJO_ANKI_PORT, SCAN_WINDOW_US, open);
        for (int i = 0; i < k; i++) {
            VjoErr err;
            if (!open[i])
                continue;
            n_open++;
            ip_text(cand[b + i], host, cap);
            if (vjo_anki_probe(&arena, &plat, host, VJO_ANKI_PORT, &err) == VJO_OK) {
                *port = VJO_ANKI_PORT;
                vjo_log("anki: scan from %s -> %s in %d ms", net, host, (int)((now_us() - t0) / 1000));
                return 0;
            }
            vjo_log("anki: %s:%d is open but not AnkiConnect (%d)", host, VJO_ANKI_PORT, err.rc);
        }
    }
    vjo_log("anki: scan from %s (mask %08X, %d hosts): not found, %d open, %d ms", net, (unsigned)mask, n, n_open,
            (int)((now_us() - t0) / 1000));
    return -1;
}

/* Runs op on the endpoint: anki_host, or (auto) the saved host, then a
 * network search if that fails and may_scan (or nothing was searched since
 * boot). Returns op's result, VJO_E_NET or VJO_E_NOT_FOUND. */
typedef int (*AnkiOp)(const char *host, int port, void *ud, VjoErr *err);

static int with_anki(int may_scan, AnkiOp op, void *ud, VjoErr *err)
{
    char host[64];
    int port, mode = vjo_anki_endpoint(acfg.anki_host, host, sizeof(host), &port), rc;
    sceClibMemset(err, 0, sizeof(*err));
    if (mode == VJO_ANKI_MANUAL)
        return op(host, port, ud, err);
    if (mode != VJO_ANKI_AUTO)
        return err->rc = VJO_E_NET;
    load_saved();
    if (saved_host[0]) {
        rc = op(saved_host, saved_port, ud, err);
        if (!vjo_anki_host_stale(rc))
            return rc;
        vjo_log("anki: saved host %s:%d failed (%d)", saved_host, saved_port, rc);
    }
    if (scanned && !may_scan)
        return err->rc = VJO_E_NET;
    scanned = 1;
    if (scan(host, sizeof(host), &port) < 0) {
        sceClibMemset(err, 0, sizeof(*err));
        return err->rc = VJO_E_NOT_FOUND;
    }
    save_host(host, port);
    return op(host, port, ud, err);
}

/* ---------------- pre-check ---------------- */

typedef struct {
    const char *json;
    uint8_t *marks;
    int n;
} CheckOp;

static int op_check(const char *host, int port, void *ud, VjoErr *err)
{
    CheckOp *c = (CheckOp *)ud;
    return vjo_anki_check(&arena, &plat, host, port, c->json, c->marks, c->n, err);
}

static void do_check(unsigned seq)
{
    static uint8_t marks[VJO_MAX_ENTRIES];
    CheckOp c = {NULL, marks, 0};
    VjoErr err;
    int n_marked = 0;
    int64_t t0 = now_us();

    vjo_view_lock();
    if (g_view.open && g_view.list_seq == seq && g_view.list && g_view.list->n_entries > 0) {
        c.n = g_view.list->n_entries < VJO_MAX_ENTRIES ? g_view.list->n_entries : VJO_MAX_ENTRIES;
        c.json = vjo_anki_can_add_request(&arena, &acfg, g_view.list, c.n); /* NULL: no word field */
    }
    vjo_view_unlock();
    if (!c.json)
        return;
    if (with_anki(0, op_check, &c, &err)) {
        vjo_log("anki: check failed %d %s", err.rc, err.detail ? err.detail : "");
        publish_error(seq, &err);
        return;
    }
    vjo_view_lock();
    if (g_view.list_seq == seq) {
        sceClibMemset(g_view.anki_mark, 0, sizeof(g_view.anki_mark));
        sceClibMemcpy(g_view.anki_mark, marks, (size_t)c.n);
        g_view.anki_marks_seq = seq;
        g_view.anki_status[0] = '\0';
        g_view.anki_version++;
    }
    vjo_view_unlock();
    for (int i = 0; i < c.n; i++)
        n_marked += marks[i];
    vjo_log("anki: check %d words, %d in Anki, %d ms", c.n, n_marked, (int)((now_us() - t0) / 1000));
}

/* ---------------- add ---------------- */

typedef struct {
    const VjoAnkiNote *note;
    const VjoAnkiMedia *media;
} AddOp;

static int op_add(const char *host, int port, void *ud, VjoErr *err)
{
    AddOp *o = (AddOp *)ud;
    return vjo_anki_add(&arena, &plat, host, port, &acfg, o->note, o->media, err);
}

static uint64_t unix_ms(void)
{
    SceRtcTick t;
    if (sceRtcGetCurrentTick(&t) < 0)
        return 0;
    return t.tick / 1000ull - 62135596800000ull; /* microseconds since 0001-01-01 UTC */
}

/* The game's frame when × was pressed, into m. */
static void take_screenshot(VjoAnkiMedia *m, char *name, size_t cap)
{
    VjoState st;
    VjoBuf jb;
    int64_t t0 = now_us();
    int rc;
    vjo_buf_init(&jb, &arena);
    rc = vjo_capture_jpeg(&arena, VJO_CAPTURE_FULL, PICTURE_QUALITY, &jb, &st);
    if (rc != VJO_OK) {
        vjo_log("anki: screenshot failed %d: adding without it", rc);
        return;
    }
    vjo_anki_media_name(name, cap, unix_ms(), "jpg");
    m->picture = jb.data;
    m->picture_len = jb.len;
    m->picture_name = name;
    vjo_log("anki: screenshot %ux%u -> %u bytes in %d ms", st.width, st.height, (unsigned)jb.len,
            (int)((now_us() - t0) / 1000));
}

static void do_add(unsigned seq, int entry)
{
    VjoAnkiNote note;
    VjoAnkiMedia media;
    AddOp o = {&note, &media};
    VjoErr err, audio_err;
    char picture_name[40], msg[sizeof(g_view.anki_status)];
    int ok = 0, marked = 0, rc, want_picture, want_audio;
    int64_t t0 = now_us();

    vjo_view_lock();
    if (g_view.open && g_view.list_seq == seq && g_view.list && entry >= 0 && entry < g_view.list->n_entries) {
        marked = entry < VJO_MAX_ENTRIES && g_view.anki_marks_seq == seq && g_view.anki_mark[entry];
        ok = marked || vjo_anki_note_from_entry(&arena, g_view.list, entry, &note) == 0;
    }
    vjo_view_unlock();
    if (!ok)
        return;
    if (marked)
        return; /* the ✓ already says so */
    publish(seq, "Adding…", VJO_ANKI_STATUS_DIM, -1);

    sceClibMemset(&media, 0, sizeof(media));
    want_picture = acfg.anki_field[VJO_ANKI_PICTURE][0] != '\0';
    want_audio = vjo_anki_audio_enabled(&acfg);
    if (want_picture) /* first: the frame of the × press */
        take_screenshot(&media, picture_name, sizeof(picture_name));
    if (want_audio) {
        int64_t ta = now_us();
        vjo_anki_find_audio(&arena, &plat, &acfg, &note, unix_ms(), &media, &audio_err);
        vjo_log("anki: audio %s: %s (rc=%d) in %d ms", note.spelling,
                media.audio_url ? media.audio_url : vjo_anki_audio_err_text(&arena, &audio_err), audio_err.rc,
                (int)((now_us() - ta) / 1000));
    }
    rc = with_anki(1, op_add, &o, &err);
    vjo_log("anki: add %s rc=%d %s in %d ms", note.spelling, rc, err.detail ? err.detail : "",
            (int)((now_us() - t0) / 1000));
    if (rc != VJO_OK && rc != VJO_E_ANKI_DUPLICATE) {
        publish_error(seq, &err);
        return;
    }
    /* The ✓ is the confirmation; only something missing is worth a line. */
    msg[0] = '\0';
    if (rc == VJO_OK) {
        int no_picture = want_picture && !media.picture_len, no_audio = want_audio && !media.audio_url;
        const char *why = no_audio ? vjo_anki_audio_err_text(&arena, &audio_err) : "";
        if (no_picture && no_audio)
            sceClibSnprintf(msg, sizeof(msg), "Added without a screenshot or audio (%s)", why);
        else if (no_picture)
            sceClibSnprintf(msg, sizeof(msg), "Added without a screenshot (the capture failed)");
        else if (no_audio)
            sceClibSnprintf(msg, sizeof(msg), "Added without audio (%s)", why);
    }
    publish(seq, msg, VJO_ANKI_STATUS_DIM, entry);
}

/* ---------------- thread ---------------- */

static int anki_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (running) {
        unsigned int bits = 0;
        sceKernelWaitEventFlag(evf, EV_WORK | EV_QUIT, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, NULL);
        if (bits & EV_QUIT)
            break;
        for (;;) {
            int check = 0, add = 0, entry = 0;
            unsigned cseq = 0, aseq = 0;
            sceKernelLockMutex(box_lock, 1, NULL);
            acfg = next_cfg;
            if (want_check) {
                check = 1;
                cseq = check_seq;
                want_check = 0;
            } else if (want_add) {
                add = 1;
                aseq = add_seq;
                entry = add_entry;
            }
            sceKernelUnlockMutex(box_lock, 1);
            if (!check && !add)
                break;
            if (!running)
                break;
            if (acfg.anki_host[0] && mem_get() == 0) {
                if (check)
                    do_check(cseq);
                else
                    do_add(aseq, entry);
            } else if (add && acfg.anki_host[0]) {
                publish(aseq, "Anki: not enough memory", VJO_ANKI_STATUS_ERROR, -1);
            }
            if (add) {
                sceKernelLockMutex(box_lock, 1, NULL);
                want_add = 0; /* × works again */
                sceKernelUnlockMutex(box_lock, 1);
            }
        }
        mem_put();
    }
    mem_put();
    return 0;
}

int vjo_anki_start(void)
{
    vjo_platform_vita(&plat);
    vjo_arena_init(&arena, NULL, 0);
    box_lock = sceKernelCreateMutex("VjoAnkiBox", 0, 0, NULL);
    evf = sceKernelCreateEventFlag("VjoAnkiEv", 0, 0, NULL);
    if (box_lock < 0 || evf < 0) {
        vjo_anki_stop();
        return -1;
    }
    running = 1;
    thread = sceKernelCreateThread("VjoAnki", anki_main, 0x10000100, 0x10000, 0, 0, NULL); /* TLS, like VjoNet */
    if (thread < 0) {
        vjo_anki_stop();
        return -1;
    }
    sceKernelStartThread(thread, 0, NULL);
    return 0;
}

void vjo_anki_stop(void)
{
    running = 0;
    if (evf >= 0)
        sceKernelSetEventFlag(evf, EV_QUIT);
    if (thread >= 0) {
        sceKernelWaitThreadEnd(thread, NULL, NULL);
        sceKernelDeleteThread(thread);
    }
    thread = -1;
    if (evf >= 0)
        sceKernelDeleteEventFlag(evf);
    if (box_lock >= 0)
        sceKernelDeleteMutex(box_lock);
    evf = box_lock = -1;
}
