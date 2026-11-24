#pragma once
#include <stdint.h>
#include <string.h>
/* LT2's click readers (004C11D0/004C1220) scan 20-byte DirectInput-style
 * events at +554, count +a54, separately from held bytes at +54c. */
typedef struct { uint32_t offset, data, time, sequence, app; } lt2_mouse_event;
static void lt2_button_event(unsigned char *state, int button, int down,
                             uint32_t now, uint32_t *sequence) {
    if (state[0x54c+button] == down) return;
    uint32_t *count = (uint32_t*)(state+0xa54);
    if (*count >= 64) return;
    lt2_mouse_event *event = (lt2_mouse_event*)(state+0x554) + (*count)++;
    event->offset = 12+button; event->data = down ? 0x80 : 0;
    event->time = now; event->sequence = ++*sequence; event->app = 0;
    state[0x54c+button] = (unsigned char)down;
}
static void lt2_mouse_buttons(unsigned char *state, int active, unsigned held,
                              const unsigned char *pending, unsigned count,
                              uint32_t now, uint32_t *sequence) {
    *(uint32_t*)(state+0xa54) = 0;
    /* Reserve four slots for reconciliation after focus changes/lost events. */
    if (active) for (unsigned i=0; i<count && i<60; ++i)
        lt2_button_event(state, pending[i]&3, (pending[i]&4)!=0, now, sequence);
    for (int i=0; i<4; ++i)
        lt2_button_event(state, i, active && (held & (1u<<i)) ? 1 : 0, now, sequence);
}
