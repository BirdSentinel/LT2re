#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native32.h"
#include "recomp_trace.h"
#include "window_bridge.h"
#include "renderer/game_render_api.h"
#ifdef LT2_NATIVE_SCENE
#include "renderer/game_scene_commands.h"
__declspec(dllimport) uint32_t LT2CreateSceneFactory(void);
__declspec(dllimport) int LT2InvokeScene(LT2SceneCommand, const uint32_t *, uint32_t *);
__declspec(dllimport) int LT2SubmitDraw(const LT2DrawPacket *, int32_t *);
static void create_scene_factory(void) {g_eax=LT2CreateSceneFactory();g_esp+=8;}
#endif
extern int lt2_platform_init(void);
extern void lt2_platform_shutdown(void);
extern void lt2_audio_shutdown(void);
extern const native32_shim_t lt2_sound_shims[];

static DWORD WINAPI diagnostics(void *unused) {
    (void)unused;
    for (;;) {
        Sleep(5000);
        fprintf(stderr, "[progress] function=%08X import=%s indirect_calls=%u\n",
                g_cur_func, g_cur_import, g_icall_count);
        fprintf(stderr, "[progress] recent:");
        uint32_t end = g_enter_idx;
        for (uint32_t i = end > 12 ? end - 12 : 0; i < end; ++i)
            fprintf(stderr, " %08X", g_enter_trace[i % RECOMP_ENTER_SIZE]);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}

recomp_func_t lt2_lookup_recovered(uint32_t va);
recomp_func_t recomp_lookup_manual(uint32_t va) { return lt2_lookup_recovered(va); }
__declspec(dllexport) unsigned lt2_graphics_caller(void) { return g_cur_func; }
unsigned lt2_graphics_site, lt2_graphics_serial;
__declspec(dllexport) unsigned lt2_graphics_callsite(unsigned *serial) {
    *serial = lt2_graphics_serial;
    return lt2_graphics_site;
}

#pragma optimize("gt", on)
int lt2_submit_game_draw(unsigned kind) {
#ifdef LT2_NATIVE_SCENE
    LT2SubmitDrawFn submit=LT2SubmitDraw;
#else
    static LT2SubmitDrawFn submit;
    static int disabled = -1;
    if (disabled < 0) disabled = getenv("LT2_LEGACY_DRAW_CALLS") != NULL;
    if (disabled) return 0;
    if (!submit) {
        HMODULE renderer = GetModuleHandleA("d3d8.dll");
        if (renderer) submit = (LT2SubmitDrawFn)GetProcAddress(renderer,"LT2SubmitDraw");
        if (!submit) return 0;
    }
#endif
    const uint32_t *a = (const uint32_t *)(uintptr_t)g_esp;
    LT2DrawPacket packet = {0};
    packet.version=LT2_RENDER_API_VERSION;
    packet.context=(void *)(uintptr_t)a[0]; packet.topology=a[1];
    unsigned arguments;
    uint32_t primitives;
    if(kind==0) { packet.firstVertex=a[2]; primitives=a[3]; arguments=4; }
    else if(kind==1) {
        packet.indexed=1; packet.firstVertex=a[2]; packet.vertexCount=a[3];
        packet.firstIndex=a[4]; primitives=a[5]; arguments=6;
    } else if(kind==2) {
        packet.userMemory=1; primitives=a[2];
        packet.vertices=(const void *)(uintptr_t)a[3]; packet.stride=a[4]; arguments=5;
    } else return 0;
    uint64_t count=primitives;
    switch(packet.topology) {
    case 1: break;
    case 2: count*=2; break;
    case 3: count+=1; break;
    case 4: count*=3; break;
    case 5: case 6: count+=2; break;
    default: return 0;
    }
    if(count>UINT32_MAX) return 0;
    if(packet.indexed)packet.indexCount=(uint32_t)count;
    else packet.vertexCount=(uint32_t)count;
    int32_t result;
    if(!submit(&packet,&result))return 0;
    g_eax=(uint32_t)result;
    g_esp+=arguments*4;
    return 1;
}
#ifdef LT2_NATIVE_SCENE
void lt2_game_scene(unsigned command, unsigned arguments, unsigned site) {
    uint32_t result;
    if(!LT2InvokeScene((LT2SceneCommand)command,(const uint32_t *)(uintptr_t)g_esp,&result)) {
        fprintf(stderr,"Native scene rejected call at %08X\n",site); ExitProcess(6);
    }
    if((int32_t)result<0)fprintf(stderr,"[scene result] site=%08X command=%u result=%08X\n",site,command,result);
    g_eax=result;g_esp+=arguments*4;
}
void lt2_native_draw(unsigned kind) {
    if(!lt2_submit_game_draw(kind)) {fprintf(stderr,"Native scene rejected draw\n");ExitProcess(6);}
}
#endif
#pragma optimize("", off)

static DWORD WINAPI run_game(void *unused) {
    (void)unused;
    freopen("lt2-recomp.log", "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
    native32_init();
    if (!lt2_platform_init()) return 5;
    {
        native32_replace(0x004B3F10, window_move);
#ifdef LT2_NATIVE_SCENE
        native32_replace(0x004B3E50, window_resize);
#endif
        native32_redirect("SetWindowPos", window_position);
    }
    if (getenv("LT2_DIAGNOSTICS")) {
        HANDLE logger = CreateThread(NULL, 0, diagnostics, NULL, 0, NULL);
        if (logger) CloseHandle(logger);
    }
    native32_set_command_line("LT2.exe");
    static native32_module_t modules[] = { {"LT2.exe", 0x00400000, 0} };
    char directory[MAX_PATH];
    GetCurrentDirectoryA(sizeof directory, directory);
    if (!native32_modules_map(directory, directory, modules, 1)) return 2;
    int sound_count = 0;
    while (lt2_sound_shims[sound_count].name) ++sound_count;
    int missing;
#ifdef LT2_NATIVE_SCENE
    native32_shim_t *shims=calloc(sound_count+1,sizeof *shims);
    memcpy(shims,lt2_sound_shims,sound_count*sizeof *shims);
    shims[sound_count].name="Direct3DCreate8";
    shims[sound_count].fn=create_scene_factory;
    missing=native32_modules_bind(shims,sound_count+1);
    free(shims);
#else
    missing = native32_modules_bind(lt2_sound_shims, sound_count);
#endif
    if (missing) { fprintf(stderr, "Unresolved imports: %d\n", missing); return 3; }
    fprintf(stderr, "LT2: entering lifted entry point 0x004FD13F\n");
    native32_call_guest(0x004FD13F, 0, NULL);
    lt2_audio_shutdown();
    lt2_platform_shutdown();
    return 0;
}

static int dimension(const char *text) {
    if (!*text) return 0;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return 0;
    char *end;
    long value = strtol(text, &end, 10);
    return !*end && value >= 64 && value <= 8192 ? (int)value : 0;
}

int main(int argc, char **argv) {
    int width = 0, height = 0;
    if (argc == 5 && !strcmp(argv[2], "-window")) {
        width = dimension(argv[3]);
        height = dimension(argv[4]);
    }
    if ((argc != 2 && !(argc == 5 && width && height))) {
        fprintf(stderr, "Usage: LT2re.exe <game-data-directory> [-window width height]\n"
                        "Window dimensions must be integers from 64 to 8192.\n");
        return 1;
    }
    if (!SetCurrentDirectoryA(argv[1])) {
        fprintf(stderr, "Cannot open game-data directory: %s\n", argv[1]);
        return 1;
    }
    if (width) {
        char size[32];
        snprintf(size, sizeof size, "%dx%d", width, height);
        SetEnvironmentVariableA("LT2_WINDOW_SIZE", size);
        SetProcessDPIAware();
    } else SetEnvironmentVariableA("LT2_WINDOW_SIZE", NULL);
    native32_hold_image("LT2.exe", 0x00400000);
    HANDLE thread = CreateThread(NULL, 16 * 1024 * 1024, run_game, NULL,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!thread) return 4;
    WaitForSingleObject(thread, INFINITE);
    DWORD result;
    GetExitCodeThread(thread, &result);
    CloseHandle(thread);
    return (int)result;
}
