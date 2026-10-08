/* Vita JP Overlay SceShell plugin: state shared between the control thread
 * (worker.c), the network thread and the ScePaf overlay (overlay.cpp). */
#ifndef VJO_SHELL_H
#define VJO_SHELL_H

#include <psp2/kernel/threadmgr.h>
#include <stdint.h>

#include "../core/client.h"
#include "../core/config.h"
#include "../include/vjo_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VJO_DATA_DIR    "ux0:data/VitaJPOverlay"
#define VJO_CONFIG_PATH VJO_DATA_DIR "/config.ini"
#define VJO_REGION_PATH VJO_DATA_DIR "/region.ini" /* per game, see core/regions.h */
#define VJO_LOG_PATH    VJO_DATA_DIR "/log.txt"
#define VJO_LOG_OLD_PATH VJO_DATA_DIR "/log.old.txt"
#define VJO_STATUS_PATH VJO_DATA_DIR "/status.txt"
#define VJO_ANKI_HOST_PATH VJO_DATA_DIR "/anki_host.txt" /* found by anki_host = auto */
#define VJO_RCO_PATH    "ur0:data/VitaJPOverlay/vitajpoverlay.rco"

#define VJO_MAX_ENTRIES 512 /* entries the overlay navigates (and Anki marks) */

enum { VJO_ANKI_STATUS_DIM = 0, VJO_ANKI_STATUS_ERROR = 1 };
/* VjoView.strip_kind */
enum { VJO_STRIP_SENTENCE = 0, VJO_STRIP_STATUS = 1, VJO_STRIP_ERROR = 2 };
#define VJO_STRIP_MAX 4096 /* bytes of UTF-8 */

/* ---- view model read by the overlay (under vjo_view_lock) ---- */
typedef struct {
    int open;                 /* overlay should be visible (stored atomically) */
    unsigned version;         /* bumps whenever content changes */
    unsigned list_seq;        /* bumps with every published list (Anki requests name it) */
    const VjoEntryList *list; /* NULL while recognizing / on error */
    char status[256];         /* "Recognizing…", an error, or "" */
    int status_is_error;
    char warnings[VJO_CONFIG_MAX_WARNINGS][96];
    int n_warnings;
    int font_size_ja;         /* config font_size_ja */
    int font_size_en;         /* config font_size_en */
    /* Anki (anki.c); changes bump anki_version only, so the overlay keeps the
     * header and the selection */
    int anki_enabled;         /* config anki_host is set */
    unsigned anki_version;
    char anki_status[128];    /* "Adding…", "Anki offline", an error, or "" */
    int anki_status_kind;     /* VJO_ANKI_STATUS_* */
    unsigned anki_marks_seq;  /* list_seq the marks are for */
    uint8_t anki_mark[VJO_MAX_ENTRIES]; /* 1 = already in Anki */
    /* subtitles: a strip at the top while the overlay is closed; changes
     * bump strip_version */
    int strip_on;             /* subtitles are on (stored atomically) */
    unsigned strip_version;
    char strip_text[VJO_STRIP_MAX]; /* "" = no strip drawn */
    int strip_kind;           /* VJO_STRIP_* */
    int strip_busy;           /* a recognition is running */
} VjoView;

extern VjoView g_view;
void vjo_view_lock(void);
void vjo_view_unlock(void);

/* ---- overlay -> control thread commands ---- */
enum {
    VJO_CMD_NONE = 0,
    VJO_CMD_CLOSED,          /* overlay closed by ○ */
    VJO_CMD_SET_REGION,      /* region selected (rect passed to vjo_post_command) */
    VJO_CMD_CLEAR_REGION,    /* hold □ */
};
void vjo_post_command(int cmd, const VjoRect *rect);

/* worker.c */
int vjo_worker_start(void);
void vjo_worker_stop(void);

/* capture.c */
int vjo_capture_init(void);
void vjo_capture_fini(void);
/* Captures the screen (flags: VJO_CAPTURE_*) and encodes it as a JPEG into
 * out; serialized with the OCR job's capture (one kernel buffer). Returns
 * VJO_OK or a VJO_E_* code. */
int vjo_capture_jpeg(VjoArena *a, uint32_t flags, int quality, VjoBuf *out, VjoState *st);

/* game.c */
/* The process's title ID; 0, or < 0 when not readable (yet). */
int vjo_title_id(SceUID pid, char *tid, int size);
/* vjoSetGameActive's mode (enum VjoGameMode) for a title ID. */
int vjo_title_game_mode(const char *tid);

/* anki.c: AnkiConnect thread */
int vjo_anki_start(void);
void vjo_anki_stop(void);
/* Settings for the next request (control thread, after each config load). */
void vjo_anki_configure(const VjoConfig *cfg);
/* Duplicate pre-check of the published list list_seq (control thread). */
void vjo_anki_post_check(unsigned list_seq);
/* Adds entry `entry` of list list_seq (overlay, × pressed). */
void vjo_anki_post_add(unsigned list_seq, int entry);

/* overlay.cpp (paf main thread) */
void vjo_overlay_init(void *plugin);

/* platform_vita.c */
/* Lets another thread abandon the requests of the thread using a platform
 * set up with it: vjo_net_cancel aborts the socket in use, and every
 * connect fails until vjo_net_cancel_clear. */
typedef struct {
    SceUID lock; /* the socket in use vs an abort */
    int fd;      /* -1: none */
    volatile int cancelled;
} VjoNetCancel;
int vjo_net_cancel_init(VjoNetCancel *c);
void vjo_net_cancel(VjoNetCancel *c);
void vjo_net_cancel_clear(VjoNetCancel *c);
/* cancel: NULL, or the VjoNetCancel of the thread using p. */
void vjo_platform_vita(VjoPlatform *p, VjoNetCancel *cancel);
/* This console's IPv4 address and netmask (host byte order); -1 if none. */
int vjo_net_local_ipv4(uint32_t *ip, uint32_t *mask);
/* Connects to port of up to VJO_NET_PROBE_MAX hosts (host byte order) at
 * once; open[i] = 1 for those that accept within window_us. Returns the
 * number of hosts tried. */
#define VJO_NET_PROBE_MAX 32
int vjo_net_probe_port(const uint32_t *ips, int n, int port, int window_us, uint8_t *open);

/* log_vita.c */
void vjo_log_configure(const VjoConfig *cfg);
void vjo_log(const char *fmt, ...);
void vjo_log_raw(const char *line, int len);
void vjo_status_reset(void);
void vjo_status_close(void);

/* files.c */
char *vjo_file_read(VjoArena *a, const char *path, size_t *len);
int vjo_file_write(const char *path, const void *data, size_t len);
void vjo_config_load(VjoConfig *cfg, VjoArena *scratch);
/* title_id's region (VJO_REGION_* from core/regions.h). */
int vjo_region_load(const char *title_id, VjoRect *out, VjoArena *scratch);
/* Saves title_id's region; NULL = the full screen. Other games' lines are
 * kept. */
int vjo_region_save(const char *title_id, const VjoRect *r, VjoArena *scratch);

#ifdef __cplusplus
}
#endif

#endif
