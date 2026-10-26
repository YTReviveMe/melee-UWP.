/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <dolphin/ai.h>
#include "pc/pc.h"
#include "pc/music_stream.h"
#include <dolphin/ar.h>
#include <dolphin/ax.h>
#include <dolphin/axfx.h>
#include <dolphin/os.h>

#include <SDL3/SDL.h>
#include <math.h>
#if defined(MELEE_XBOX_UWP)
#include "../../platforms/xbox/pthread_compat.h"
#else
#include <pthread.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MELEE_XBOX_UWP
void pc_xbox_audio_start(float gain);
void pc_xbox_audio_push(const float* samples);
void pc_xbox_audio_set_volume(float gain);
void pc_xbox_audio_stop(void);
static bool s_xbox_native_audio;
#endif

static pthread_mutex_t s_audio_mutex;
static pthread_once_t s_audio_mutex_once = PTHREAD_ONCE_INIT;

static void init_audio_mutex(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_audio_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

static inline void audio_lock(void) {
    pthread_once(&s_audio_mutex_once, init_audio_mutex);
    pthread_mutex_lock(&s_audio_mutex);
}

static inline void audio_unlock(void) {
    pthread_mutex_unlock(&s_audio_mutex);
}

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define PC_AUDIO_SIMD_SSE2 1
#elif defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define PC_AUDIO_SIMD_NEON 1
#endif

static int pc_dbg_audio_stats(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("MELEE_AUDIO_STATS") != NULL;
    }
    return cached;
}

#define AX_VOICES 64
#define AX_RATE 32000
#define AX_FRAME 160 /* 5ms */

#define AX_FORMAT_ADPCM 0
#define AX_FORMAT_PCM16 10
#define AX_FORMAT_PCM8 25

typedef struct Voice {
    AXVPB vpb; /* handed to the game; vpb.pb is our native mirror */
    bool used;
    bool is_stream; /* true for HPS music stream voices */
    s16 prev, cur;  /* last two decoded samples, for interpolation */
    u32 frac;       /* 16.16 position between prev and cur */
    u16 pred_scale; /* ADPCM header of the current frame */
    s32 yn1, yn2;   /* ADPCM history */
    u32 loop_addr, end_addr, cur_addr;
    u32 age; /* AX frames this voice has been running; MELEE_AUDIO_STATS */
} Voice;

static Voice s_voices[AX_VOICES];
static void (*s_frame_callback)(void);
static SDL_AudioStream* s_stream;
static float s_master_volume = 1.0f;
static float s_music_volume = 1.0f;
static float s_sfx_volume = 1.0f;

void pc_audio_set_volume(float volume) {
    s_master_volume = volume;
#ifdef MELEE_XBOX_UWP
    pc_xbox_audio_set_volume(volume);
#endif
    if (s_stream)
        SDL_SetAudioStreamGain(s_stream, volume);
}

void pc_audio_set_music_volume(float volume) {
    s_music_volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
}

void pc_audio_set_sfx_volume(float volume) {
    s_sfx_volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
}

float pc_audio_get_music_volume(void) {
    return s_music_volume;
}

float pc_audio_get_sfx_volume(void) {
    return s_sfx_volume;
}

__attribute__((weak)) float pc_get_music_volume(void) {
    return s_music_volume;
}

__attribute__((weak)) float pc_get_sfx_volume(void) {
    return s_sfx_volume;
}

__attribute__((weak)) void pc_music_stream_mix(float* dst_left, float* dst_right, int num_samples);
static u8* s_aram;
static float s_master = 1.0f;

static inline u16 be16(u16 v) {
    return __builtin_bswap16(v);
}

static inline u32 addr32(u16 hi, u16 lo) {
    return ((u32)hi << 16) | lo;
}

static inline void set_addr(u16* hi, u16* lo, u32 addr) {
    *hi = (u16)(addr >> 16);
    *lo = (u16)addr;
}

static inline s16 clamp16(s64 v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v;
}

static bool next_sample(Voice* v, s16* out) {
    AXPB* pb = &v->vpb.pb;
    u16 format = pb->addr.format;

    if (!pb->state || !s_aram) {
        return false;
    }

    switch (format) {
    case AX_FORMAT_ADPCM: {
        if ((v->cur_addr & 15) == 0) {
            if ((v->cur_addr >> 1) + 2 > PC_ARAM_SIZE) {
                return false;
            }
            v->pred_scale = s_aram[v->cur_addr >> 1];
            v->cur_addr += 2;
        }
        if ((v->cur_addr >> 1) >= PC_ARAM_SIZE) {
            return false;
        }
        u8 byte = s_aram[v->cur_addr >> 1];
        s32 nibble = (v->cur_addr & 1) ? (byte & 0xF) : (byte >> 4);
        nibble = nibble >= 8 ? nibble - 16 : nibble;
        s32 scale = 1 << (v->pred_scale & 0xF);
        u32 coef = (v->pred_scale >> 4) & 7;
        s32 c0 = (s16)pb->adpcm.a[coef][0];
        s32 c1 = (s16)pb->adpcm.a[coef][1];
        s64 acc = (s64)nibble * scale * 2048 + 1024 + (s64)c0 * v->yn1 + (s64)c1 * v->yn2;
        s32 sample = clamp16(acc >> 11);
        v->yn2 = v->yn1;
        v->yn1 = sample;
        *out = (s16)sample;
        break;
    }
    case AX_FORMAT_PCM16: {
        const u8* p;
        if (v->cur_addr >= PC_ARAM_SIZE / 2) {
            return false;
        }
        p = &s_aram[v->cur_addr * 2];
        *out = (s16)((p[0] << 8) | p[1]);
        break;
    }
    case AX_FORMAT_PCM8:
        if (v->cur_addr >= PC_ARAM_SIZE) {
            return false;
        }
        *out = (s16)((s8)s_aram[v->cur_addr] * 256);
        break;
    default:
        return false;
    }

    if (v->cur_addr == v->end_addr) {
        if (pb->addr.loopFlag) {
            v->cur_addr = v->loop_addr;
            if (format == AX_FORMAT_ADPCM) {
                v->pred_scale = pb->adpcmLoop.loop_pred_scale;
                v->yn1 = (s16)pb->adpcmLoop.loop_yn1;
                v->yn2 = (s16)pb->adpcmLoop.loop_yn2;
            }
        } else {
            pb->state = 0;
        }
    } else {
        v->cur_addr++;
    }
    return true;
}

typedef struct AuxBus {
    void (*cb)(void*, void*);
    void* ctx;
    long ch[3][AX_FRAME];  // NOLINT: Dolphin SDK AXFX_BUFFERUPDATE expects long*
    int quiet; /* consecutive frames with a silent input and a wet output below the floor */
    int hold;  /* frames that covers the effect's longest delay line */
    int runs;  /* frames the effect ran; MELEE_AUDIO_STATS */
} AuxBus;

static AuxBus s_auxA, s_auxB;
static bool s_aux_on = true;

void pc_audio_set_reverb(bool on) {
    s_aux_on = on;
}

static bool is_music_stream(Voice* v) {
    if (v->vpb.pb.addr.format != AX_FORMAT_ADPCM) {
        return false;
    }
    if (v->is_stream) {
        return true;
    }
    if ((u32)v->vpb.priority == 0x1D) {
        v->is_stream = true;
        return true;
    }
    if (v->vpb.pb.addr.loopFlag) {
        u32 diff = v->loop_addr > v->cur_addr ? (v->loop_addr - v->cur_addr) :
                                                (v->cur_addr - v->loop_addr);
        if (diff >= 0x10000) {
            v->is_stream = true;
            return true;
        }
    }
    return false;
}

static void mix_voice(Voice* v, float* out) {
    AXPB* pb = &v->vpb.pb;
    if (!pb->state) {
        return;
    }

    u32 ratio = addr32(pb->src.ratioHi, pb->src.ratioLo);
    s32 vol = pb->ve.currentVolume;
    s32 delta = pb->ve.currentDelta;

    float voice_gain = is_music_stream(v) ? pc_get_music_volume() : pc_get_sfx_volume();
    if (voice_gain < 0.0f) {
        voice_gain = 0.0f;
    }

    float vl = (pb->mix.vL / 32767.0f) * voice_gain;
    float vr = (pb->mix.vR / 32767.0f) * voice_gain;
    float al = (pb->mix.vAuxAL / 32767.0f) * voice_gain;
    float ar = (pb->mix.vAuxAR / 32767.0f) * voice_gain;
    float bl = (pb->mix.vAuxBL / 32767.0f) * voice_gain;
    float br = (pb->mix.vAuxBR / 32767.0f) * voice_gain;
    bool send_a = s_aux_on && (s_auxA.cb != NULL) && (al != 0.0f || ar != 0.0f);
    bool send_b = s_aux_on && (s_auxB.cb != NULL) && (bl != 0.0f || br != 0.0f);

    if (vol < 0) {
        vol = 0;
    } else if (vol > 32767) {
        vol = 32767;
    }

    bool is_silent = ((vol == 0 && delta == 0) || (vl == 0.0f && vr == 0.0f)) && !send_a && !send_b;

    if (ratio == 0) {
        if (is_silent) {
            return;
        }
        float t = (float)v->frac * (1.0f / 65536.0f);
        float s = ((float)v->prev + t * (float)(v->cur - v->prev)) * (1.0f / 32768.0f);
        for (int i = 0; i < AX_FRAME; i++) {
            float g = (float)vol * (1.0f / 32767.0f);
            float sv = s * g;
            out[i * 2] += sv * vl;
            out[i * 2 + 1] += sv * vr;
            if (send_a) {
                s_auxA.ch[0][i] += (long)(sv * al * 32767.0f);
                s_auxA.ch[1][i] += (long)(sv * ar * 32767.0f);
            }
            if (send_b) {
                s_auxB.ch[0][i] += (long)(sv * bl * 32767.0f);
                s_auxB.ch[1][i] += (long)(sv * br * 32767.0f);
            }
            vol += delta;
            if (vol < 0) {
                vol = 0;
            } else if (vol > 32767) {
                vol = 32767;
            }
        }
        pb->ve.currentVolume = (u16)vol;
        set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
        return;
    }

    if (is_silent) {
        if (ratio == 0x10000) {
            for (int i = 0; i < AX_FRAME; i++) {
                s16 s;
                if (!next_sample(v, &s)) {
                    pb->state = 0;
                    pb->ve.currentVolume = (u16)(vol < 0 ? 0 : vol > 32767 ? 32767 : vol);
                    set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
                    return;
                }
                v->prev = v->cur;
                v->cur = s;
                vol += delta;
                if (vol < 0) {
                    vol = 0;
                } else if (vol > 32767) {
                    vol = 32767;
                }
            }
        } else {
            for (int i = 0; i < AX_FRAME; i++) {
                v->frac += ratio;
                while (v->frac >= 0x10000) {
                    s16 s;
                    if (!next_sample(v, &s)) {
                        pb->state = 0;
                        pb->ve.currentVolume = (u16)(vol < 0 ? 0 : vol > 32767 ? 32767 : vol);
                        set_addr(
                            &pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
                        return;
                    }
                    v->prev = v->cur;
                    v->cur = s;
                    v->frac -= 0x10000;
                }
                vol += delta;
                if (vol < 0) {
                    vol = 0;
                } else if (vol > 32767) {
                    vol = 32767;
                }
            }
        }
        pb->ve.currentVolume = (u16)vol;
        set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
        return;
    }

    if (ratio == 0x10000 && v->frac == 0) {
        if (delta == 0 && !send_a && !send_b) {
            float scale_l = ((float)vol * (1.0f / 32767.0f)) * vl * (1.0f / 32768.0f);
            float scale_r = ((float)vol * (1.0f / 32767.0f)) * vr * (1.0f / 32768.0f);
            for (int i = 0; i < AX_FRAME; i++) {
                s16 s;
                if (!next_sample(v, &s)) {
                    pb->state = 0;
                    pb->ve.currentVolume = (u16)vol;
                    set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
                    return;
                }
                v->prev = v->cur;
                v->cur = s;
                float smp = (float)v->prev;
                out[i * 2] += smp * scale_l;
                out[i * 2 + 1] += smp * scale_r;
            }
            pb->ve.currentVolume = (u16)vol;
            set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
            return;
        }

        for (int i = 0; i < AX_FRAME; i++) {
            s16 s;
            if (!next_sample(v, &s)) {
                pb->state = 0;
                pb->ve.currentVolume = (u16)(vol < 0 ? 0 : vol > 32767 ? 32767 : vol);
                set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
                return;
            }
            v->prev = v->cur;
            v->cur = s;
            float smp = (float)v->prev * (1.0f / 32768.0f);
            float g = (float)vol * (1.0f / 32767.0f);
            float sv = smp * g;
            out[i * 2] += sv * vl;
            out[i * 2 + 1] += sv * vr;
            if (send_a) {
                s_auxA.ch[0][i] += (long)(sv * al * 32767.0f);
                s_auxA.ch[1][i] += (long)(sv * ar * 32767.0f);
            }
            if (send_b) {
                s_auxB.ch[0][i] += (long)(sv * bl * 32767.0f);
                s_auxB.ch[1][i] += (long)(sv * br * 32767.0f);
            }
            vol += delta;
            if (vol < 0) {
                vol = 0;
            } else if (vol > 32767) {
                vol = 32767;
            }
        }
        pb->ve.currentVolume = (u16)vol;
        set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
        return;
    }

    if (ratio == 0x10000) {
        float t = (float)v->frac * (1.0f / 65536.0f);
        for (int i = 0; i < AX_FRAME; i++) {
            s16 s;
            if (!next_sample(v, &s)) {
                pb->state = 0;
                pb->ve.currentVolume = (u16)(vol < 0 ? 0 : vol > 32767 ? 32767 : vol);
                set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
                return;
            }
            v->prev = v->cur;
            v->cur = s;
            float smp = ((float)v->prev + t * (float)(v->cur - v->prev)) * (1.0f / 32768.0f);
            float g = (float)vol * (1.0f / 32767.0f);
            float sv = smp * g;
            out[i * 2] += sv * vl;
            out[i * 2 + 1] += sv * vr;
            if (send_a) {
                s_auxA.ch[0][i] += (long)(sv * al * 32767.0f);
                s_auxA.ch[1][i] += (long)(sv * ar * 32767.0f);
            }
            if (send_b) {
                s_auxB.ch[0][i] += (long)(sv * bl * 32767.0f);
                s_auxB.ch[1][i] += (long)(sv * br * 32767.0f);
            }
            vol += delta;
            if (vol < 0) {
                vol = 0;
            } else if (vol > 32767) {
                vol = 32767;
            }
        }
        pb->ve.currentVolume = (u16)vol;
        set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
        return;
    }

    for (int i = 0; i < AX_FRAME; i++) {
        v->frac += ratio;
        while (v->frac >= 0x10000) {
            s16 s;
            if (!next_sample(v, &s)) {
                pb->state = 0;
                pb->ve.currentVolume = (u16)(vol < 0 ? 0 : vol > 32767 ? 32767 : vol);
                set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
                return;
            }
            v->prev = v->cur;
            v->cur = s;
            v->frac -= 0x10000;
        }
        float t = (float)v->frac * (1.0f / 65536.0f);
        float s = ((float)v->prev + t * (float)(v->cur - v->prev)) * (1.0f / 32768.0f);
        float g = (float)vol * (1.0f / 32767.0f);
        float sv = s * g;
        out[i * 2] += sv * vl;
        out[i * 2 + 1] += sv * vr;
        if (send_a) {
            s_auxA.ch[0][i] += (long)(sv * al * 32767.0f);
            s_auxA.ch[1][i] += (long)(sv * ar * 32767.0f);
        }
        if (send_b) {
            s_auxB.ch[0][i] += (long)(sv * bl * 32767.0f);
            s_auxB.ch[1][i] += (long)(sv * br * 32767.0f);
        }
        vol += delta;
        if (vol < 0) {
            vol = 0;
        } else if (vol > 32767) {
            vol = 32767;
        }
    }
    pb->ve.currentVolume = (u16)vol;
    set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
}

#define AUX_QUIET_ENERGY (2.0f * AX_FRAME * 1e-8f)

static void run_aux(AuxBus* bus, float* out) {
    struct AXFX_BUFFERUPDATE bu;
    void (*cb)(void*, void*) = bus->cb;
    void* ctx = bus->ctx;
    float energy = 0.0f;
    long input = 0;  // NOLINT: the bus is long

    if (cb == NULL || ctx == NULL) {
        return;
    }
    for (int i = 0; i < AX_FRAME; i++) {
        input |= bus->ch[0][i] | bus->ch[1][i];
    }
    if (input == 0 && bus->quiet >= bus->hold) {
        return;
    }
    bu.left = bus->ch[0];
    bu.right = bus->ch[1];
    bu.surround = bus->ch[2];
    cb(&bu, ctx);
    bus->runs++;
    for (int i = 0; i < AX_FRAME; i++) {
        float l = (float)bus->ch[0][i] * (1.0f / 32767.0f);
        float r = (float)bus->ch[1][i] * (1.0f / 32767.0f);
        out[i * 2] += l;
        out[i * 2 + 1] += r;
        energy += l * l + r * r;
    }
    bus->quiet = input != 0 || energy >= AUX_QUIET_ENERGY ? 0 : bus->quiet + 1;
}

static void render_frame(float* out) {
    memset(out, 0, sizeof(float) * AX_FRAME * 2);
    memset(s_auxA.ch, 0, sizeof(s_auxA.ch));
    memset(s_auxB.ch, 0, sizeof(s_auxB.ch));

    BOOL intr = OSDisableInterrupts();
    audio_lock();
    if (s_frame_callback) {
        s_frame_callback();
    }
    OSRestoreInterrupts(intr);

    int n_used = 0, n_running = 0, n_stopped = 0, n_zero_mix = 0;
    int n_zero_ratio = 0, n_loop = 0, n_old = 0;
    for (int i = 0; i < AX_VOICES; i++) {
        Voice* v = &s_voices[i];
        if (!v->used) {
            v->age = 0;
            continue;
        }
        n_used++;
        if (++v->age == 2001) { /* 10s of AX frames */
            if (pc_dbg_audio_stats()) {
                fprintf(stderr,
                    "stuck voice %u: state=%u fmt=%u loop=%u ratio=%u"
                    " cur=%u end=%u loopaddr=%u vol=%u prio=%d\n",
                    v->vpb.index, v->vpb.pb.state, v->vpb.pb.addr.format, v->vpb.pb.addr.loopFlag,
                    addr32(v->vpb.pb.src.ratioHi, v->vpb.pb.src.ratioLo), v->cur_addr, v->end_addr,
                    v->loop_addr, v->vpb.pb.ve.currentVolume, v->vpb.priority);
            }
        }
        if (v->age > 2000) {
            n_old++;
        }
        if (v->vpb.pb.state != 1) {
            n_stopped++;
            continue;
        }
        n_running++;
        if (v->vpb.pb.mix.vL == 0 && v->vpb.pb.mix.vR == 0) {
            n_zero_mix++;
        }
        if (addr32(v->vpb.pb.src.ratioHi, v->vpb.pb.src.ratioLo) == 0) {
            n_zero_ratio++;
        }
        if (v->vpb.pb.addr.loopFlag) {
            n_loop++;
        }
        mix_voice(v, out);
    }

    run_aux(&s_auxA, out);
    run_aux(&s_auxB, out);
    audio_unlock();

    if (pc_music_stream_mix) {
        pc_music_stream_mix(out, NULL, AX_FRAME);
    }
    if (pc_dbg_audio_stats()) {
        static uint64_t frames;
        frames++;
        if ((frames % 100) == 0) { /* every 100 * 5ms = 0.5s of output */
            fprintf(stderr,
                "voices t=%.1fs used=%d running=%d stopped=%d"
                " zero_mix=%d zero_ratio=%d loop=%d old=%d aux=%d/%d\n",
                frames * (double)AX_FRAME / AX_RATE, n_used, n_running, n_stopped, n_zero_mix,
                n_zero_ratio, n_loop, n_old, s_auxA.runs, s_auxB.runs);
            s_auxA.runs = s_auxB.runs = 0;
        }
    }
#if defined(PC_AUDIO_SIMD_SSE2)
    const __m128 vmaster = _mm_set1_ps(s_master);
    const __m128 vone = _mm_set1_ps(1.0f);
    const __m128 vneg_one = _mm_set1_ps(-1.0f);
    for (int i = 0; i < AX_FRAME * 2; i += 8) {
        __m128 v0 = _mm_loadu_ps(&out[i]);
        __m128 v1 = _mm_loadu_ps(&out[i + 4]);
        v0 = _mm_mul_ps(v0, vmaster);
        v1 = _mm_mul_ps(v1, vmaster);
        v0 = _mm_min_ps(v0, vone);
        v1 = _mm_min_ps(v1, vone);
        v0 = _mm_max_ps(v0, vneg_one);
        v1 = _mm_max_ps(v1, vneg_one);
        _mm_storeu_ps(&out[i], v0);
        _mm_storeu_ps(&out[i + 4], v1);
    }
#elif defined(PC_AUDIO_SIMD_NEON)
    const float32x4_t vmaster = vdupq_n_f32(s_master);
    const float32x4_t vone = vdupq_n_f32(1.0f);
    const float32x4_t vneg_one = vdupq_n_f32(-1.0f);
    for (int i = 0; i < AX_FRAME * 2; i += 8) {
        float32x4_t v0 = vld1q_f32(&out[i]);
        float32x4_t v1 = vld1q_f32(&out[i + 4]);
        v0 = vmulq_f32(v0, vmaster);
        v1 = vmulq_f32(v1, vmaster);
        v0 = vminq_f32(v0, vone);
        v1 = vminq_f32(v1, vone);
        v0 = vmaxq_f32(v0, vneg_one);
        v1 = vmaxq_f32(v1, vneg_one);
        vst1q_f32(&out[i], v0);
        vst1q_f32(&out[i + 4], v1);
    }
#else
    for (int i = 0; i < AX_FRAME * 2; i++) {
        float s = out[i] * s_master;
        out[i] = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
    }
#endif
}

static FILE* s_dump;

static void SDLCALL audio_pull(void* userdata, SDL_AudioStream* stream, int additional, int total) {
    static float frame[AX_FRAME * 2];
    (void)userdata;
    (void)total;
    while (additional > 0) {
        render_frame(frame);
#ifdef MELEE_XBOX_UWP
        if (s_xbox_native_audio)
            pc_xbox_audio_push(frame);
#endif
        SDL_PutAudioStreamData(stream, frame, sizeof(frame));
        if (s_dump) {
            fwrite(frame, sizeof(frame), 1, s_dump);
        }
        additional -= (int)sizeof(frame);
    }
}

#ifdef __EMSCRIPTEN__
void pc_audio_pump(void) {
    static int pumping;
    if (pumping || !s_stream)
        return;
    pumping = 1;
    float frame[AX_FRAME * 2];
    while (SDL_GetAudioStreamQueued(s_stream) < AX_RATE * 2 * sizeof(float) / 20) {
        render_frame(frame);
        if (!SDL_PutAudioStreamData(s_stream, frame, sizeof(frame)))
            break;
    }
    pumping = 0;
}
#endif

void AXInit(void) {
    pthread_once(&s_audio_mutex_once, init_audio_mutex);
    memset(s_voices, 0, sizeof(s_voices));
    for (int i = 0; i < AX_VOICES; i++) {
        s_voices[i].vpb.index = i;
    }
    s_aram = aurora_aram_base();
    if (s_dump == NULL && getenv("MELEE_AUDIO_DUMP") != NULL) {
        s_dump = fopen(getenv("MELEE_AUDIO_DUMP"), "wb");
    }
    if (s_stream == NULL) {
        const SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, AX_RATE};
#ifdef MELEE_XBOX_UWP
        const char* xbox_audio_driver = getenv("MELEE_XBOX_AUDIO_DRIVER");
        if (xbox_audio_driver == NULL || xbox_audio_driver[0] == '\0')
            xbox_audio_driver = "dummy";
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, xbox_audio_driver);
#endif
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, "audio: SDL_InitSubSystem failed: %s\n", SDL_GetError());
            return;
        }
#ifdef __EMSCRIPTEN__
        s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
#else
        s_stream =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_pull, NULL);
#endif
        if (s_stream)
            SDL_SetAudioStreamGain(s_stream, s_master_volume);
        if (s_stream == NULL) {
            fprintf(stderr, "audio: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
            return;
        }
#ifdef MELEE_XBOX_UWP
        s_xbox_native_audio = strcmp(SDL_GetCurrentAudioDriver(), "dummy") == 0;
        if (s_xbox_native_audio)
            pc_xbox_audio_start(s_master_volume);
#endif
        SDL_ResumeAudioStreamDevice(s_stream);
    }
}

void AXQuit(void) {
    if (s_stream) {
        SDL_DestroyAudioStream(s_stream);
        s_stream = NULL;
    }
#ifdef MELEE_XBOX_UWP
    s_xbox_native_audio = false;
    pc_xbox_audio_stop();
#endif
}

void AXRegisterCallback(void (*callback)()) {
    s_frame_callback = (void (*)(void))callback;
}

AXVPB* AXAcquireVoice(u32 priority, void (*callback)(void*), u32 userContext) {
    audio_lock();
    Voice* pick = NULL;
    for (int i = 0; i < AX_VOICES; i++) {
        if (!s_voices[i].used) {
            pick = &s_voices[i];
            break;
        }
    }
    if (pick == NULL) {
        for (int i = 0; i < AX_VOICES; i++) {
            Voice* v = &s_voices[i];
            if ((u32)v->vpb.priority < priority &&
                (pick == NULL || v->vpb.priority < pick->vpb.priority))
            {
                pick = v;
            }
        }
        if (pick != NULL && pick->vpb.callback != NULL) {
            pick->vpb.callback(&pick->vpb);
        }
    }
    if (pick != NULL) {
        u32 index = pick->vpb.index;
        memset(pick, 0, sizeof(*pick));
        pick->used = true;
        pick->vpb.index = index;
        pick->vpb.priority = (int)priority;
        pick->vpb.callback = callback;
        pick->vpb.userContext = userContext;
        if (priority == 0x1D) {
            pick->is_stream = true;
        }
    }
    audio_unlock();
    return pick ? &pick->vpb : NULL;
}

void AXFreeVoice(AXVPB* p) {
    if (p) {
        audio_lock();
        Voice* v = (Voice*)p;
        v->used = false;
        v->is_stream = false;
        v->vpb.pb.state = 0;
        v->vpb.priority = 0;
        audio_unlock();
    }
}

void AXSetVoicePriority(AXVPB* p, u32 priority) {
    audio_lock();
    Voice* v = (Voice*)p;
    p->priority = (int)priority;
    if (priority == 0x1D) {
        v->is_stream = true;
    }
    audio_unlock();
}

void AXSetVoiceState(AXVPB* p, u16 state) {
    audio_lock();
    p->pb.state = state;
    audio_unlock();
}

void AXSetVoiceMix(AXVPB* p, AXPBMIX* mix) {
    audio_lock();
    p->pb.mix = *mix;
    audio_unlock();
}

void AXSetVoiceItdOn(AXVPB* p) {
    audio_lock();
    p->pb.itd.flag = 1;
    audio_unlock();
}

void AXSetVoiceItdTarget(AXVPB* p, u16 l, u16 r) {
    audio_lock();
    p->pb.itd.targetShiftL = l;
    p->pb.itd.targetShiftR = r;
    audio_unlock();
}

void AXSetVoiceVe(AXVPB* p, AXPBVE* ve) {
    audio_lock();
    p->pb.ve = *ve;
    audio_unlock();
}

void AXSetVoiceVeDelta(AXVPB* p, s16 delta) {
    audio_lock();
    p->pb.ve.currentDelta = delta;
    audio_unlock();
}

void AXSetVoiceAddr(AXVPB* p, AXPBADDR* addr) {
    audio_lock();
    Voice* v = (Voice*)p;
    AXPBADDR* dst = &p->pb.addr;
    dst->loopFlag = be16(addr->loopFlag);
    dst->format = be16(addr->format);
    v->loop_addr = addr32(be16(addr->loopAddressHi), be16(addr->loopAddressLo));
    v->end_addr = addr32(be16(addr->endAddressHi), be16(addr->endAddressLo));
    v->cur_addr = addr32(be16(addr->currentAddressHi), be16(addr->currentAddressLo));

    {
        static int log_cached = -1;
        const u32 limit = (u32)(PC_ARAM_SIZE * 2u);

        if (log_cached < 0) {
            log_cached = getenv("MELEE_AUDIO_ADDR") != NULL;
        }
        if (v->end_addr > limit || v->cur_addr > limit) {
            static uint32_t bad;
            if (log_cached || ++bad <= 4) {
                fprintf(stderr,
                    "audio: voice addr outside ARAM: cur=%u end=%u"
                    " limit=%u\n",
                    v->cur_addr, v->end_addr, limit);
            }
        } else if (log_cached) {
            static uint32_t ok;
            if (++ok <= 4 || ok % 200 == 0) {
                fprintf(stderr, "audio: voice addr ok: cur=%u end=%u (%u)\n", v->cur_addr,
                    v->end_addr, ok);
            }
        }
    }
    set_addr(&dst->loopAddressHi, &dst->loopAddressLo, v->loop_addr);
    set_addr(&dst->endAddressHi, &dst->endAddressLo, v->end_addr);
    set_addr(&dst->currentAddressHi, &dst->currentAddressLo, v->cur_addr);
    v->frac = 0;
    v->prev = v->cur = 0;
    audio_unlock();
}

void AXSetVoiceLoop(AXVPB* p, u16 loop) {
    audio_lock();
    p->pb.addr.loopFlag = loop;
    audio_unlock();
}

void AXSetVoiceLoopAddr(AXVPB* p, u32 addr) {
    audio_lock();
    Voice* v = (Voice*)p;
    v->loop_addr = addr;
    v->is_stream = true;
    set_addr(&p->pb.addr.loopAddressHi, &p->pb.addr.loopAddressLo, addr);
    audio_unlock();
}

void AXSetVoiceEndAddr(AXVPB* p, u32 addr) {
    audio_lock();
    Voice* v = (Voice*)p;
    v->end_addr = addr;
    v->is_stream = true;
    set_addr(&p->pb.addr.endAddressHi, &p->pb.addr.endAddressLo, addr);
    audio_unlock();
}

void AXSetVoiceCurrentAddr(AXVPB* p, u32 addr) {
    audio_lock();
    Voice* v = (Voice*)p;
    v->cur_addr = addr;
    v->is_stream = true;
    set_addr(&p->pb.addr.currentAddressHi, &p->pb.addr.currentAddressLo, addr);
    audio_unlock();
}

void AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* a) {
    audio_lock();
    Voice* v = (Voice*)p;
    AXPBADPCM* dst = &p->pb.adpcm;
    for (int i = 0; i < 8; i++) {
        dst->a[i][0] = be16(a->a[i][0]);
        dst->a[i][1] = be16(a->a[i][1]);
    }
    dst->gain = be16(a->gain);
    dst->pred_scale = be16(a->pred_scale);
    dst->yn1 = be16(a->yn1);
    dst->yn2 = be16(a->yn2);
    v->pred_scale = dst->pred_scale;
    v->yn1 = (s16)dst->yn1;
    v->yn2 = (s16)dst->yn2;
    audio_unlock();
}

void AXSetVoiceAdpcmLoop(AXVPB* p, AXPBADPCMLOOP* l) {
    audio_lock();
    p->pb.adpcmLoop.loop_pred_scale = be16(l->loop_pred_scale);
    p->pb.adpcmLoop.loop_yn1 = be16(l->loop_yn1);
    p->pb.adpcmLoop.loop_yn2 = be16(l->loop_yn2);
    audio_unlock();
}

#define AX_MAX_RATIO 0x40000u /* 4.0 in 16.16 */

static u32 clamp_ratio(u32 ratio) {
    if (ratio > AX_MAX_RATIO) {
        static uint32_t capped;
        if (++capped <= 8 || pc_dbg_audio_stats()) {
            fprintf(stderr, "audio: SRC ratio %u (%.2fx) capped to AX's 4.0 (#%u)\n", ratio,
                ratio / 65536.0, capped);
        }
        return AX_MAX_RATIO;
    }
    return ratio;
}

void AXSetVoiceSrc(AXVPB* p, AXPBSRC* s) {
    audio_lock();
    Voice* v = (Voice*)p;
    int i;

    set_addr(&p->pb.src.ratioHi, &p->pb.src.ratioLo, clamp_ratio(addr32(s->ratioHi, s->ratioLo)));
    p->pb.src.currentAddressFrac = s->currentAddressFrac;
    for (i = 0; i < 4; i++) {
        p->pb.src.last_samples[i] = s->last_samples[i];
    }
    v->frac = s->currentAddressFrac;
    audio_unlock();
}

void AXSetVoiceSrcRatio(AXVPB* p, float ratio) {
    audio_lock();
    if (!(isfinite(ratio) && ratio > 0.0f)) {
        ratio = 0.0f;
    } else if (ratio > 4.0f) {
        ratio = 4.0f;
    }
    u32 fixed = clamp_ratio((u32)(ratio * 65536.0f));
    set_addr(&p->pb.src.ratioHi, &p->pb.src.ratioLo, fixed);
    audio_unlock();
}

static int32_t axfx_longest_path(void (*cb)(void*, void*), void* ctx);

static void aux_register(AuxBus* bus, void (*cb)(void*, void*), void* ctx) {
    audio_lock();
    bus->cb = cb;
    bus->ctx = ctx;
    bus->quiet = 0;
    bus->hold = cb != NULL ? axfx_longest_path(cb, ctx) / AX_FRAME + 1 : 0;
    audio_unlock();
}

void AXRegisterAuxACallback(void (*callback)(void*, void*), void* context) {
    aux_register(&s_auxA, callback, context);
}

void AXRegisterAuxBCallback(void (*callback)(void*, void*), void* context) {
    aux_register(&s_auxB, callback, context);
}

void* (*__AXFXAlloc)(size_t) = NULL;
void (*__AXFXFree)(void*) = NULL;

void* AXFXAllocFunction(size_t size) {
    return __AXFXAlloc != NULL ? __AXFXAlloc(size) : calloc(1, size);
}

void AXFXFreeFunction(void* ptr) {
    if (__AXFXFree != NULL) {
        __AXFXFree(ptr);
    } else {
        free(ptr);
    }
}

void AXFXSetHooks(void* (*alloc_hook)(size_t), void (*free_hook)(void*)) {
    __AXFXAlloc = alloc_hook;
    __AXFXFree = free_hook;
}

#define AXFX_CHANNELS 2

static const long kCombLen[AXFX_CHANNELS][3] = {
    {1789, 1999, 2333},
    {1847, 2063, 2399},
};
static const long kAllPassLen[AXFX_CHANNELS][3] = {
    {433, 149, 53},
    {449, 157, 59},
};

static bool axfx_line_alloc(struct AXFX_REVHI_DELAYLINE* d, long length) {
    d->inputs = calloc((size_t)length, sizeof(float));
    if (d->inputs == NULL) {
        return false;
    }
    d->length = length;
    d->inPoint = 0;
    d->outPoint = 0;
    d->lastOutput = 0.0f;
    return true;
}

static void axfx_line_free(struct AXFX_REVHI_DELAYLINE* d) {
    if (d->inputs != NULL) {
        free(d->inputs);
        d->inputs = NULL;
    }
    d->length = 0;
}

static void axfx_reverb_shutdown(struct AXFX_REVHI_WORK* rv) {
    int i;
    for (i = 0; i < 9; i++) {
        axfx_line_free(&rv->C[i]);
        axfx_line_free(&rv->AP[i]);
    }
}

static int axfx_reverb_init(struct AXFX_REVHI_WORK* rv, float coloration, float mix, float time,
    float damping, float crosstalk) {
    int ch, k;

    memset(rv, 0, sizeof(*rv));
    for (ch = 0; ch < AXFX_CHANNELS; ch++) {
        for (k = 0; k < 3; k++) {
            if (!axfx_line_alloc(&rv->C[ch * 3 + k], kCombLen[ch][k]) ||
                !axfx_line_alloc(&rv->AP[ch * 3 + k], kAllPassLen[ch][k]))
            {
                axfx_reverb_shutdown(rv);
                return 0;
            }
        }
    }
    if (time < 0.01f) {
        time = 0.01f;
    }
    for (ch = 0; ch < AXFX_CHANNELS; ch++) {
        for (k = 0; k < 3; k++) {
            rv->combCoef[ch * 3 + k] =
                powf(10.0f, -3.0f * (float)kCombLen[ch][k] / (time * AX_RATE));
        }
    }
    rv->allPassCoeff = coloration;
    rv->level = mix;
    rv->damping = damping;
    rv->crosstalk = crosstalk;
    return 1;
}

static void axfx_reverb_run(struct AXFX_REVHI_WORK* rv, struct AXFX_BUFFERUPDATE* b) {
    long* chan[AXFX_CHANNELS] = {b->left, b->right};
    float in[AXFX_CHANNELS][AX_FRAME];
    int ch, k, i;
    float damp = rv->damping;
    float ap = rv->allPassCoeff;
    float wet = rv->level;

    if (rv->C[0].inputs == NULL) {
        return;
    }
    if (damp < 0.0f) {
        damp = 0.0f;
    } else if (damp > 0.95f) {
        damp = 0.95f;
    }

    for (i = 0; i < AX_FRAME; i++) {
        in[0][i] = (float)chan[0][i];
        in[1][i] = (float)chan[1][i];
        if (rv->crosstalk > 0.0f) {
            float c = rv->crosstalk;
            float l = in[0][i] + c * in[1][i];
            float r = in[1][i] + c * in[0][i];
            in[0][i] = l;
            in[1][i] = r;
        }
    }
    for (ch = 0; ch < AXFX_CHANNELS; ch++) {
        struct AXFX_REVHI_DELAYLINE* line[6];
        float* buf[6];
        int32_t pos[6], len[6];
        float lp = rv->lpLastout[ch];

        for (k = 0; k < 3; k++) {
            line[k] = &rv->C[ch * 3 + k];
            line[3 + k] = &rv->AP[ch * 3 + k];
        }
        for (k = 0; k < 6; k++) {
            buf[k] = line[k]->inputs;
            pos[k] = line[k]->outPoint;
            len[k] = line[k]->length;
        }
        for (i = 0; i < AX_FRAME; i++) {
            float acc = 0.0f;
            float y;

            for (k = 0; k < 3; k++) {
                float out = buf[k][pos[k]];
                lp = out * (1.0f - damp) + lp * damp;
                buf[k][pos[k]] = in[ch][i] + lp * rv->combCoef[ch * 3 + k];
                if (++pos[k] >= len[k]) {
                    pos[k] = 0;
                }
                acc += out;
            }
            y = acc * (1.0f / 3.0f);
            for (k = 3; k < 6; k++) {
                float out = buf[k][pos[k]];
                float v = y + ap * out;
                buf[k][pos[k]] = v;
                if (++pos[k] >= len[k]) {
                    pos[k] = 0;
                }
                y = out - ap * v;
            }
            chan[ch][i] = (long)(y * wet);
        }
        for (k = 0; k < 6; k++) {
            line[k]->inPoint = line[k]->outPoint = pos[k];
        }
        rv->lpLastout[ch] = lp;
    }
}

int AXFXReverbHiInit(struct AXFX_REVERBHI* rev) {
    if (rev == NULL) {
        return 0;
    }
    audio_lock();
    int res = axfx_reverb_init(
        &rev->rv, rev->coloration, rev->mix, rev->time, rev->damping, rev->crosstalk);
    audio_unlock();
    return res;
}

int AXFXReverbHiShutdown(struct AXFX_REVERBHI* rev) {
    if (rev == NULL) {
        return 1;
    }
    audio_lock();
    axfx_reverb_shutdown(&rev->rv);
    audio_unlock();
    return 1;
}

int AXFXReverbHiSettings(struct AXFX_REVERBHI* rev) {
    AXFXReverbHiShutdown(rev);
    return AXFXReverbHiInit(rev);
}

void AXFXReverbHiCallback(struct AXFX_BUFFERUPDATE* b, struct AXFX_REVERBHI* r) {
    if (b == NULL || r == NULL) {
        return;
    }
    if (!r->tempDisableFX) {
        axfx_reverb_run(&r->rv, b);
    }
}

static struct AXFX_REVHI_WORK s_revstd_work[2];
static struct AXFX_REVERBSTD* s_revstd_owner[2];

static struct AXFX_REVHI_WORK* revstd_work(struct AXFX_REVERBSTD* rev, bool claim) {
    if (rev == NULL) {
        return NULL;
    }
    int i;
    for (i = 0; i < 2; i++) {
        if (s_revstd_owner[i] == rev) {
            return &s_revstd_work[i];
        }
    }
    if (!claim) {
        return NULL;
    }
    for (i = 0; i < 2; i++) {
        if (s_revstd_owner[i] == NULL) {
            s_revstd_owner[i] = rev;
            return &s_revstd_work[i];
        }
    }
    return NULL;
}

int AXFXReverbStdInit(struct AXFX_REVERBSTD* rev) {
    if (rev == NULL) {
        return 0;
    }
    audio_lock();
    struct AXFX_REVHI_WORK* w = revstd_work(rev, true);
    if (w == NULL) {
        audio_unlock();
        return 0;
    }
    int res = axfx_reverb_init(w, rev->coloration, rev->mix, rev->time, rev->damping, 0.0f);
    audio_unlock();
    return res;
}

int AXFXReverbStdShutdown(struct AXFX_REVERBSTD* rev) {
    if (rev == NULL) {
        return 1;
    }
    audio_lock();
    struct AXFX_REVHI_WORK* w = revstd_work(rev, false);
    int i;
    if (w != NULL) {
        axfx_reverb_shutdown(w);
        for (i = 0; i < 2; i++) {
            if (s_revstd_owner[i] == rev) {
                s_revstd_owner[i] = NULL;
            }
        }
    }
    audio_unlock();
    return 1;
}

int AXFXReverbStdSettings(struct AXFX_REVERBSTD* rev) {
    AXFXReverbStdShutdown(rev);
    return AXFXReverbStdInit(rev);
}

void AXFXReverbStdCallback(struct AXFX_BUFFERUPDATE* b, struct AXFX_REVERBSTD* r) {
    if (b == NULL || r == NULL) {
        return;
    }
    struct AXFX_REVHI_WORK* w = revstd_work(r, false);
    if (w != NULL && !r->tempDisableFX) {
        axfx_reverb_run(w, b);
    }
}

int AXFXChorusInit(struct AXFX_CHORUS* c) {
    (void)c;
    return 1;
}
int AXFXChorusShutdown(struct AXFX_CHORUS* c) {
    (void)c;
    return 1;
}
int AXFXChorusSettings(struct AXFX_CHORUS* c) {
    (void)c;
    return 1;
}
void AXFXChorusCallback(struct AXFX_BUFFERUPDATE* b, struct AXFX_CHORUS* c) {
    (void)b;
    (void)c;
}

int AXFXDelayShutdown(struct AXFX_DELAY* d) {
    if (d == NULL) {
        return 1;
    }
    audio_lock();
    long** lines[3] = {&d->left, &d->right, &d->sur};
    int i;
    for (i = 0; i < 3; i++) {
        free(*lines[i]);
        *lines[i] = NULL;
        d->currentSize[i] = 0;
        d->currentPos[i] = 0;
    }
    audio_unlock();
    return 1;
}

int AXFXDelayInit(struct AXFX_DELAY* d) {
    if (d == NULL) {
        return 0;
    }
    audio_lock();
    long** lines[3] = {&d->left, &d->right, &d->sur};
    int i;

    for (i = 0; i < 3; i++) {
        *lines[i] = NULL;
    }
    for (i = 0; i < AXFX_CHANNELS; i++) {
        u32 n = d->delay[i] * (AX_RATE / 1000u);
        if (n == 0) {
            n = 1;
        }
        *lines[i] = calloc(n, sizeof(long));
        if (*lines[i] == NULL) {
            audio_unlock();
            AXFXDelayShutdown(d);
            return 0;
        }
        d->currentSize[i] = n;
        d->currentPos[i] = 0;
        d->currentFeedback[i] = d->feedback[i];
        d->currentOutput[i] = d->output[i];
    }
    audio_unlock();
    return 1;
}

int AXFXDelaySettings(struct AXFX_DELAY* d) {
    AXFXDelayShutdown(d);
    return AXFXDelayInit(d);
}

void AXFXDelayCallback(struct AXFX_BUFFERUPDATE* b, struct AXFX_DELAY* d) {
    if (b == NULL || d == NULL) {
        return;
    }
    long* chan[AXFX_CHANNELS] = {b->left, b->right};
    long* line[AXFX_CHANNELS] = {d->left, d->right};
    int c, i;

    for (c = 0; c < AXFX_CHANNELS; c++) {
        u32 size = d->currentSize[c];
        u32 pos = d->currentPos[c];
        long fb = (long)d->currentFeedback[c];  // NOLINT
        long og = (long)d->currentOutput[c];    // NOLINT

        if (line[c] == NULL || size == 0) {
            continue;
        }
        for (i = 0; i < AX_FRAME; i++) {
            long tap = line[c][pos];
            line[c][pos] = chan[c][i] + tap * fb / 100;
            if (++pos >= size) {
                pos = 0;
            }
            chan[c][i] = tap * og / 100;
        }
        d->currentPos[c] = pos;
    }
}

static int32_t axfx_longest_path(void (*cb)(void*, void*), void* ctx) {
    int32_t longest = 0;
    int c, k;

    for (c = 0; c < AXFX_CHANNELS; c++) {
        int32_t n = kCombLen[c][2];
        if ((void*)cb == (void*)AXFXDelayCallback) {
            n = (int32_t)((struct AXFX_DELAY*)ctx)->currentSize[c];
        } else {
            for (k = 0; k < 3; k++) {
                n += kAllPassLen[c][k];
            }
        }
        longest = n > longest ? n : longest;
    }
    return longest;
}

void AIInit(u8* stack) {
    (void)stack;
}
void AISetDSPSampleRate(u32 rate) {
    (void)rate;
}
void AISetStreamVolLeft(u8 vol) {
    (void)vol;
}
void AISetStreamVolRight(u8 vol) {
    (void)vol;
}
