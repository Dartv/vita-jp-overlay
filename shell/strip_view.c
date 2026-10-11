/* The subtitles strip's view model (VjoView.strip_*), written by the
 * control thread (worker.c) and drawn by strip.cpp. */
#include <psp2/kernel/clib.h>

#include "shell.h"

static int s_on, s_font_ja, s_font_en, s_room;

void vjo_strip_configure(int on, const VjoConfig *cfg)
{
    s_on = on;
    s_font_ja = cfg->font_size_ja;
    s_font_en = cfg->font_size_en;
    s_room = vjo_config_api_key(cfg)[0] != '\0';
}

/* Every change bumps strip_version (under the view lock). */
static void bump(void)
{
    __atomic_store_n(&g_view.strip_on, s_on, __ATOMIC_RELEASE); /* also read unlocked */
    g_view.font_size_ja = s_font_ja;
    g_view.font_size_en = s_font_en;
    g_view.strip_version++;
}

void vjo_strip_show(const char *text, int kind, int busy)
{
    size_t n = sceClibStrnlen(text, sizeof(g_view.strip_text) - 1);
    if (n == sizeof(g_view.strip_text) - 1)
        while (n && ((uint8_t)text[n] & 0xC0) == 0x80)
            n--;
    vjo_view_lock();
    if (kind != g_view.strip_kind || sceClibStrncmp(g_view.strip_text, text, n) || g_view.strip_text[n]) {
        sceClibMemcpy(g_view.strip_text, text, n);
        g_view.strip_text[n] = '\0';
        g_view.strip_kind = kind;
        g_view.strip_n_ruby = 0;
        g_view.strip_text_seq++;
    }
    g_view.strip_ruby_room = s_room;
    g_view.strip_busy = busy;
    bump();
    vjo_view_unlock();
}

void vjo_strip_set_busy(int busy)
{
    vjo_view_lock();
    g_view.strip_busy = busy;
    bump();
    vjo_view_unlock();
}

/* The text is read unlocked: the control thread is its only writer. */
void vjo_strip_show_readings(VjoArena *scratch, const VjoEntryList *words)
{
    static VjoStripRuby r[VJO_STRIP_RUBY_MAX];
    size_t mark = vjo_arena_mark(scratch);
    int n = g_view.strip_kind == VJO_STRIP_SENTENCE
                ? vjo_strip_rubies(scratch, g_view.strip_text, words, r, VJO_STRIP_RUBY_MAX)
                : 0;
    vjo_arena_release(scratch, mark);
    vjo_view_lock();
    sceClibMemcpy(g_view.strip_ruby, r, sizeof(r[0]) * (size_t)n);
    g_view.strip_n_ruby = n;
    g_view.strip_ruby_seq++;
    bump();
    vjo_view_unlock();
}
