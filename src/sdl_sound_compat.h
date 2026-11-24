/* Small native32 adapter for the pcrecomp software DirectSound mixer. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <SDL3/SDL.h>
#include "native32.h"
#define A32(i) MEM32(g_esp + 4 + 4*(i))
#define RET(value, argc) do { g_eax = (uint32_t)(value); g_esp += 4 + 4*(argc); return; } while (0)
#define hle_va(p) ((uint32_t)(uintptr_t)(p))
#define hle_ptr(p) ((void*)(uintptr_t)(p))
#define hle_alloc(n) calloc(1, n)
#define hle_free free
#define hle_guest_strdup(s) hle_va(_strdup(s))
#define hle_call_guest native32_call_guest
typedef native32_shim_t win32hle_shim;
static uint32_t hle_com_new(uint32_t vt, size_t size) {
    uint32_t *p = (uint32_t*)calloc(1, size);
    if (!p) return 0;
    p[0] = vt; p[1] = 1; return hle_va(p);
}
static void hle_com_free(uint32_t self) { free(hle_ptr(self)); }
static void hle_com_AddRef(void) { uint32_t n = ++MEM32(A32(0)+4); RET(n, 1); }
static void hle_com_QueryInterface(void) {
    uint32_t self = A32(0);
    if (!A32(2)) RET(E_POINTER, 3);
    MEM32(A32(2)) = self; ++MEM32(self+4); RET(0, 3);
}
static uint32_t hle_com_vtable(const win32hle_shim *methods) {
    int n = 0; while (methods[n].name) ++n;
    uint32_t *v = (uint32_t*)calloc(n, sizeof(uint32_t));
    for (int i = 0; i < n; ++i) {
        /* Unique non-executable addresses resolved before the native bridge. */
        v[i] = hle_va(&v[i]); native32_replace(v[i], methods[i].fn);
    }
    return hle_va(v);
}
