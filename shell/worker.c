/* Control thread (kernel events, overlay state, auto/on-press policy) and
 * network thread (capture -> JPEG -> Lens -> dictionary). */
#include <psp2/appmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "../core/jpegsw.h"
#include "../core/regions.h"
#include "shell.h"

/* Memory: two result arenas (the overlay shows one while the network thread
 * fills the other) in one memblock allocated while a game runs, plus a small
 * static scratch arena (control thread only) for config/region/messages. */
#define RESULT_ARENA_SIZE (384 * 1024)
#define SCRATCH_SIZE      (24 * 1024)
#define MEM_SIZE          (2 * RESULT_ARENA_SIZE)

#define POLL_TIMEOUT_US   50000
#define CAPTURE_TIMEOUT_US 3000000
#define BACKOFF_MIN_US    2000000LL
#define BACKOFF_MAX_US    60000000LL
#define JPEG_QUALITY      80
#define SUBTITLE_GAP_US   1000000LL /* between subtitle jobs */
#define UNSETTLED_US      2000000LL /* subtitles: a region that keeps changing is recognized this often */

VjoView g_view;

static SceUID view_lock = -1;
static SceUID cmd_lock = -1;
static SceUID capture_lock = -1; /* the kernel's one raw buffer: OCR job vs Anki screenshot */
static SceUID ctl_thread = -1, net_thread = -1;
static int threads_started;
static int anki_started; /* optional: the overlay runs without it */
static SceUID net_evf = -1;
static volatile int running = 1;

static SceUID mem_uid = -1;
static VjoArena results[2];
static VjoArena scratch; /* static; control thread only */
static uint8_t scratch_mem[SCRATCH_SIZE];
static int active = -1;               /* results[] index shown/cached */
static uint32_t cache_scene; /* VjoState.scene of the cached result, 0 = unknown */
static int cache_ok;
static int cache_dictionary; /* the cached lookup's VJO_DICT_* */
static VjoOverlayData cache_data[2];

static VjoConfig cfg;      /* control thread */
static VjoConfig job_cfg;  /* snapshot used by the network thread */
static VjoPlatform plat;
static char title_id[12];   /* set while a game is active ("" otherwise) */

/* Control-thread state. The overlay and the network job are independent: an
 * auto job runs while the overlay is closed, the overlay can open while a
 * job runs, and a result can be shown while the next job runs.
 *   overlay: OV_CLOSED, OV_OPEN, or OV_OPEN_OLD_JOB (opened while a job was
 *            already running: its result may be for an earlier screen, which
 *            is checked when it ends)
 *   job:     job_running (set here, finished via job_done from the network
 *            thread)
 *   subtitles: on/off (the strip shows each job's sentence while the
 *            overlay is closed)
 * plus inputs to the background policy: stable_pending (a REGION_STABLE
 * that has not been acted on yet) and the error backoffs, one per stage (a
 * failing dictionary does not stop the subtitles, which need only OCR).
 * Each event has one handler below (on_*). */
enum { OV_CLOSED, OV_OPEN, OV_OPEN_OLD_JOB };
static int ov = OV_CLOSED;
static int subtitles; /* written by set_subtitles only */
static int stable_pending;
typedef struct {
    int64_t until, us;
} Backoff;
static Backoff ocr_backoff, dict_backoff;
static int64_t job_started_us;

/* network job */
#define NET_EV_JOB  1u
#define NET_EV_QUIT 2u
static volatile int job_running;
static volatile int job_done;
static volatile int job_idx;
static volatile int job_rc;
static volatile uint32_t job_scene;
static volatile int job_lookup;     /* the job looks the words up (else OCR only) */
/* The cached result's text when the job started (NULL: no usable lookup).
 * Its arena is not touched while the job runs (the job fills the other). */
static const char *job_cached_text;
/* The job's text is job_cached_text: no lookup, the cached result stays
 * (and is now for the job's screen). */
static volatile int job_same_text;
/* cache_data[job_idx].sentence is ready (the lookup may still run): the
 * net thread only appends to the arena, so the control thread may read it */
static volatile int job_text_ready;

static volatile int pending_cmd;
static VjoRect pending_rect;

void vjo_view_lock(void)
{
    sceKernelLockMutex(view_lock, 1, NULL);
}

void vjo_view_unlock(void)
{
    sceKernelUnlockMutex(view_lock, 1);
}

void vjo_post_command(int cmd, const VjoRect *rect)
{
    sceKernelLockMutex(cmd_lock, 1, NULL);
    pending_cmd = cmd;
    if (rect)
        pending_rect = *rect;
    sceKernelUnlockMutex(cmd_lock, 1);
}

static int64_t now_us(void)
{
    return (int64_t)sceKernelGetProcessTimeWide();
}

/* Does a result for scene `a` show the current scene `b`? */
static int same_scene(uint32_t a, uint32_t b)
{
    return a != 0 && a == b;
}

/* ---------------- memory ---------------- */

static int mem_alloc(void)
{
    void *base = NULL;
    uint8_t *p;
    if (mem_uid >= 0)
        return 0;
    mem_uid = sceKernelAllocMemBlock("VjoShellMem", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                     (MEM_SIZE + 0xFFF) & ~0xFFF, NULL);
    if (mem_uid < 0) {
        vjo_log("shell memblock (%d KiB) failed 0x%08X", MEM_SIZE >> 10, mem_uid);
        return mem_uid;
    }
    sceKernelGetMemBlockBase(mem_uid, &base);
    p = (uint8_t *)base;
    vjo_arena_init(&results[0], p, RESULT_ARENA_SIZE);
    vjo_arena_init(&results[1], p + RESULT_ARENA_SIZE, RESULT_ARENA_SIZE);
    active = -1;
    cache_ok = 0;
    return 0;
}

static void mem_free(void)
{
    if (mem_uid >= 0) {
        vjo_view_lock();
        g_view.list = NULL;
        vjo_view_unlock();
        sceKernelFreeMemBlock(mem_uid);
    }
    mem_uid = -1;
    vjo_arena_init(&results[0], NULL, 0);
    vjo_arena_init(&results[1], NULL, 0);
    active = -1;
    cache_ok = 0;
}

/* ---------------- view ---------------- */

static void view_publish(int open, const VjoEntryList *list, const char *status, int is_error)
{
    vjo_view_lock();
    __atomic_store_n(&g_view.open, open, __ATOMIC_RELEASE); /* also read unlocked (overlay frame) */
    g_view.list = list;
    g_view.list_seq++;
    g_view.anki_enabled = anki_started && cfg.anki_host[0] != '\0';
    g_view.anki_status[0] = '\0'; /* the pre-check of a new list reports again */
    sceClibSnprintf(g_view.status, sizeof(g_view.status), "%s", status ? status : "");
    g_view.status_is_error = is_error;
    g_view.font_size_ja = cfg.font_size_ja;
    g_view.font_size_en = cfg.font_size_en;
    g_view.n_warnings = cfg.n_warnings;
    sceClibMemcpy(g_view.warnings, cfg.warnings, sizeof(g_view.warnings));
    g_view.version++;
    vjo_view_unlock();
}

static void view_show_cache(void)
{
    VjoOverlayData *d = &cache_data[active];
    size_t mark = vjo_arena_mark(&scratch);
    const char *err = d->err.rc ? vjo_err_text(&scratch, d->failed_stage, &d->err) : NULL;
    view_publish(1, &d->list, err, err != NULL);
    vjo_arena_release(&scratch, mark);
    if (anki_started && cfg.anki_host[0] && d->list.n_entries > 0) {
        unsigned seq;
        vjo_view_lock();
        seq = g_view.list_seq;
        vjo_view_unlock();
        vjo_anki_post_check(seq);
    }
}

/* Copies UTF-8 text, cut at a character boundary to fit. */
static void copy_utf8(char *dst, size_t cap, const char *src)
{
    size_t n = sceClibStrnlen(src, cap - 1);
    if (n == cap - 1)
        while (n && ((uint8_t)src[n] & 0xC0) == 0x80)
            n--;
    sceClibMemcpy(dst, src, n);
    dst[n] = '\0';
}

/* The strip: text (NULL keeps it) of kind VJO_STRIP_*, and the busy mark. */
static void strip_publish(const char *text, int kind, int busy)
{
    vjo_view_lock();
    __atomic_store_n(&g_view.strip_on, subtitles, __ATOMIC_RELEASE); /* also read unlocked */
    if (text) {
        copy_utf8(g_view.strip_text, sizeof(g_view.strip_text), text);
        g_view.strip_kind = kind;
    }
    g_view.strip_busy = busy;
    g_view.font_size_ja = cfg.font_size_ja;
    g_view.font_size_en = cfg.font_size_en;
    g_view.strip_version++;
    vjo_view_unlock();
}

/* ---------------- network thread ---------------- */

typedef struct {
    const uint8_t *data;
} MemJpeg;

static int mem_jpeg_read(void *ud, uint32_t off, void *dst, uint32_t len)
{
    sceClibMemcpy(dst, ((MemJpeg *)ud)->data + off, len);
    return 0;
}

static int raw_rows(void *ud, uint32_t row, uint32_t n, uint8_t *dst)
{
    (void)ud;
    return vjoReadRaw(row, n, dst);
}

int vjo_capture_jpeg(VjoArena *a, uint32_t flags, int quality, VjoBuf *out, VjoState *st)
{
    int seq, rc = VJO_OK;
    int64_t deadline, t0;

    /* Held until the last row is read: another request would invalidate them. */
    sceKernelLockMutex(capture_lock, 1, NULL);
    seq = vjoRequestCapture(flags);
    if (seq < 0) {
        vjo_log("capture request failed %d", seq);
        rc = seq == VJO_ERR_NO_MEMORY ? VJO_E_OOM : VJO_E_SOURCE;
        goto out;
    }
    /* CAPTURE_DONE may be left over from an earlier capture: the sequence
     * number decides which capture finished. */
    deadline = now_us() + CAPTURE_TIMEOUT_US;
    for (;;) {
        uint32_t bits = 0;
        st->size = sizeof(*st);
        vjoGetState(st);
        if (st->done_seq == (uint32_t)seq)
            break;
        if (now_us() >= deadline) {
            vjo_log("capture %d timed out", seq);
            rc = VJO_E_SOURCE;
            goto out;
        }
        vjoWaitEvent(VJO_EV_CAPTURE_DONE, &bits, 100000);
    }
    if (st->capture_result != 0) {
        vjo_log("capture failed %d", st->capture_result);
        rc = st->capture_result == VJO_ERR_NO_MEMORY ? VJO_E_OOM : VJO_E_SOURCE;
        goto out;
    }
    t0 = now_us();
    if (vjo_jpeg_encode(a, st->width, st->height, st->raw_stride, raw_rows, NULL, quality, out) < 0) {
        rc = out->oom ? VJO_E_OOM : VJO_E_SOURCE;
        goto out;
    }
    vjo_log("JPEG %ux%u -> %u bytes in %d ms", st->width, st->height, (unsigned)out->len,
            (int)((now_us() - t0) / 1000));
out:
    sceKernelUnlockMutex(capture_lock, 1);
    return rc;
}

static int run_job(VjoArena *a, VjoOverlayData *out, uint32_t *scene)
{
    VjoState st;
    VjoJpegSource src;
    VjoBuf jb;
    MemJpeg mem;

    sceClibMemset(out, 0, sizeof(*out));
    out->list.header = "";
    vjo_buf_init(&jb, a);
    st.capture_scene = 0;
    if ((out->err.rc = vjo_capture_jpeg(a, 0, JPEG_QUALITY, &jb, &st)) != VJO_OK) {
        out->failed_stage = VJO_STAGE_OCR;
        return out->err.rc;
    }
    *scene = st.capture_scene;
    sceClibMemset(&src, 0, sizeof(src));
    src.width = st.width;
    src.height = st.height;
    mem.data = jb.data;
    src.ud = &mem;
    src.read = mem_jpeg_read;
    src.size = (uint32_t)jb.len;
    if (vjo_overlay_ocr(a, &plat, &job_cfg, &src, out))
        return out->err.rc;
    /* The screen changed only in pixels (a mark, a cursor, an effect):
     * the cached lookup is this text's. */
    job_same_text = job_cached_text && out->filtered && !sceClibStrcmp(out->filtered, job_cached_text);
    __sync_synchronize(); /* the sentence is visible before job_text_ready */
    job_text_ready = 1;
    return job_lookup && !job_same_text ? vjo_overlay_lookup(a, &plat, &job_cfg, out) : VJO_OK;
}

static int net_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (running) {
        unsigned int bits = 0;
        sceKernelWaitEventFlag(net_evf, NET_EV_JOB | NET_EV_QUIT, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
                               &bits, NULL);
        if (bits & NET_EV_QUIT)
            break;
        if (bits & NET_EV_JOB) {
            int idx = job_idx;
            uint32_t scene = 0;
            int64_t t0 = now_us();
            vjo_arena_reset(&results[idx]);
            job_rc = run_job(&results[idx], &cache_data[idx], &scene);
            job_scene = scene;
            vjo_log("job %d done rc=%d in %d ms, arena peak %u", idx, job_rc,
                    (int)((now_us() - t0) / 1000), (unsigned)results[idx].peak);
            __sync_synchronize(); /* results are visible before job_done */
            job_done = 1;
        }
    }
    return 0;
}

/* lookup = 0: OCR only (the subtitles' sentence). */
static void start_job(const char *why, int lookup)
{
    if (job_running || mem_uid < 0)
        return;
    vjo_log("job start (%s)%s", why, lookup ? "" : ", OCR only");
    job_lookup = lookup;
    job_idx = active == 0 ? 1 : 0;
    job_cfg = cfg; /* the control thread may reload cfg while the job runs */
    job_cached_text = cache_ok && cache_dictionary == cfg.dictionary ? cache_data[active].filtered : NULL;
    job_same_text = 0;
    job_done = 0;
    job_text_ready = 0;
    job_running = 1;
    job_started_us = now_us();
    __sync_synchronize();
    sceKernelSetEventFlag(net_evf, NET_EV_JOB);
    if (subtitles)
        strip_publish(NULL, 0, 1);
}

/* ---------------- control thread ---------------- */

static void apply_config(void)
{
    vjo_config_load(&cfg, &scratch);
    vjo_log_configure(&cfg);
    vjo_anki_configure(&cfg);
    if (vjoSetTriggers(cfg.toggle_button, cfg.subtitle_button) < 0)
        vjo_log("kernel refused the buttons: keeping the previous ones");
    for (int i = 0; i < cfg.n_warnings; i++)
        vjo_log("config warning: %s", cfg.warnings[i]);
}

static void push_region(void)
{
    VjoRect r;
    int found = vjo_region_load(title_id, &r, &scratch);
    vjoSetRegion(found && r.w ? &r : NULL);
    vjo_log("region: %s%s", found == VJO_REGION_GAME ? "this game's" : found ? "all games'" : "none",
            found && r.w ? "" : ", full screen");
}

static int game_active(void)
{
    return title_id[0] != '\0';
}

static void open_overlay(void)
{
    VjoState st;
    ov = OV_OPEN;
    vjoSetInputBlock(1);
    if (mem_uid < 0 && mem_alloc() < 0) {
        view_publish(1, NULL, "Not enough memory for the overlay", 1);
        return;
    }
    apply_config(); /* settings are re-read on every open */
    if (!vjo_config_api_key(&cfg)[0]) {
        VjoErr e = {VJO_E_NO_KEY, 0, 0, NULL, cfg.dictionary};
        size_t mark = vjo_arena_mark(&scratch);
        view_publish(1, NULL, vjo_err_text(&scratch, VJO_STAGE_DICT, &e), 1);
        vjo_arena_release(&scratch, mark);
        return;
    }
    st.size = sizeof(st);
    vjoGetState(&st);
    if (st.alloc_status == VJO_ALLOC_FAIL) {
        view_publish(1, NULL, "Not enough memory to capture the screen", 1);
        return;
    }
    if (cfg.ocr_mode == VJO_OCR_AUTO && cache_ok && active >= 0 && same_scene(cache_scene, st.scene)) {
        vjo_log("open: background result");
        view_show_cache();
        return;
    }
    /* Why the background result can't be used (auto mode tuning). */
    if (cfg.ocr_mode == VJO_OCR_AUTO)
        vjo_log("open: %s (stable %u)",
                job_running ? "background job still running"
                : !cache_ok || active < 0 ? "no background result"
                                          : "screen changed since the background result",
                st.stable);
    view_publish(1, NULL, "Recognizing…", 0);
    if (job_running)
        ov = OV_OPEN_OLD_JOB;
    else
        start_job("overlay opened", 1);
}

static void close_overlay(void)
{
    if (ov == OV_CLOSED)
        return;
    ov = OV_CLOSED;
    vjoSetInputBlock(0);
    vjo_view_lock();
    __atomic_store_n(&g_view.open, 0, __ATOMIC_RELEASE);
    /* The next job may reuse the shown arena; an open republishes it. */
    g_view.list = NULL;
    g_view.version++;
    vjo_view_unlock();
}

static void backoff_note(Backoff *b, int failed)
{
    if (!failed) {
        b->us = 0;
        b->until = 0;
        return;
    }
    b->us = b->us ? b->us * 2 : BACKOFF_MIN_US;
    if (b->us > BACKOFF_MAX_US)
        b->us = BACKOFF_MAX_US;
    b->until = now_us() + b->us;
}

/* The strip for a finished job: its sentence, else its error. */
static void strip_show_result(const VjoOverlayData *d)
{
    size_t mark = vjo_arena_mark(&scratch);
    if (d->sentence)
        strip_publish(d->sentence, VJO_STRIP_SENTENCE, 0);
    else
        strip_publish(vjo_err_text(&scratch, d->failed_stage, &d->err), VJO_STRIP_ERROR, 0);
    vjo_arena_release(&scratch, mark);
}

/* The overlay was opened while the job ran: is the result for the screen
 * shown now, with its lookup? Else a new job. Returns 1 if one started. */
static int restart_old_job(int has_lookup)
{
    VjoState st;
    if (ov != OV_OPEN_OLD_JOB)
        return 0;
    ov = OV_OPEN;
    st.size = sizeof(st);
    vjoGetState(&st);
    if (same_scene(job_scene, st.scene) && has_lookup)
        return 0;
    start_job(has_lookup ? "result was for an earlier screen" : "the running job was OCR only", 1);
    return 1;
}

/* The network thread finished the job. */
static void on_job_done(void)
{
    int idx = job_idx, ocr_failed;
    VjoOverlayData *d = &cache_data[idx];
    __sync_synchronize(); /* pairs with the network thread's barrier */
    job_running = 0;
    job_done = 0;
    job_text_ready = 0;
    ocr_failed = d->failed_stage == VJO_STAGE_OCR;
    backoff_note(&ocr_backoff, ocr_failed);
    if (subtitles)
        strip_show_result(d);
    if (job_same_text) {
        /* The cached result's text: that result (its arena stays active)
         * is this screen's. A region change meanwhile cleared cache_ok, but
         * the lookup depends on the text only. */
        vjo_log("same text as the cached result: kept for this screen");
        vjo_view_lock();
        cache_ok = 1;
        cache_scene = job_scene;
        vjo_view_unlock();
        if (ov == OV_OPEN && !job_lookup)
            return; /* OCR only: the overlay already shows a result */
        if (!restart_old_job(1) && ov != OV_CLOSED)
            view_show_cache();
        return;
    }
    if (!ocr_failed && job_lookup)
        backoff_note(&dict_backoff, d->err.rc != VJO_OK);
    if (!job_lookup && ov == OV_OPEN) {
        /* OCR only, while the overlay shows a result or an error: that stays
         * (and its arena stays active); the strip took the sentence */
        return;
    }
    /* Publish the new arena; the old one becomes the next job's target. */
    vjo_view_lock();
    active = idx;
    cache_ok = d->err.rc == VJO_OK && job_lookup; /* the overlay needs the lookup */
    cache_dictionary = job_cfg.dictionary;
    cache_scene = job_scene;
    vjo_view_unlock();
    if (!restart_old_job(job_lookup) && ov != OV_CLOSED)
        view_show_cache();
}

/* Does a job started now look the words up? Yes when the overlay can show
 * them: it is open, or opens from the background result (auto). */
static int want_lookup(void)
{
    return vjo_config_api_key(&cfg)[0] && (ov != OV_CLOSED || cfg.ocr_mode == VJO_OCR_AUTO) &&
           now_us() >= dict_backoff.until;
}

/* The network thread posted the job's sentence (before its lookup). */
static void on_job_text(void)
{
    const char *sentence;
    __sync_synchronize(); /* pairs with the network thread's barrier */
    sentence = cache_data[job_idx].sentence;
    job_text_ready = 0;
    vjo_log("subtitle text ready in %d ms", (int)((now_us() - job_started_us) / 1000));
    if (subtitles && sentence)
        strip_publish(sentence, VJO_STRIP_SENTENCE, 1);
}

static void set_subtitles(int on)
{
    VjoState st;
    const VjoOverlayData *d = active >= 0 ? &cache_data[active] : NULL;
    subtitles = on;
    vjo_log("subtitles %s", on ? "on" : "off");
    if (!on) {
        strip_publish("", VJO_STRIP_SENTENCE, 0);
        return;
    }
    if (mem_uid < 0 && mem_alloc() < 0) {
        strip_publish("Not enough memory for subtitles", VJO_STRIP_ERROR, 0);
        return;
    }
    apply_config(); /* settings are re-read, as when the overlay opens */
    st.size = sizeof(st);
    vjoGetState(&st);
    if (st.alloc_status == VJO_ALLOC_FAIL) {
        strip_publish("Not enough memory to capture the screen", VJO_STRIP_ERROR, 0);
        return;
    }
    if (!job_running && d && d->sentence && same_scene(cache_scene, st.scene)) {
        strip_publish(d->sentence, VJO_STRIP_SENTENCE, 0);
        return;
    }
    strip_publish("Recognizing…", VJO_STRIP_STATUS, 1);
    start_job("subtitles on", want_lookup()); /* if one runs, its result comes */
}

/* subtitle_button: from the full overlay, everything closes. */
static void on_subtitle(void)
{
    if (!game_active())
        return;
    if (ov != OV_CLOSED) {
        close_overlay();
        if (subtitles)
            set_subtitles(0);
    } else {
        set_subtitles(!subtitles);
    }
}

/* A command from the overlay (paf main thread). */
static void on_command(void)
{
    int cmd;
    VjoRect r;
    sceKernelLockMutex(cmd_lock, 1, NULL);
    cmd = pending_cmd;
    r = pending_rect;
    pending_cmd = VJO_CMD_NONE;
    sceKernelUnlockMutex(cmd_lock, 1);

    switch (cmd) {
    case VJO_CMD_CLOSED:
        close_overlay();
        break;
    case VJO_CMD_SET_REGION:
    case VJO_CMD_CLEAR_REGION:
        if (!game_active()) { /* the game exited meanwhile */
            close_overlay();
            break;
        }
        if (cmd == VJO_CMD_SET_REGION)
            vjo_log("region set for %s: %u,%u,%u,%u", title_id, r.x, r.y, r.w, r.h);
        else
            vjo_log("region for %s: full screen", title_id);
        if (vjo_region_save(title_id, cmd == VJO_CMD_SET_REGION ? &r : NULL, &scratch) < 0)
            vjo_log("region.ini: write failed");
        push_region();
        cache_ok = 0;
        close_overlay();
        if (subtitles)
            start_job("region changed", want_lookup());
        break;
    }
}

/* ---- is the new foreground process a game? ----
 * Its title ID is often not readable right when the process starts, so the
 * lookup is retried for a few seconds. Unknown after that = a game. */
#define CLASSIFY_TIMEOUT_US 3000000

static SceUID pending_pid;
static int64_t pending_since;

int sceKernelGetProcessTitleId(SceUID pid, char *titleid, SceSize len); /* SceProcessmgr, not in headers */

static int lookup_title(SceUID pid, char *tid, int size)
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
static int title_game_mode(const char *tid)
{
    if (!sceClibStrcmp(tid, "NPXS10028"))
        return VJO_GAME_STATIC_FB;
    if (!sceClibStrncmp(tid, "NPXS", 4) || !sceClibStrncmp(tid, "main", 4) ||
        !sceClibStrncmp(tid, "VITASHELL", 9))
        return VJO_GAME_NONE;
    return VJO_GAME;
}

static void activate_game(SceUID pid, const char *tid, int mode)
{
    sceClibSnprintf(title_id, sizeof(title_id), "%s", tid);
    if (mem_uid < 0)
        mem_alloc();
    apply_config();
    push_region();
    backoff_note(&ocr_backoff, 0);
    backoff_note(&dict_backoff, 0);
    vjoSetGameActive(pid, mode);
    vjo_log("game %s started: dictionary %s (key %s), trigger %s, subtitles %s, ocr_mode %s", title_id,
            vjo_dict_name(cfg.dictionary), vjo_config_api_key(&cfg)[0] ? "set" : "MISSING",
            vjo_trigger_name(cfg.toggle_button), vjo_trigger_name(cfg.subtitle_button),
            cfg.ocr_mode == VJO_OCR_AUTO ? "auto" : "on_press");
}

static void classify_pending(void)
{
    char tid[32];
    SceUID pid = pending_pid;
    int ret = lookup_title(pid, tid, sizeof(tid));
    if (ret == 0) {
        pending_pid = 0;
        int mode = title_game_mode(tid);
        if (mode == VJO_GAME_NONE) {
            vjo_log("%s: system app, not a game", tid);
            vjoSetGameActive(pid, VJO_GAME_NONE);
        } else {
            activate_game(pid, tid, mode);
        }
    } else if (now_us() - pending_since > CLASSIFY_TIMEOUT_US) {
        pending_pid = 0;
        vjo_log("no title ID for 0x%X (0x%08X): treating it as a game", pid, ret);
        activate_game(pid, "GAME", VJO_GAME);
    }
}

static void on_game_exit(void)
{
    pending_pid = 0;
    close_overlay();
    if (subtitles)
        set_subtitles(0);
    /* Free memory unless the network thread still uses it. */
    if (!job_running)
        mem_free();
    title_id[0] = '\0';
}

/* A new foreground process: classified by classify_pending. */
static void on_game_start(void)
{
    VjoState st;
    stable_pending = 0;
    if (subtitles) /* over a game, without its exit: the strip goes */
        set_subtitles(0);
    st.size = sizeof(st);
    vjoGetState(&st);
    title_id[0] = '\0';
    pending_pid = st.game_pid;
    pending_since = now_us();
}

static void on_trigger(void)
{
    if (ov != OV_CLOSED)
        close_overlay();
    else if (game_active())
        open_overlay();
}

/* A background job for a settled screen: the auto prefetch (for an instant
 * overlay) and the subtitles' refresh (whatever ocr_mode says). */
static int background_wanted(void)
{
    return subtitles || (cfg.ocr_mode == VJO_OCR_AUTO && want_lookup());
}

static int background_throttled(void)
{
    int64_t t = now_us();
    return t < ocr_backoff.until || (subtitles && t < job_started_us + SUBTITLE_GAP_US);
}

/* A stable event that arrives while busy (or throttled) is kept, so the
 * newest screen is still recognized afterwards. With subtitles, a region
 * that never settles (a video behind the text) is recognized every
 * UNSETTLED_US anyway. */
static void auto_prefetch(void)
{
    VjoState st;
    int unsettled_due;
    if (ov != OV_CLOSED || job_running || !game_active() || !background_wanted() || background_throttled())
        return;
    unsettled_due = subtitles && now_us() - job_started_us >= UNSETTLED_US;
    if (!stable_pending && !unsettled_due)
        return;
    st.size = sizeof(st);
    vjoGetState(&st);
    if (stable_pending) {
        stable_pending = 0;
        if (st.stable && !(cache_ok && same_scene(cache_scene, st.scene)))
            start_job("auto: screen settled", want_lookup());
    } else if (st.unsettled_ms >= UNSETTLED_US / 1000) {
        start_job("subtitles: screen keeps changing", 0); /* no lookup every 2 s */
    }
}

static int ctl_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    /* The order of the handlers within one wakeup is part of the policy
     * (e.g. a trigger and a game start in the same wakeup). */
    while (running) {
        uint32_t bits = 0;
        vjoWaitEvent(VJO_EV_TRIGGER | VJO_EV_SUBTITLE | VJO_EV_REGION_STABLE | VJO_EV_GAME_START |
                         VJO_EV_GAME_EXIT,
                     &bits, POLL_TIMEOUT_US);

        if (bits & VJO_EV_GAME_EXIT)
            on_game_exit();
        if (bits & VJO_EV_GAME_START)
            on_game_start();
        if (pending_pid > 0)
            classify_pending();
        if (bits & VJO_EV_TRIGGER)
            on_trigger();
        if (bits & VJO_EV_SUBTITLE)
            on_subtitle();
        if (job_running && job_text_ready)
            on_job_text();
        if (job_running && job_done)
            on_job_done();
        if (pending_cmd != VJO_CMD_NONE)
            on_command();
        if (bits & VJO_EV_REGION_STABLE)
            stable_pending = 1;
        auto_prefetch();
        if (!game_active() && !job_running && mem_uid >= 0 && ov == OV_CLOSED)
            mem_free();
    }
    return 0;
}

int vjo_worker_start(void)
{
    int ver = vjoGetVersion(), rc;
    if (ver != VJO_API_VERSION) {
        vjo_log("kernel API version %d, expected %d: not starting", ver, VJO_API_VERSION);
        return -1;
    }
    rc = vjoRegisterShell();
    if (rc < 0) {
        vjo_log("kernel refused the shell registration %d: not starting", rc);
        return -1;
    }
    view_lock = sceKernelCreateMutex("VjoView", 0, 0, NULL);
    cmd_lock = sceKernelCreateMutex("VjoCmd", 0, 0, NULL);
    capture_lock = sceKernelCreateMutex("VjoCapture", 0, 0, NULL);
    net_evf = sceKernelCreateEventFlag("VjoNetEv", 0, 0, NULL);
    if (view_lock < 0 || cmd_lock < 0 || capture_lock < 0 || net_evf < 0) {
        vjo_worker_stop();
        return -1;
    }
    anki_started = vjo_anki_start() == 0;
    if (!anki_started)
        vjo_log("anki: thread failed to start: Anki is off");
    vjo_platform_vita(&plat);
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_arena_init(&results[0], NULL, 0);
    vjo_arena_init(&results[1], NULL, 0);
    apply_config(); /* logging, the trigger and Anki are set up before the first game */
    vjo_log("Vita JP Overlay shell started (kernel API %d)", ver);

    net_thread = sceKernelCreateThread("VjoNet", net_main, 0x10000100, 0x10000, 0, 0, NULL);
    ctl_thread = sceKernelCreateThread("VjoControl", ctl_main, 0x10000100, 0x4000, 0, 0, NULL);
    if (net_thread < 0 || ctl_thread < 0) {
        vjo_worker_stop();
        return -1;
    }
    sceKernelStartThread(net_thread, 0, NULL);
    sceKernelStartThread(ctl_thread, 0, NULL);
    threads_started = 1;
    return 0;
}

/* Also undoes a partial vjo_worker_start. */
void vjo_worker_stop(void)
{
    running = 0;
    if (net_evf >= 0)
        sceKernelSetEventFlag(net_evf, NET_EV_QUIT);
    if (threads_started) {
        sceKernelWaitThreadEnd(net_thread, NULL, NULL);
        sceKernelWaitThreadEnd(ctl_thread, NULL, NULL);
        threads_started = 0;
    }
    if (net_thread >= 0)
        sceKernelDeleteThread(net_thread);
    if (ctl_thread >= 0)
        sceKernelDeleteThread(ctl_thread);
    net_thread = ctl_thread = -1;
    vjo_anki_stop();
    anki_started = 0;
    mem_free();
    if (net_evf >= 0)
        sceKernelDeleteEventFlag(net_evf);
    if (cmd_lock >= 0)
        sceKernelDeleteMutex(cmd_lock);
    if (capture_lock >= 0)
        sceKernelDeleteMutex(capture_lock);
    if (view_lock >= 0)
        sceKernelDeleteMutex(view_lock);
    net_evf = cmd_lock = capture_lock = view_lock = -1;
}
