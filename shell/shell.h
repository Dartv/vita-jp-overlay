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
#define VJO_REGION_PATH VJO_DATA_DIR "/region.ini" /* one region for all games */
#define VJO_LOG_PATH    VJO_DATA_DIR "/log.txt"
#define VJO_LOG_OLD_PATH VJO_DATA_DIR "/log.old.txt"
#define VJO_STATUS_PATH VJO_DATA_DIR "/status.txt"
#define VJO_RCO_PATH    "ur0:data/VitaJPOverlay/vitajpoverlay.rco"

/* ---- view model read by the overlay (under vjo_view_lock) ---- */
typedef struct {
    int open;                 /* overlay should be visible (stored atomically) */
    unsigned version;         /* bumps whenever content changes */
    const VjoEntryList *list; /* NULL while recognizing / on error */
    char status[256];         /* "Recognizing…", an error, or "" */
    int status_is_error;
    char warnings[VJO_CONFIG_MAX_WARNINGS][96];
    int n_warnings;
    int font_size_ja;         /* config font_size_ja */
    int font_size_en;         /* config font_size_en */
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

/* overlay.cpp (paf main thread) */
void vjo_overlay_init(void *plugin);

/* platform_vita.c */
void vjo_platform_vita(VjoPlatform *p);

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
int vjo_region_load(VjoRect *out, VjoArena *scratch);
int vjo_region_save(const VjoRect *r); /* NULL = full screen */

#ifdef __cplusplus
}
#endif

#endif
