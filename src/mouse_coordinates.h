#pragma once
#include "display_layout.h"
/* Reject borders/outside instead of clamping them onto a clickable menu edge. */
static int lt2_mouse_coordinates(int x, int y, int client_w, int client_h,
                                 int logical_w, int logical_h, int *out_x, int *out_y) {
    *out_x = *out_y = -1;
    if (client_w <= 0 || client_h <= 0 || logical_w <= 0 || logical_h <= 0 ||
        x < 0 || y < 0 || x >= client_w || y >= client_h) return 0;
    *out_x = (int)((long long)x*logical_w/client_w);
    *out_y = (int)((long long)y*logical_h/client_h);
    return 1;
}
