#pragma once

void sub_004B3F10(void);

// Native rendering follows the client extent at frame boundaries. The old
// WM_SIZE handler changes the game's logical UI coordinates and recreates every
// mesh. Keep that coordinate space stable: both UI drawing and mouse hit testing
// map it to the current client size without a legacy device reset.
static void window_resize(void) {
    g_eax = 0;
    g_esp += 16; // original handler: ret 12
}

// The shipped WM_MOVE handler assumes its window-state argument is present.
// During initialization/device reset the event table temporarily supplies NULL.
static void window_move(void) {
    if (!MEM32(g_esp + 4)) {
        g_eax = 0;
        g_esp += 16; // handler uses ret 12
        return;
    }
    sub_004B3F10();
}

static BOOL WINAPI window_position(HWND window, HWND after, int x, int y,
                                  int width, int height, UINT flags) {
    if (GetPropW(window, L"LT2.Windowed")) {
        flags |= SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER;
    }
    return SetWindowPos(window, after, x, y, width, height, flags);
}
