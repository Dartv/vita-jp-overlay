/* The region (or the whole frame) as a JPEG: a kernel capture, then the
 * software encoder over its raw rows. */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "../core/jpegsw.h"
#include "shell.h"

#define CAPTURE_TIMEOUT_US 3000000

static SceUID capture_lock = -1; /* the kernel's one raw buffer: OCR job vs Anki screenshot */

static int64_t now_us(void)
{
    return (int64_t)sceKernelGetProcessTimeWide();
}

int vjo_capture_init(void)
{
    capture_lock = sceKernelCreateMutex("VjoCapture", 0, 0, NULL);
    return capture_lock < 0 ? -1 : 0;
}

void vjo_capture_fini(void)
{
    if (capture_lock >= 0)
        sceKernelDeleteMutex(capture_lock);
    capture_lock = -1;
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
