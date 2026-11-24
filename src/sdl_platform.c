/* SDL owns window sizing/fullscreen, event pumping and mouse policy. The
 * existing HWND is wrapped so the game's original Win32 procedure still gets
 * messages and Vulkan can keep using the same native surface. */
#define WIN32_LEAN_AND_MEAN
#define SDL_MAIN_HANDLED
#include <windows.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "native32.h"
#include "mouse_coordinates.h"
#include "mouse_buttons.h"
static float mouse_wheel;
static unsigned char pending_buttons[60];
static unsigned pending_button_count;
static uint32_t mouse_sequence;
static SDL_Window *window;
static HWND hwnd;
static int pumping;
static int quit_pending;
void lt2_fit_camera(unsigned int address) {
    if(!hwnd || !address || IsIconic(hwnd))return;
    RECT client;
    if(!GetClientRect(hwnd,&client) || client.right<=0 || client.bottom<=0)return;
    unsigned char *camera=(unsigned char*)(uintptr_t)address;
    if(camera[0x35c]&4)return; // game's orthographic camera flag
    int *viewport=(int*)(camera+0x54);
    int viewportWidth=viewport[2]-viewport[0],viewportHeight=viewport[3]-viewport[1];
    // Menu cameras live in authored subrectangles. Uniform UI scaling keeps
    // their aspect constant; only the full race camera follows the window.
    float aspect=(float)client.right/client.bottom;
    if((viewport[0]!=0 || viewport[1]!=0) && viewportWidth>0 && viewportHeight>0)
        aspect=(float)viewportWidth/viewportHeight;
    if(lt2_camera_fit((float*)(camera+0x2dc),aspect))
        camera[0x24]=1; // rebuild view/frustum even with a stationary camera
}
static HWND WINAPI create_window(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style,
    int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    if (parent || window)
        return CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, instance, param);
    HWND result = CreateWindowExA(ex, cls, title, style & ~WS_VISIBLE,
                                 x, y, w, h, parent, menu, instance, param);
    if (!result || parent || window) return result;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, result);
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window) { fprintf(stderr, "[SDL3 window] %s\n", SDL_GetError()); DestroyWindow(result); return NULL; }
    hwnd = result;
    SetPropW(hwnd, L"LT2.SDLWindow", (HANDLE)1);
    SetPropW(hwnd, L"LT2.Windowed", (HANDLE)1);
    char size[32]; int width = 1280, height = 720;
    int windowed = GetEnvironmentVariableA("LT2_WINDOW_SIZE", size, sizeof size) != 0;
    if (windowed) sscanf_s(size, "%dx%d", &width, &height);
    SDL_SetWindowBordered(window, windowed != 0);
    SDL_SetWindowResizable(window, true);
    SDL_SetWindowSize(window, width, height);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (!windowed) SDL_SetWindowFullscreen(window, true);
    SDL_SetWindowMouseGrab(window, false);
    SDL_SetWindowKeyboardGrab(window, false);
    SDL_SetWindowRelativeMouseMode(window, false);
    SDL_ShowCursor();
    ClipCursor(NULL);
    if (style & WS_VISIBLE) SDL_ShowWindow(window);
    fprintf(stderr, "[SDL3 window] %dx%d %s; mouse unconfined\n", width, height, windowed ? "windowed" : "desktop fullscreen");
    return result;
}
static BOOL WINAPI show_window(HWND w, int command) {
    if (w != hwnd || !window) return ShowWindow(w, command);
    BOOL was = IsWindowVisible(w);
    if (command == SW_HIDE) SDL_HideWindow(window);
    else if (command == SW_MINIMIZE || command == SW_SHOWMINIMIZED) SDL_MinimizeWindow(window);
    else { SDL_ShowWindow(window); if (command == SW_RESTORE) SDL_RestoreWindow(window); }
    fprintf(stderr, "[SDL3 window] ShowWindow(%d), visible=%d\n", command, IsWindowVisible(w));
    return was;
}
static BOOL WINAPI peek_message(MSG *msg, HWND w, UINT lo, UINT hi, UINT remove) {
    if (window && !pumping) {
        pumping = 1;
        SDL_PumpEvents();
        /* Native messages have reached the original game WndProc through SDL's
         * subclass. Drop duplicate SDL events; this game consumes Win32 input. */
        SDL_Event event;
        while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
            if (event.type == SDL_EVENT_QUIT) quit_pending = 1;
            if (event.type == SDL_EVENT_MOUSE_WHEEL)
                mouse_wheel += event.wheel.y * (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -120.0f : 120.0f);
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                int button = event.button.button == SDL_BUTTON_LEFT ? 0 :
                             event.button.button == SDL_BUTTON_RIGHT ? 1 :
                             event.button.button == SDL_BUTTON_MIDDLE ? 2 :
                             event.button.button == SDL_BUTTON_X1 ? 3 : -1;
                if (button >= 0 && pending_button_count < sizeof pending_buttons)
                    pending_buttons[pending_button_count++] = (unsigned char)(button |
                        (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 4 : 0));
            }
        }
        pumping = 0;
    }
    if (quit_pending && (!lo || lo <= WM_QUIT) && (!hi || hi >= WM_QUIT)) {
        memset(msg, 0, sizeof *msg); msg->message = WM_QUIT;
        if (remove & PM_REMOVE) quit_pending = 0;
        return TRUE;
    }
    return PeekMessageA(msg, w, lo, hi, remove);
}
/* LT2 input object layout verified from 004C16C0..004C187A. Menu hit testing
 * consumes a58/a5c; 540/544 are motion floats and 54c..54f are button bytes. */
void lt2_poll_mouse(unsigned int input) {
    unsigned char *state = (unsigned char*)(uintptr_t)input;
    unsigned char *view = *(unsigned char**)(state + 0xa6c);
    POINT point; RECT client;
    int x = -1, y = -1, active = 0;
    if (hwnd && view && GetForegroundWindow() == hwnd && GetCursorPos(&point) &&
        ScreenToClient(hwnd, &point) && GetClientRect(hwnd, &client))
        active = lt2_mouse_coordinates(point.x, point.y, client.right, client.bottom,
                                      800, 600, &x, &y);
    int old_x = *(int*)(state+0xa58), old_y = *(int*)(state+0xa5c);
    *(float*)(state+0x540) = active && old_x >= 0 ? (float)(x-old_x) : 0;
    *(float*)(state+0x544) = active && old_y >= 0 ? (float)(y-old_y) : 0;
    *(float*)(state+0x548) = active ? -mouse_wheel : 0;
    mouse_wheel = 0;
    *(int*)(state+0xa58) = x; *(int*)(state+0xa5c) = y;
    const int keys[4] = {VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1};
    unsigned held = 0;
    for (int i=0;i<4;++i) if (GetAsyncKeyState(keys[i]) & 0x8000) held |= 1u<<i;
    lt2_mouse_buttons(state, active, held, pending_buttons, pending_button_count,
                      GetTickCount(), &mouse_sequence);
    pending_button_count = 0;
}
static BOOL WINAPI clip_cursor(const RECT *unused) { (void)unused; return ClipCursor(NULL); }
static HWND WINAPI capture_mouse(HWND unused) { (void)unused; ReleaseCapture(); return NULL; }
static BOOL WINAPI move_cursor(int x, int y) { (void)x; (void)y; return TRUE; }
static int WINAPI show_cursor(BOOL show) { (void)show; SDL_ShowCursor(); return 0; }
int lt2_platform_init(void) {
    /* The game has a dedicated large-stack thread; designate it SDL's main
     * thread before initializing video. No SDL video calls on other threads. */
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr,"[SDL3] %s\n",SDL_GetError()); return 0; }
    SetEnvironmentVariableA("LT2_FREE_MOUSE", "1");
    native32_redirect("CreateWindowExA", create_window);
    native32_redirect("ShowWindow", show_window);
    native32_redirect("PeekMessageA", peek_message);
    native32_redirect("ClipCursor", clip_cursor);
    native32_redirect("SetCapture", capture_mouse);
    native32_redirect("SetCursorPos", move_cursor);
    native32_redirect("ShowCursor", show_cursor);
    return 1;
}
void lt2_platform_shutdown(void) {
    if (window) SDL_DestroyWindow(window);
    window = NULL; hwnd = NULL;
    ClipCursor(NULL);
    SDL_Quit();
}
