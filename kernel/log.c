/* Kernel log ring buffer. The worker thread writes it to
 * ux0:data/VitaJPOverlay/kernel.txt (see kernel_file_flush in main.c). */
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/cpu/spinlock.h>
#include <psp2kern/kernel/sysclib.h>
#include <stdarg.h>

#include "vjo_kernel.h"

#define RING_SIZE 4096

static char ring[RING_SIZE];
static uint32_t head, tail; /* monotonically increasing byte counters */
static SceKernelSpinlock lock; /* hooks run on every core */

void klog(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    int n;
    SceKernelIntrStatus st;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';

    st = ksceKernelSpinlockLowLockCpuSuspendIntr(&lock);
    for (int i = 0; i < n; i++) {
        ring[head % RING_SIZE] = line[i];
        head++;
    }
    if (head - tail > RING_SIZE)
        tail = head - RING_SIZE; /* drop oldest */
    ksceKernelSpinlockLowUnlockCpuResumeIntr(&lock, st);
}

int klog_read(char *dst, int len)
{
    int n = 0;
    SceKernelIntrStatus st = ksceKernelSpinlockLowLockCpuSuspendIntr(&lock);
    if (head - tail > RING_SIZE)
        tail = head - RING_SIZE;
    while (n < len && tail != head) {
        dst[n++] = ring[tail % RING_SIZE];
        tail++;
    }
    ksceKernelSpinlockLowUnlockCpuResumeIntr(&lock, st);
    return n;
}
