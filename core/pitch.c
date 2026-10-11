#include "pitch.h"

#include <string.h>

#include "utf.h"

int vjo_pitch_from_pattern(const char *pattern, size_t n)
{
    for (size_t i = 0; i + 1 < n; i++)
        if (pattern[i] == 'H' && pattern[i + 1] == 'L')
            return (int)i + 1;
    return 0;
}

int vjo_morae(const char *reading)
{
    static const uint32_t small[] = {0x3083, 0x3085, 0x3087, 0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x308E,
                                     0x30E3, 0x30E5, 0x30E7, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30EE};
    size_t n = strlen(reading), i = 0;
    int m = 0;
    while (i < n) {
        uint32_t cp = vjo_utf8_next(reading, n, &i);
        int is_small = 0;
        for (unsigned k = 0; k < sizeof(small) / sizeof(small[0]) && !is_small; k++)
            is_small = cp == small[k];
        m += !is_small;
    }
    return m;
}

/* A verb or i-adjective (adj-i, adj-ix), but not a noun that takes する. */
static int verb_or_adjective(const char *const *pos, int n)
{
    int verb = 0, suru = 0, noun = 0;
    for (int i = 0; i < n; i++) {
        const char *p = pos[i];
        if (!strncmp(p, "v1", 2) || !strncmp(p, "v5", 2) || !strncmp(p, "vk", 2) || !strncmp(p, "vz", 2) ||
            !strncmp(p, "adj-i", 5))
            verb = 1;
        else if (!strncmp(p, "vs", 2))
            verb = suru = 1;
        else if (!strcmp(p, "n"))
            noun = 1;
    }
    return verb && !(suru && noun);
}

const char *vjo_pitch_category(const char *reading, int downstep, const char *const *pos, int n_pos)
{
    if (downstep == 0)
        return "heiban";
    if (verb_or_adjective(pos, n_pos))
        return "kifuku";
    if (downstep == 1)
        return "atamadaka";
    if (downstep == vjo_morae(reading ? reading : ""))
        return "odaka";
    return "nakadaka";
}
