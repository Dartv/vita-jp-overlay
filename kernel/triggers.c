/* Trigger detection and hiding (see triggers.h). */
#include "triggers.h"

int trig_edge(TrigEdge *e, uint32_t mask, uint32_t other, uint32_t buttons)
{
    int pressed = mask && (buttons & mask) == mask, fire;
    int subset = mask && (mask & other) == mask && mask != other;
    if (subset) {
        if ((buttons & other) == other)
            e->superset_seen = 1;
        fire = !pressed && e->prev_pressed && !e->superset_seen;
        if (!pressed)
            e->superset_seen = 0;
    } else {
        fire = pressed && !e->prev_pressed;
    }
    e->prev_pressed = pressed;
    return fire;
}

/* Returns the combo buttons to hide in this sample. */
static uint32_t hold_step(TrigHold *h, uint32_t mask, uint32_t pos, int64_t now, int64_t delay)
{
    uint32_t part = pos & mask;
    if (!part) {
        if (h->start_us && !h->combo && !h->claimed && now - h->start_us < delay) {
            h->replay = h->buttons;
            h->replay_until_us = now + (delay > TRIG_REPLAY_MIN_US ? delay : TRIG_REPLAY_MIN_US);
        }
        h->start_us = 0;
        h->combo = 0;
        return 0;
    }
    if (!h->start_us) {
        h->start_us = now;
        h->buttons = 0;
        h->claimed = 0;
        h->replay = 0; /* pressed again: this press decides */
    }
    h->buttons |= part;
    if (part == mask)
        h->combo = 1;
    return h->combo || now - h->start_us < delay ? mask : 0;
}

uint32_t trig_filter(const TrigConfig *c, TrigHold *holds, uint32_t pos, int64_t now)
{
    uint32_t hide = 0, replay = 0;
    int delay = holds && c->delay_us > 0;
    for (int k = 0; k < TRIG_COUNT; k++) {
        uint32_t m = c->mask[k];
        if (!m)
            continue;
        if (c->single[k])
            hide |= m;
        else if (delay)
            hide |= hold_step(&holds[k], m, pos, now, c->delay_us);
        else if ((pos & m) == m)
            hide |= m;
    }
    if (!delay)
        return pos & ~hide;
    for (int k = 0; k < TRIG_COUNT; k++) {
        TrigHold *o = &holds[trig_other(k)];
        if (holds[k].combo && (c->mask[k] & c->mask[trig_other(k)]))
            o->claimed = 1; /* e.g. select+r completed: l+r's R is not replayed */
        if (holds[k].replay && now >= holds[k].replay_until_us)
            holds[k].replay = 0;
        replay |= holds[k].replay;
    }
    return (pos | replay) & ~hide;
}
