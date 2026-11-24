/* Shared by generated code and native32; preserve all eight values and shadows. */
double g_stbuf[X87_RING];
int64_t g_sti64buf[X87_RING];
double *g_stp = g_stbuf + X87_RING / 2;
int64_t *g_sti64p = g_sti64buf + X87_RING / 2;
void recomp_x87_recentre(void) {
    memmove(g_stbuf + X87_RING / 2, g_stp, 8 * sizeof(double));
    memmove(g_sti64buf + X87_RING / 2, g_sti64p, 8 * sizeof(int64_t));
    g_stp = g_stbuf + X87_RING / 2;
    g_sti64p = g_sti64buf + X87_RING / 2;
}
