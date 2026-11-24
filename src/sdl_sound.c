/*
 * Adapted from pcrecomp runtime/win32hle/dsound.c (MIT, see licenses/pcrecomp.txt).
 * SDL3 audio: IDirectSound and IDirectSoundBuffer,
 * with a software mixer.
 *
 * A secondary buffer is guest memory the game Locks and writes, as it would
 * be on Windows; the mixer (SDL's audio thread) reads it at the buffer's
 * frequency, volume and pan and moves its play cursor in real time, so a
 * game that streams into a looping buffer by watching GetCurrentPosition
 * keeps doing so. The primary buffer is only its format: everything is mixed
 * at the device's rate. One mutex covers the buffers' state, between the
 * mixer and the shims.
 *
 * With no audio device (SDL_AUDIODRIVER=dummy, a headless box) SDL's dummy
 * driver still runs the callback in real time, so the cursors still move.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sdl_sound_compat.h"

#define DS_OK               0u
#define DSERR_INVALIDPARAM  0x80070057u
#define DSERR_NODRIVER      0x88780078u
#define DSERR_INVALIDCALL   0x88780032u
#define DSERR_PRIOLEVELNEEDED 0x88780046u
#define DSERR_BUFFERLOST    0x88780096u
#define DSBCAPS_PRIMARYBUFFER 0x00000001u
#define DSBPLAY_LOOPING     0x00000001u
#define DSBSTATUS_PLAYING   0x00000001u
#define DSBSTATUS_LOOPING   0x00000004u

#define OUT_RATE 44100

typedef struct {
    uint8_t *pcm;               /* committed samples, protected by g_mix */
    uint8_t *data;              /* the samples, guest-visible */
    int *data_refs;             /* shared with duplicates */
    uint32_t bytes, flags;
    int channels, bits, rate, block;
    int playing, looping, primary;
    double pos;                 /* where the mixer has read to, in frames */
    double pos0;                /* ...and where its last mix began */
    int mixed;                  /* pos0 is from the last mix (not set since) */
    int volume, pan;            /* hundredths of a dB; -10000..10000 */
    float gl, gr;
    /* DirectSound 3D (a DSBCAPS_CTRL3D buffer): where it is, as a listener hears it */
    int is3d, mode3d;           /* DS3DMODE_NORMAL 0, HEADRELATIVE 1, DISABLE 2 */
    float p3d[3], mind, maxd;
} dsbuf;
/* the one listener (IDirectSound3DListener on the primary buffer) */
static struct { float pos[3], front[3], top[3], rolloff, distf; } g_lis = { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 1, 1 };

static SRWLOCK g_mix = SRWLOCK_INIT;
#define MAX_BUFS 256
static dsbuf *g_buf[MAX_BUFS];
static SDL_AudioStream *g_dev;
static int g_audio_ok = -1;
static float g_master = 1.0f;
static uint64_t g_mix_at;       /* when the last mix ran (SDL's counter) */
static int g_mix_frames;        /* ...and how many output frames it made */
void hle_dsound_set_master(float gain) { g_master = gain; }
static dsbuf *buf_of(uint32_t self);
int hle_dsound_buffer_gains(uint32_t buffer, float *l, float *r) {
    AcquireSRWLockExclusive(&g_mix);
    dsbuf *b = buf_of(buffer);
    if (b) *l = b->gl, *r = b->gr;
    ReleaseSRWLockExclusive(&g_mix);
    return b != NULL;
}

static void gains(dsbuf *b) {
    float v = b->volume <= -10000 ? 0.0f : powf(10.0f, (float)b->volume / 2000.0f);
    float l = 1.0f, r = 1.0f;
    if (b->is3d && b->mode3d != 2) {
        /* inverse distance past the minimum, held at the maximum; panned
         * by the side the sound is on (DirectSound is left-handed: right =
         * top x front) */
        float rel[3];
        for (int k = 0; k < 3; k++) rel[k] = b->mode3d == 1 ? b->p3d[k] : b->p3d[k] - g_lis.pos[k];
        float d = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
        float mind = b->mind > 0 ? b->mind : 1.0f, maxd = b->maxd > mind ? b->maxd : 1e9f;
        float dd = d < mind ? mind : d > maxd ? maxd : d;
        float att = mind / (mind + g_lis.rolloff * (dd - mind));
        const float *f = g_lis.front, *t = g_lis.top;
        float right[3] = { t[1] * f[2] - t[2] * f[1], t[2] * f[0] - t[0] * f[2], t[0] * f[1] - t[1] * f[0] };
        float rn = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
        float side = d > 1e-4f && rn > 1e-6f ? (rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2]) / (d * rn) : 0.0f;
        v *= att;
        l = side > 0 ? 1.0f - 0.7f * side : 1.0f, r = side < 0 ? 1.0f + 0.7f * side : 1.0f;
    }
    /* a 3D buffer has no pan control on Windows (DSBCAPS_CTRL3D and
     * DSBCAPS_CTRLPAN cannot be combined): its side comes from the 3D mix */
    else {
        if (b->pan < 0) r = b->pan <= -10000 ? 0.0f : powf(10.0f, (float)b->pan / 2000.0f);
        if (b->pan > 0) l = b->pan >= 10000 ? 0.0f : powf(10.0f, (float)-b->pan / 2000.0f);
    }
    b->gl = v * l, b->gr = v * r;
}

static inline float sample(const dsbuf *b, uint32_t frame, int ch) {
    const uint8_t *p = b->pcm + (size_t)frame * (uint32_t)b->block + (b->channels > 1 ? (size_t)ch * (uint32_t)(b->bits / 8) : 0);
    return b->bits == 16 ? (float)*(const int16_t *)p / 32768.0f : ((float)*p - 128.0f) / 128.0f;
}

static int audio_open(void);
static int audio_open_once(void) { return audio_open(); }

/* Host streams: PCM a host pushes (a movie's sound), mixed with the
 * buffers. A ring of interleaved s16 at the stream's rate, played at that
 * rate. */
#define MAX_STREAMS 4
#define STREAM_FRAMES (1 << 17)
static struct { int used, channels, rate; float gain; int16_t *ring; uint32_t rd, wr; double pos; } g_stream[MAX_STREAMS];
int hle_audio_stream_open(int rate, int channels) {
    if (!audio_open_once()) return -1;
    AcquireSRWLockExclusive(&g_mix);
    int id = -1;
    for (int i = 0; i < MAX_STREAMS && id < 0; i++)
        if (!g_stream[i].used) {
            id = i;
            g_stream[i].ring = (int16_t *)calloc(STREAM_FRAMES * 2, 2);
            g_stream[i].used = 1, g_stream[i].channels = channels, g_stream[i].rate = rate, g_stream[i].gain = 1.0f;
            g_stream[i].rd = g_stream[i].wr = 0, g_stream[i].pos = 0;
        }
    ReleaseSRWLockExclusive(&g_mix);
    return id;
}
void hle_audio_stream_push(int id, const int16_t *pcm, int frames) {
    if (id < 0 || id >= MAX_STREAMS) return;
    AcquireSRWLockExclusive(&g_mix);
    for (int f = 0; f < frames && g_stream[id].wr - g_stream[id].rd < STREAM_FRAMES - 1; f++, g_stream[id].wr++) {
        uint32_t k = g_stream[id].wr % STREAM_FRAMES;
        g_stream[id].ring[2 * k] = pcm[f * g_stream[id].channels];
        g_stream[id].ring[2 * k + 1] = pcm[f * g_stream[id].channels + (g_stream[id].channels > 1)];
    }
    ReleaseSRWLockExclusive(&g_mix);
}
int hle_audio_stream_queued(int id) {                    /* frames not yet played */
    if (id < 0 || id >= MAX_STREAMS) return 0;
    AcquireSRWLockExclusive(&g_mix);
    int n = (int)(g_stream[id].wr - g_stream[id].rd);
    ReleaseSRWLockExclusive(&g_mix);
    return n;
}
void hle_audio_stream_gain(int id, float gain) { if (id >= 0 && id < MAX_STREAMS) g_stream[id].gain = gain; }
void hle_audio_stream_close(int id) {
    if (id < 0 || id >= MAX_STREAMS) return;
    AcquireSRWLockExclusive(&g_mix);
    free(g_stream[id].ring);
    memset(&g_stream[id], 0, sizeof g_stream[id]);
    ReleaseSRWLockExclusive(&g_mix);
}

static void mix(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int frames = len / 4;
    static float acc[2 * 8192];
    if (frames > 8192) frames = 8192;
    memset(acc, 0, sizeof(float) * 2 * (size_t)frames);
    AcquireSRWLockExclusive(&g_mix);
    for (int i = 0; i < MAX_BUFS; i++) {
        dsbuf *b = g_buf[i];
        if (!b || !b->playing || b->primary || !b->block) continue;
        uint32_t nframes = b->bytes / (uint32_t)b->block;
        double step = (double)b->rate / OUT_RATE;
        b->pos0 = b->pos, b->mixed = 1;
        for (int f = 0; f < frames && b->playing; f++) {
            uint32_t at = (uint32_t)b->pos;
            if (at >= nframes) {
                if (b->looping) { b->pos -= nframes; at = (uint32_t)b->pos; if (at >= nframes) b->pos = 0, at = 0; }
                else { b->playing = 0; b->pos = 0; break; }
            }
            uint32_t next = at + 1 < nframes ? at + 1 : b->looping ? 0 : at;
            float frac = (float)(b->pos - at);
            float l = sample(b, at, 0) * (1-frac) + sample(b, next, 0) * frac;
            float r = b->channels > 1 ? sample(b, at, 1) * (1-frac) + sample(b, next, 1) * frac : l;
            acc[2 * f] += l * b->gl;
            acc[2 * f + 1] += r * b->gr;
            b->pos += step;
        }
    }
    g_mix_at = SDL_GetPerformanceCounter(), g_mix_frames = frames;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!g_stream[i].used) continue;
        double step = (double)g_stream[i].rate / OUT_RATE;
        for (int f = 0; f < frames && g_stream[i].rd < g_stream[i].wr; f++) {
            uint32_t k = g_stream[i].rd % STREAM_FRAMES;
            acc[2 * f] += g_stream[i].ring[2 * k] / 32768.0f * g_stream[i].gain;
            acc[2 * f + 1] += g_stream[i].ring[2 * k + 1] / 32768.0f * g_stream[i].gain;
            g_stream[i].pos += step;
            while (g_stream[i].pos >= 1.0 && g_stream[i].rd < g_stream[i].wr) g_stream[i].pos -= 1.0, g_stream[i].rd++;
        }
    }
    ReleaseSRWLockExclusive(&g_mix);
    int16_t *out = (int16_t *)stream;
    for (int k = 0; k < 2 * frames; k++) {
        float v = acc[k] * g_master * 32767.0f;
        out[k] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
}

/* SDL owns the audio thread: never call the guest or take its machine lock here. */
static void SDLCALL audio_feed(void *user, SDL_AudioStream *stream, int additional, int total) {
    (void)user; (void)total;
    int16_t pcm[2048];
    while (additional > 0) {
        int bytes = additional > sizeof pcm ? sizeof pcm : additional;
        bytes = (bytes + 3) & ~3;
        mix(NULL, (Uint8*)pcm, bytes);
        if (!SDL_PutAudioStreamData(stream, pcm, bytes)) break;
        additional -= bytes;
    }
}
static int audio_open(void) {
    if (g_audio_ok >= 0) return g_audio_ok;
    g_audio_ok = 0;
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, OUT_RATE };
    if (SDL_InitSubSystem(SDL_INIT_AUDIO))
        g_dev = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_feed, NULL);
    if (!g_dev || !SDL_ResumeAudioStreamDevice(g_dev)) {
        fprintf(stderr, "[SDL3 audio] open failed: %s\n", SDL_GetError());
        return 0;
    }
    fprintf(stderr, "[SDL3 audio] %s, %d Hz stereo, independent mixer\n", SDL_GetCurrentAudioDriver(), OUT_RATE);
    return g_audio_ok = 1;
}
void lt2_audio_shutdown(void) {
    if (g_dev) SDL_DestroyAudioStream(g_dev);
    g_dev = NULL;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

/* The play cursor, in bytes. The mixer reads a callback's worth at a time,
 * but a sound card's cursor moves smoothly: between mixes it is moved on by
 * the time since the last one, up to where that mix read to. A game that
 * streams by the cursor counts on that: one that refills the buffer when the
 * cursor reaches its own write position would otherwise find them equal,
 * take the buffer for empty and write over a lap not yet played. Under
 * g_mix. */
static uint32_t cursor(const dsbuf *b) {
    if (!b->block || !b->bytes) return 0;
    double pos = b->pos;
    if (b->playing && b->mixed && g_mix_frames) {
        double out = (double)(SDL_GetPerformanceCounter() - g_mix_at) * OUT_RATE / (double)SDL_GetPerformanceFrequency();
        if (out > g_mix_frames) out = g_mix_frames;
        double nframes = (double)(b->bytes / (uint32_t)b->block);
        double end = b->pos < b->pos0 ? b->pos + nframes : b->pos;   /* the mix wrapped */
        pos = b->pos0 + out * b->rate / OUT_RATE;
        if (pos > end) pos = end;
        if (pos >= nframes) pos -= nframes;
    }
    return ((uint32_t)pos * (uint32_t)b->block) % b->bytes;
}

/* the object: vtable, refs, then the buffer slot */
#define SLOT(self) MEM32((self) + 8)
static dsbuf *buf_of(uint32_t self) { uint32_t i = SLOT(self); return i < MAX_BUFS ? g_buf[i] : NULL; }

static uint32_t g_vt_ds, g_vt_buf;
static uint32_t new_buffer(dsbuf *b) {
    AcquireSRWLockExclusive(&g_mix);
    int slot = -1;
    for (int i = 0; i < MAX_BUFS && slot < 0; i++) if (!g_buf[i]) slot = i;
    if (slot >= 0) g_buf[slot] = b;
    ReleaseSRWLockExclusive(&g_mix);
    if (slot < 0) return 0;
    uint32_t o = hle_com_new(g_vt_buf, 12);
    SLOT(o) = (uint32_t)slot;
    return o;
}

/* ---- IDirectSoundBuffer ---- */
static uint32_t buf_release(uint32_t self) {
    uint32_t n = --MEM32(self + 4);
    if (!n) {
        AcquireSRWLockExclusive(&g_mix);
        dsbuf *b = buf_of(self);
        g_buf[SLOT(self)] = NULL;
        ReleaseSRWLockExclusive(&g_mix);
        if (b) {
            if (--*b->data_refs == 0) { hle_free(b->data); free(b->pcm); free(b->data_refs); }
            free(b);
        }
        hle_com_free(self);
    }
    return n;
}
static void b_Release(void) { RET(buf_release(A32(0)), 1); }
static void b_GetCaps(void) {                            /* (this, &DSBCAPS) */
    dsbuf *b = buf_of(A32(0));
    uint32_t c = A32(1);
    if (!b || !c) RET(DSERR_INVALIDPARAM, 2);
    MEM32(c + 4) = b->flags | 0x8u /* LOCSOFTWARE */, MEM32(c + 8) = b->bytes, MEM32(c + 12) = 0, MEM32(c + 16) = 0;
    RET(DS_OK, 2);
}
static void b_GetCurrentPosition(void) {                 /* (this, &play, &write) */
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 3);
    AcquireSRWLockExclusive(&g_mix);
    uint32_t play = cursor(b);
    uint32_t lead = b->block ? (uint32_t)(b->rate / 20) * (uint32_t)b->block : 0;   /* 50 ms ahead of the playback cursor */
    ReleaseSRWLockExclusive(&g_mix);
    if (b->bytes) play %= b->bytes;
    if (A32(1)) MEM32(A32(1)) = play;
    if (A32(2)) MEM32(A32(2)) = b->bytes ? (play + (b->playing ? lead : 0)) % b->bytes : 0;
    RET(DS_OK, 3);
}
static void b_GetFormat(void) {                          /* (this, &wfx, size, &written) */
    dsbuf *b = buf_of(A32(0));
    uint32_t w = A32(1);
    if (!b) RET(DSERR_INVALIDPARAM, 4);
    if (w && A32(2) >= 16) {
        MEM16(w) = 1, MEM16(w + 2) = (uint16_t)b->channels, MEM32(w + 4) = (uint32_t)b->rate;
        MEM32(w + 8) = (uint32_t)(b->rate * b->block), MEM16(w + 12) = (uint16_t)b->block, MEM16(w + 14) = (uint16_t)b->bits;
        if (A32(2) >= 18) MEM16(w + 16) = 0;
    }
    if (A32(3)) MEM32(A32(3)) = 18;
    RET(DS_OK, 4);
}
static void b_GetVolume(void)    { dsbuf *b = buf_of(A32(0)); if (A32(1) && b) MEM32(A32(1)) = (uint32_t)b->volume; RET(b ? DS_OK : DSERR_INVALIDPARAM, 2); }
static void b_GetPan(void)       { dsbuf *b = buf_of(A32(0)); if (A32(1) && b) MEM32(A32(1)) = (uint32_t)b->pan; RET(b ? DS_OK : DSERR_INVALIDPARAM, 2); }
static void b_GetFrequency(void) { dsbuf *b = buf_of(A32(0)); if (A32(1) && b) MEM32(A32(1)) = (uint32_t)b->rate; RET(b ? DS_OK : DSERR_INVALIDPARAM, 2); }
static void b_GetStatus(void) {
    dsbuf *b = buf_of(A32(0));
    if (A32(1) && b) MEM32(A32(1)) = (b->playing ? DSBSTATUS_PLAYING : 0u) | (b->playing && b->looping ? DSBSTATUS_LOOPING : 0u);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 2);
}
static void b_Initialize(void) { RET(DSERR_INVALIDCALL, 3); }   /* already initialized */
static void b_Lock(void) {                               /* (this, offset, bytes, &p1, &n1, &p2, &n2, flags) */
    dsbuf *b = buf_of(A32(0));
    if (!b || b->primary) RET(b ? DSERR_PRIOLEVELNEEDED : DSERR_INVALIDPARAM, 8);
    uint32_t off = A32(1), n = A32(2), fl = A32(7);
    if (fl & 2u) off = 0, n = b->bytes;                  /* DSBLOCK_ENTIREBUFFER */
    else if (fl & 1u) {                                  /* DSBLOCK_FROMWRITECURSOR */
        AcquireSRWLockExclusive(&g_mix);
        off = b->block ? (cursor(b) + (uint32_t)(b->rate / 20) * (uint32_t)b->block) % b->bytes : 0;
        ReleaseSRWLockExclusive(&g_mix);
    }
    if (off >= b->bytes || n > b->bytes) RET(DSERR_INVALIDPARAM, 8);
    uint32_t n1 = off + n <= b->bytes ? n : b->bytes - off;
    if (A32(3)) MEM32(A32(3)) = hle_va(b->data + off);
    if (A32(4)) MEM32(A32(4)) = n1;
    if (A32(5)) MEM32(A32(5)) = n1 < n ? hle_va(b->data) : 0;
    if (A32(6)) MEM32(A32(6)) = n - n1;
    RET(DS_OK, 8);
}
static void b_Unlock(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 5);
    uint32_t p1 = A32(1), n1 = A32(2), p2 = A32(3), n2 = A32(4);
    uintptr_t base = (uintptr_t)b->data;
    if ((n1 && (p1 < base || p1 - base > b->bytes || n1 > b->bytes - (p1 - base))) ||
        (n2 && (p2 < base || p2 - base > b->bytes || n2 > b->bytes - (p2 - base)))) RET(DSERR_INVALIDPARAM, 5);
    AcquireSRWLockExclusive(&g_mix);
    if (n1) memcpy(b->pcm + (p1 - base), (void*)p1, n1);
    if (n2) memcpy(b->pcm + (p2 - base), (void*)p2, n2);
    ReleaseSRWLockExclusive(&g_mix);
    RET(DS_OK, 5);
}            /* (this, p1, n1, p2, n2): the memory is the buffer */
static void b_Play(void) {                               /* (this, reserved, priority, flags) */
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 4);
    AcquireSRWLockExclusive(&g_mix);
    if (!b->playing) b->mixed = 0;
    b->playing = 1, b->looping = (A32(3) & DSBPLAY_LOOPING) != 0;
    ReleaseSRWLockExclusive(&g_mix);
    RET(DS_OK, 4);
}
static void b_SetCurrentPosition(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b || !b->block) RET(DSERR_INVALIDPARAM, 2);
    AcquireSRWLockExclusive(&g_mix);
    b->pos = (double)((A32(1) % (b->bytes ? b->bytes : 1)) / (uint32_t)b->block), b->mixed = 0;
    ReleaseSRWLockExclusive(&g_mix);
    RET(DS_OK, 2);
}
static void b_SetFormat(void) {                          /* the primary's: the mix is at the device rate anyway */
    dsbuf *b = buf_of(A32(0));
    uint32_t w = A32(1);
    if (b && w) b->channels = MEM16(w + 2), b->rate = (int)MEM32(w + 4), b->bits = MEM16(w + 14), b->block = MEM16(w + 12);
    RET(DS_OK, 2);
}
static void b_SetVolume(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 2);
    AcquireSRWLockExclusive(&g_mix);
    b->volume = (int)A32(1);
    gains(b);
    if (b->primary) g_master = b->gl;
    ReleaseSRWLockExclusive(&g_mix);
    RET(DS_OK, 2);
}
static void b_SetPan(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 2);
    AcquireSRWLockExclusive(&g_mix);
    b->pan = (int)A32(1);
    gains(b);
    ReleaseSRWLockExclusive(&g_mix);
    RET(DS_OK, 2);
}
static void b_SetFrequency(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 2);
    AcquireSRWLockExclusive(&g_mix);
    if (A32(1)) b->rate = (int)A32(1);
    ReleaseSRWLockExclusive(&g_mix);
    RET(DS_OK, 2);
}
static void b_Stop(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 1);
    AcquireSRWLockExclusive(&g_mix);
    b->playing = 0;
    ReleaseSRWLockExclusive(&g_mix);
    RET(DS_OK, 1);
}
static void b_Restore(void) { RET(DS_OK, 1); }

/* ---- DirectSound 8 and 3D ---- */
static uint32_t g_vt_3db, g_vt_lis, g_lis_obj;
static float fa(int i) { uint32_t v = A32(i); float f; memcpy(&f, &v, 4); return f; }
static void all3d_gains(void) { for (int i = 0; i < MAX_BUFS; i++) if (g_buf[i] && g_buf[i]->is3d) gains(g_buf[i]); }
static void b_QueryInterface(void) {                     /* (this, iid, &out) */
    uint32_t self = A32(0), d1 = A32(1) ? MEM32(A32(1)) : 0, out = A32(2);
    dsbuf *b = buf_of(self);
    if (d1 == 0x279AFA86u && b) {                        /* IID_IDirectSound3DBuffer */
        /* vtable, refs, the buffer's slot, the buffer: the interface holds
         * a reference on its buffer, so the slot stays the buffer's for as
         * long as the interface lives */
        uint32_t o = hle_com_new(g_vt_3db, 16);
        SLOT(o) = SLOT(self), MEM32(o + 12) = self;
        MEM32(self + 4)++;
        AcquireSRWLockExclusive(&g_mix);
        if (!b->is3d) b->is3d = 1, b->mind = 1.0f, b->maxd = 1e9f, gains(b);
        ReleaseSRWLockExclusive(&g_mix);
        if (out) MEM32(out) = o;
        RET(DS_OK, 3);
    }
    if (d1 == 0x279AFA84u) {                             /* IID_IDirectSound3DListener */
        /* one listener, made once and kept: hle_com_new's reference is
         * ours, and l_Release never gives it up */
        if (!g_lis_obj) g_lis_obj = hle_com_new(g_vt_lis, 12);
        MEM32(g_lis_obj + 4)++;
        if (out) MEM32(out) = g_lis_obj;
        RET(DS_OK, 3);
    }
    if (d1 != 0 && d1 != 0x279AFA85u && d1 != 0x6825A449u) {
        if (out) MEM32(out) = 0;
        fprintf(stderr, "[SDL3 audio] unsupported buffer interface %08X\n", d1);
        RET(0x80004002u, 3);
    }
    if (out) MEM32(out) = self;
    MEM32(self + 4)++;
    RET(DS_OK, 3);
}
static void b8_SetFX(void) { RET(0x8878001Eu, 4); }      /* DSERR_CONTROLUNAVAIL */
static void b8_AcquireResources(void) { RET(DS_OK, 4); }
static void b8_GetObjectInPath(void) { if (A32(4)) MEM32(A32(4)) = 0; RET(0x88780FFFu, 5); }
static void d8_VerifyCertification(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(DS_OK, 2); }

static void t_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) {
        uint32_t buf = MEM32(self + 12);
        hle_com_free(self);
        buf_release(buf);
    }
    RET(n, 1);
}
static void l_Release(void) {
    uint32_t self = A32(0);
    if (MEM32(self + 4) > 1) MEM32(self + 4)--;          /* never below our own reference */
    RET(MEM32(self + 4) - 1, 1);
}
static void t_SetPosition(void) {                        /* (this, x, y, z, apply) */
    dsbuf *b = buf_of(A32(0));
    if (b) { AcquireSRWLockExclusive(&g_mix); b->p3d[0] = fa(1), b->p3d[1] = fa(2), b->p3d[2] = fa(3); gains(b); ReleaseSRWLockExclusive(&g_mix); }
    RET(DS_OK, 5);
}
static void t_GetPosition(void) {
    dsbuf *b = buf_of(A32(0));
    if (b && A32(1)) memcpy(hle_ptr(A32(1)), b->p3d, 12);
    RET(DS_OK, 2);
}
static void t_SetMinDistance(void) { dsbuf *b = buf_of(A32(0)); if (b) { AcquireSRWLockExclusive(&g_mix); b->mind = fa(1); gains(b); ReleaseSRWLockExclusive(&g_mix); } RET(DS_OK, 3); }
static void t_SetMaxDistance(void) { dsbuf *b = buf_of(A32(0)); if (b) { AcquireSRWLockExclusive(&g_mix); b->maxd = fa(1); gains(b); ReleaseSRWLockExclusive(&g_mix); } RET(DS_OK, 3); }
static void t_GetMinDistance(void) { dsbuf *b = buf_of(A32(0)); if (b && A32(1)) memcpy(hle_ptr(A32(1)), &b->mind, 4); RET(DS_OK, 2); }
static void t_GetMaxDistance(void) { dsbuf *b = buf_of(A32(0)); if (b && A32(1)) memcpy(hle_ptr(A32(1)), &b->maxd, 4); RET(DS_OK, 2); }
static void t_SetMode(void) { dsbuf *b = buf_of(A32(0)); if (b) { AcquireSRWLockExclusive(&g_mix); b->mode3d = (int)A32(1); gains(b); ReleaseSRWLockExclusive(&g_mix); } RET(DS_OK, 3); }
static void t_GetMode(void) { dsbuf *b = buf_of(A32(0)); if (b && A32(1)) MEM32(A32(1)) = (uint32_t)b->mode3d; RET(DS_OK, 2); }
static void t_SetAllParameters(void) {                   /* (this, DS3DBUFFER*, apply) */
    dsbuf *b = buf_of(A32(0));
    uint32_t p = A32(1);
    if (b && p) {
        AcquireSRWLockExclusive(&g_mix);
        memcpy(b->p3d, hle_ptr(p + 4), 12);
        memcpy(&b->mind, hle_ptr(p + 52), 4), memcpy(&b->maxd, hle_ptr(p + 56), 4);
        b->mode3d = (int)MEM32(p + 60);
        gains(b);
        ReleaseSRWLockExclusive(&g_mix);
    }
    RET(DS_OK, 3);
}
static void t_GetAllParameters(void) {
    dsbuf *b = buf_of(A32(0));
    uint32_t p = A32(1);
    if (b && p) {
        memset(hle_ptr(p + 4), 0, 60);
        memcpy(hle_ptr(p + 4), b->p3d, 12);
        MEM32(p + 28) = MEM32(p + 32) = 360;
        MEM32(p + 44) = 0; memcpy(hle_ptr(p + 52), &b->mind, 4), memcpy(hle_ptr(p + 56), &b->maxd, 4);
        MEM32(p + 60) = (uint32_t)b->mode3d;
    }
    RET(DS_OK, 2);
}
static void t_ok2(void) { RET(DS_OK, 3); }
static void t_ok3(void) { RET(DS_OK, 4); }
static void t_ok4(void) { RET(DS_OK, 5); }
static void t_zero1(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(DS_OK, 2); }
static void t_zero2(void) { if (A32(1)) MEM32(A32(1)) = 0; if (A32(2)) MEM32(A32(2)) = 0; RET(DS_OK, 3); }
static void l_SetPosition(void) { AcquireSRWLockExclusive(&g_mix); g_lis.pos[0] = fa(1), g_lis.pos[1] = fa(2), g_lis.pos[2] = fa(3); all3d_gains(); ReleaseSRWLockExclusive(&g_mix); RET(DS_OK, 5); }
static void l_GetPosition(void) { if (A32(1)) memcpy(hle_ptr(A32(1)), g_lis.pos, 12); RET(DS_OK, 2); }
static void l_SetOrientation(void) {                     /* (this, fx, fy, fz, tx, ty, tz, apply) */
    AcquireSRWLockExclusive(&g_mix);
    for (int k = 0; k < 3; k++) g_lis.front[k] = fa(1 + k), g_lis.top[k] = fa(4 + k);
    all3d_gains();
    ReleaseSRWLockExclusive(&g_mix);
    RET(DS_OK, 8);
}
static void l_GetOrientation(void) {
    if (A32(1)) memcpy(hle_ptr(A32(1)), g_lis.front, 12);
    if (A32(2)) memcpy(hle_ptr(A32(2)), g_lis.top, 12);
    RET(DS_OK, 3);
}
static void l_SetRolloffFactor(void) { AcquireSRWLockExclusive(&g_mix); g_lis.rolloff = fa(1); all3d_gains(); ReleaseSRWLockExclusive(&g_mix); RET(DS_OK, 3); }
static void l_GetRolloffFactor(void) { if (A32(1)) memcpy(hle_ptr(A32(1)), &g_lis.rolloff, 4); RET(DS_OK, 2); }
static void l_SetDistanceFactor(void) { g_lis.distf = fa(1); RET(DS_OK, 3); }
static void l_GetDistanceFactor(void) { if (A32(1)) memcpy(hle_ptr(A32(1)), &g_lis.distf, 4); RET(DS_OK, 2); }
static void l_SetAllParameters(void) {                   /* (this, DS3DLISTENER*, apply) */
    uint32_t p = A32(1);
    if (p) {
        AcquireSRWLockExclusive(&g_mix);
        memcpy(g_lis.pos, hle_ptr(p + 4), 12);
        memcpy(g_lis.front, hle_ptr(p + 28), 12), memcpy(g_lis.top, hle_ptr(p + 40), 12);
        memcpy(&g_lis.distf, hle_ptr(p + 52), 4), memcpy(&g_lis.rolloff, hle_ptr(p + 56), 4);
        all3d_gains();
        ReleaseSRWLockExclusive(&g_mix);
    }
    RET(DS_OK, 3);
}
static void l_GetAllParameters(void) {
    uint32_t p = A32(1);
    if (p) {
        for (uint32_t k = 4; k < 64; k += 4) MEM32(p + k) = 0;
        memcpy(hle_ptr(p + 4), g_lis.pos, 12);
        memcpy(hle_ptr(p + 28), g_lis.front, 12), memcpy(hle_ptr(p + 40), g_lis.top, 12);
        memcpy(hle_ptr(p + 52), &g_lis.distf, 4), memcpy(hle_ptr(p + 56), &g_lis.rolloff, 4);
        float one = 1.0f; memcpy(hle_ptr(p + 60), &one, 4);
    }
    RET(DS_OK, 2);
}
static void l_Commit(void) { RET(DS_OK, 1); }
static void vtables3d(void) {
    if (g_vt_3db) return;
    static const win32hle_shim t[] = {
        { "IDirectSound3DBuffer::QueryInterface", hle_com_QueryInterface }, { "IDirectSound3DBuffer::AddRef", hle_com_AddRef },
        { "IDirectSound3DBuffer::Release", t_Release }, { "IDirectSound3DBuffer::GetAllParameters", t_GetAllParameters },
        { "IDirectSound3DBuffer::GetConeAngles", t_zero2 }, { "IDirectSound3DBuffer::GetConeOrientation", t_zero1 },
        { "IDirectSound3DBuffer::GetConeOutsideVolume", t_zero1 }, { "IDirectSound3DBuffer::GetMaxDistance", t_GetMaxDistance },
        { "IDirectSound3DBuffer::GetMinDistance", t_GetMinDistance }, { "IDirectSound3DBuffer::GetMode", t_GetMode },
        { "IDirectSound3DBuffer::GetPosition", t_GetPosition }, { "IDirectSound3DBuffer::GetVelocity", t_zero1 },
        { "IDirectSound3DBuffer::SetAllParameters", t_SetAllParameters }, { "IDirectSound3DBuffer::SetConeAngles", t_ok3 },
        { "IDirectSound3DBuffer::SetConeOrientation", t_ok4 }, { "IDirectSound3DBuffer::SetConeOutsideVolume", t_ok2 },
        { "IDirectSound3DBuffer::SetMaxDistance", t_SetMaxDistance }, { "IDirectSound3DBuffer::SetMinDistance", t_SetMinDistance },
        { "IDirectSound3DBuffer::SetMode", t_SetMode }, { "IDirectSound3DBuffer::SetPosition", t_SetPosition },
        { "IDirectSound3DBuffer::SetVelocity", t_ok4 }, { 0, 0 } };
    static const win32hle_shim l[] = {
        { "IDirectSound3DListener::QueryInterface", hle_com_QueryInterface }, { "IDirectSound3DListener::AddRef", hle_com_AddRef },
        { "IDirectSound3DListener::Release", l_Release }, { "IDirectSound3DListener::GetAllParameters", l_GetAllParameters },
        { "IDirectSound3DListener::GetDistanceFactor", l_GetDistanceFactor },
        { "IDirectSound3DListener::GetDopplerFactor", t_zero1 }, { "IDirectSound3DListener::GetOrientation", l_GetOrientation },
        { "IDirectSound3DListener::GetPosition", l_GetPosition }, { "IDirectSound3DListener::GetRolloffFactor", l_GetRolloffFactor },
        { "IDirectSound3DListener::GetVelocity", t_zero1 }, { "IDirectSound3DListener::SetAllParameters", l_SetAllParameters },
        { "IDirectSound3DListener::SetDistanceFactor", l_SetDistanceFactor },
        { "IDirectSound3DListener::SetDopplerFactor", t_ok2 }, { "IDirectSound3DListener::SetOrientation", l_SetOrientation },
        { "IDirectSound3DListener::SetPosition", l_SetPosition }, { "IDirectSound3DListener::SetRolloffFactor", l_SetRolloffFactor },
        { "IDirectSound3DListener::SetVelocity", t_ok4 }, { "IDirectSound3DListener::CommitDeferredSettings", l_Commit }, { 0, 0 } };
    g_vt_3db = hle_com_vtable(t);
    g_vt_lis = hle_com_vtable(l);
}

/* ---- IDirectSound ---- */
static void d_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) hle_com_free(self);
    RET(n, 1);
}
static void d_CreateSoundBuffer(void) {                  /* (this, &DSBUFFERDESC, &out, outer) */
    uint32_t d = A32(1), out = A32(2);
    if (!d || !out) RET(DSERR_INVALIDPARAM, 4);
    uint32_t flags = MEM32(d + 4), bytes = MEM32(d + 8), wfx = MEM32(d + 16);
    dsbuf *b = (dsbuf *)calloc(1, sizeof *b);
    b->flags = flags, b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
    b->is3d = (flags & 0x10u) != 0;
    b->mind = 1.0f; b->maxd = 1e9f;
    b->channels = 2, b->bits = 16, b->rate = 22050, b->block = 4;
    if (wfx) {
        b->channels = MEM16(wfx + 2), b->rate = (int)MEM32(wfx + 4), b->block = MEM16(wfx + 12), b->bits = MEM16(wfx + 14);
        if (MEM16(wfx) != 1 || (b->bits != 8 && b->bits != 16) || b->channels < 1 || b->channels > 2) {
            fprintf(stderr, "[dsound] buffer format tag %u, %d bits, %d channels: not PCM this mixer plays\n", MEM16(wfx), b->bits, b->channels);
            free(b);
            RET(DSERR_INVALIDPARAM, 4);
        }
    }
    if (b->primary) bytes = 4096 * 4;
    if (!bytes || !b->block || bytes % b->block || b->rate <= 0) { free(b); RET(DSERR_INVALIDPARAM, 4); }
    b->bytes = bytes;
    b->data = (uint8_t *)hle_alloc(bytes ? bytes : 4);
    if (b->bits == 8) memset(b->data, 128, bytes);
    b->pcm = (uint8_t*)malloc(bytes ? bytes : 4);
    if (!b->data || !b->pcm) { free(b->data); free(b->pcm); free(b); RET(0x8007000Eu, 4); }
    memcpy(b->pcm, b->data, bytes);
    b->data_refs = (int *)malloc(sizeof(int));
    *b->data_refs = 1;
    gains(b);
    uint32_t o = new_buffer(b);
    if (!o) { hle_free(b->data); free(b->pcm); free(b->data_refs); free(b); RET(DSERR_INVALIDPARAM, 4); }
    MEM32(out) = o;
    RET(DS_OK, 4);
}
static void d_GetCaps(void) {                            /* (this, &DSCAPS) */
    uint32_t c = A32(1);
    if (c) {
        uint32_t size = MEM32(c);
        memset(hle_ptr(c + 4), 0, size > 4 ? size - 4 : 0);
        MEM32(c + 4) = 0x0F5Fu;                          /* the primary/secondary 8/16-bit mono/stereo, continuous rate */
        MEM32(c + 8) = 100, MEM32(c + 12) = 100000;      /* min/max secondary rate */
        MEM32(c + 16) = 1;                               /* primary buffers */
        MEM32(c + 20) = 64, MEM32(c + 24) = 64, MEM32(c + 28) = 64;   /* max hw mixing: all, static, streaming */
        MEM32(c + 32) = 64, MEM32(c + 36) = 64, MEM32(c + 40) = 64;   /* free hw */
    }
    RET(DS_OK, 2);
}
static void d_DuplicateSoundBuffer(void) {               /* (this, original, &out) */
    dsbuf *src = buf_of(A32(1));
    if (!src || !A32(2)) RET(DSERR_INVALIDPARAM, 3);
    dsbuf *b = (dsbuf *)malloc(sizeof *b);
    AcquireSRWLockExclusive(&g_mix);
    *b = *src;
    (*b->data_refs)++;
    ReleaseSRWLockExclusive(&g_mix);
    b->playing = 0, b->pos = 0;
    uint32_t o = new_buffer(b);
    MEM32(A32(2)) = o;
    if (!o) { --*b->data_refs; free(b); }
    RET(o ? DS_OK : DSERR_INVALIDPARAM, 3);
}
static void d_SetCooperativeLevel(void) { RET(DS_OK, 3); }
static void d_Compact(void) { RET(DS_OK, 1); }
static void d_GetSpeakerConfig(void) { if (A32(1)) MEM32(A32(1)) = 4; RET(DS_OK, 2); }   /* DSSPEAKER_STEREO */
static void d_SetSpeakerConfig(void) { RET(DS_OK, 2); }
static void d_Initialize(void) { RET(DS_OK, 2); }

static void vtables(void) {
    if (g_vt_ds) return;
    static const win32hle_shim ds[] = {
        { "IDirectSound::QueryInterface", hle_com_QueryInterface }, { "IDirectSound::AddRef", hle_com_AddRef },
        { "IDirectSound::Release", d_Release }, { "IDirectSound::CreateSoundBuffer", d_CreateSoundBuffer },
        { "IDirectSound::GetCaps", d_GetCaps }, { "IDirectSound::DuplicateSoundBuffer", d_DuplicateSoundBuffer },
        { "IDirectSound::SetCooperativeLevel", d_SetCooperativeLevel }, { "IDirectSound::Compact", d_Compact },
        { "IDirectSound::GetSpeakerConfig", d_GetSpeakerConfig }, { "IDirectSound::SetSpeakerConfig", d_SetSpeakerConfig },
        { "IDirectSound::Initialize", d_Initialize },
        { "IDirectSound8::VerifyCertification", d8_VerifyCertification }, { 0, 0 } };
    static const win32hle_shim buf[] = {
        { "IDirectSoundBuffer::QueryInterface", b_QueryInterface }, { "IDirectSoundBuffer::AddRef", hle_com_AddRef },
        { "IDirectSoundBuffer::Release", b_Release }, { "IDirectSoundBuffer::GetCaps", b_GetCaps },
        { "IDirectSoundBuffer::GetCurrentPosition", b_GetCurrentPosition }, { "IDirectSoundBuffer::GetFormat", b_GetFormat },
        { "IDirectSoundBuffer::GetVolume", b_GetVolume }, { "IDirectSoundBuffer::GetPan", b_GetPan },
        { "IDirectSoundBuffer::GetFrequency", b_GetFrequency }, { "IDirectSoundBuffer::GetStatus", b_GetStatus },
        { "IDirectSoundBuffer::Initialize", b_Initialize }, { "IDirectSoundBuffer::Lock", b_Lock },
        { "IDirectSoundBuffer::Play", b_Play }, { "IDirectSoundBuffer::SetCurrentPosition", b_SetCurrentPosition },
        { "IDirectSoundBuffer::SetFormat", b_SetFormat }, { "IDirectSoundBuffer::SetVolume", b_SetVolume },
        { "IDirectSoundBuffer::SetPan", b_SetPan }, { "IDirectSoundBuffer::SetFrequency", b_SetFrequency },
        { "IDirectSoundBuffer::Stop", b_Stop }, { "IDirectSoundBuffer::Unlock", b_Unlock },
        { "IDirectSoundBuffer::Restore", b_Restore },
        { "IDirectSoundBuffer8::SetFX", b8_SetFX }, { "IDirectSoundBuffer8::AcquireResources", b8_AcquireResources },
        { "IDirectSoundBuffer8::GetObjectInPath", b8_GetObjectInPath }, { 0, 0 } };
    g_vt_ds = hle_com_vtable(ds);
    g_vt_buf = hle_com_vtable(buf);
    vtables3d();
}

static void s_DirectSoundCreate(void) {                  /* (guid, &out, outer) */
    vtables();
    if (!A32(1)) RET(DSERR_INVALIDPARAM, 3);
    if (!audio_open()) { MEM32(A32(1)) = 0; RET(DSERR_NODRIVER, 3); }
    MEM32(A32(1)) = hle_com_new(g_vt_ds, 12);
    RET(DS_OK, 3);
}
static void s_DirectSoundEnumerateA(void) {              /* (callback, context): the one device */
    static uint32_t desc, mod;                           /* the guest may keep them */
    if (!desc) desc = hle_guest_strdup("Primary Sound Driver"), mod = hle_guest_strdup("");
    uint32_t a[4] = { 0, desc, mod, A32(1) };
    if (A32(0)) hle_call_guest(A32(0), 4, a);
    RET(DS_OK, 2);
}

const native32_shim_t lt2_sound_shims[] = {
    { "dsound.dll#1", s_DirectSoundCreate }, { "DirectSoundCreate", s_DirectSoundCreate },
    { "dsound.dll#2", s_DirectSoundEnumerateA }, { "DirectSoundEnumerateA", s_DirectSoundEnumerateA },
    { "dsound.dll#11", s_DirectSoundCreate }, { "DirectSoundCreate8", s_DirectSoundCreate },
    { 0, 0 }
};
