/*
 * miles.c - mss32.dll (Miles Sound System 6.5b) as Red Faction II imports it:
 * 2D samples and file streams (the game does its own positional volume and
 * pan; it imports no AIL_3D_* call). Startup, memory and RIB helpers are
 * win32hle/mss.c's.
 *
 * Mixing: a host thread mixes every playing sample and stream into one
 * 44.1 kHz stereo s16 stream of win32hle's mixer (hle_audio_stream_*), keeping
 * ~70 ms queued. Guest code never runs on that thread: a stream's end
 * callback is delivered from the next Miles call the game makes.
 *
 * Samples: AIL_set_sample_file gets a WAV file image in guest memory (RF2's
 * are mono IMA ADPCM at 11-44 kHz; PCM 8/16 also handled). It is decoded once
 * to s16 and cached by (address, size, content hash).
 * Streams: AIL_open_stream gets a file name (the MP3 music); FFmpeg decodes it
 * on the mixer thread as it plays.
 *
 * Miles 6.5 semantics used: volume 0..1 and pan 0..1 (0.5 centre) as F32;
 * status SMP_FREE 1, SMP_DONE 2, SMP_PLAYING 4, SMP_STOPPED 8; loop count 0 =
 * forever; callbacks are stdcall (AILCALLBACK).
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win32hle.h"
#ifdef HLE_WITH_FFMPEG
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#endif

enum { SMP_FREE = 1, SMP_DONE = 2, SMP_PLAYING = 4, SMP_STOPPED = 8 };
#define OUT_RATE 44100
#define MAX_SAMPLES 128
#define MAX_STREAMS 8

static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* ---------------------------------------------------------------- decoded sounds */
typedef struct pcm { uint32_t key_va, key_len, hash; int16_t *data; uint32_t frames; int channels, rate; struct pcm *next; } pcm;
static pcm *g_cache[1024];

static const int k_ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int k_ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
    1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static int16_t ima_nibble(int *pred, int *idx, int n) {
    int step = k_ima_step[*idx], diff = step >> 3;
    if (n & 4) diff += step;
    if (n & 2) diff += step >> 1;
    if (n & 1) diff += step >> 2;
    *pred += (n & 8) ? -diff : diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += k_ima_index[n];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
    return (int16_t)*pred;
}
/* Microsoft IMA ADPCM (WAVE_FORMAT_IMA_ADPCM 0x11): per block and channel a
 * 4-byte header (s16 sample, u8 index, pad), then 4-byte groups per channel. */
static int16_t *ima_decode(const uint8_t *d, uint32_t n, int ch, int block, uint32_t *frames_out) {
    if (block <= 4 * ch) return NULL;
    uint32_t per_block = (uint32_t)((block - 4 * ch) * 8 / (4 * ch) + 1);
    uint32_t blocks = (n + (uint32_t)block - 1) / (uint32_t)block;
    int16_t *out = malloc((size_t)blocks * per_block * (size_t)ch * 2 + 16);
    uint32_t f = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        const uint8_t *p = d + (size_t)b * (uint32_t)block;
        uint32_t len = n - b * (uint32_t)block < (uint32_t)block ? n - b * (uint32_t)block : (uint32_t)block;
        if (len < 4u * (uint32_t)ch) break;
        int pred[2], idx[2];
        for (int c = 0; c < ch; c++) {
            pred[c] = (int16_t)(p[4 * c] | p[4 * c + 1] << 8); idx[c] = p[4 * c + 2] > 88 ? 88 : p[4 * c + 2];
            out[(size_t)f * (size_t)ch + (size_t)c] = (int16_t)pred[c];
        }
        f++;
        const uint8_t *q = p + 4 * ch, *e = p + len;
        while (q + 4 * ch <= e) {               /* 8 frames per channel group */
            int16_t tmp[2][8];
            for (int c = 0; c < ch; c++)
                for (int i = 0; i < 4; i++) {
                    uint8_t byte = q[c * 4 + i];
                    tmp[c][2 * i] = ima_nibble(&pred[c], &idx[c], byte & 15);
                    tmp[c][2 * i + 1] = ima_nibble(&pred[c], &idx[c], byte >> 4);
                }
            for (int i = 0; i < 8; i++, f++) for (int c = 0; c < ch; c++) out[(size_t)f * (size_t)ch + (size_t)c] = tmp[c][i];
            q += 4 * ch;
        }
    }
    *frames_out = f;
    return out;
}
static pcm *decode_wav(uint32_t va, uint32_t len) {
    const uint8_t *d = hle_ptr(va);
    if (!d || len < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return NULL;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < len && i < 256; i++) hash = (hash ^ d[i]) * 16777619u;
    for (uint32_t i = len > 256 ? len - 64 : 0; i < len; i++) hash = (hash ^ d[i]) * 16777619u;
    uint32_t slot = (va ^ hash) & 1023;
    for (pcm *c = g_cache[slot]; c; c = c->next) if (c->key_va == va && c->key_len == len && c->hash == hash) return c;
    int fmt = 0, ch = 0, rate = 0, bits = 0, block = 0;
    const uint8_t *data = NULL; uint32_t dlen = 0;
    for (uint32_t o = 12; o + 8 <= len; ) {
        uint32_t cl; memcpy(&cl, d + o + 4, 4);
        if (!memcmp(d + o, "fmt ", 4) && cl >= 16) {
            fmt = d[o + 8] | d[o + 9] << 8; ch = d[o + 10] | d[o + 11] << 8; memcpy(&rate, d + o + 12, 4);
            block = d[o + 20] | d[o + 21] << 8; bits = d[o + 22] | d[o + 23] << 8;
        } else if (!memcmp(d + o, "data", 4)) { data = d + o + 8; dlen = cl > len - o - 8 ? len - o - 8 : cl; }
        if (cl > len) break;
        o += 8 + cl + (cl & 1);
    }
    if (!data || ch < 1 || ch > 2 || rate <= 0) return NULL;
    uint32_t frames = 0; int16_t *out = NULL;
    if (fmt == 0x11) out = ima_decode(data, dlen, ch, block, &frames);
    else if (fmt == 1 && bits == 16) { frames = dlen / (2u * (uint32_t)ch); out = malloc((size_t)frames * (size_t)ch * 2 + 2); memcpy(out, data, (size_t)frames * (size_t)ch * 2); }
    else if (fmt == 1 && bits == 8) { frames = dlen / (uint32_t)ch; out = malloc((size_t)frames * (size_t)ch * 2 + 2); for (uint32_t i = 0; i < frames * (uint32_t)ch; i++) out[i] = (int16_t)((data[i] - 128) << 8); }
    if (!out || !frames) { static int once; if (!once++) fprintf(stderr, "[miles] unsupported WAV format 0x%X (%d bit, %d ch)\n", fmt, bits, ch); free(out); return NULL; }
    pcm *c = calloc(1, sizeof *c);
    c->key_va = va; c->key_len = len; c->hash = hash; c->data = out; c->frames = frames; c->channels = ch; c->rate = rate;
    c->next = g_cache[slot]; g_cache[slot] = c;
    return c;
}

/* ---------------------------------------------------------------- voices */
typedef struct {
    uint32_t handle;                 /* guest HSAMPLE */
    int status;
    pcm *snd;
    double pos, step;                /* frames; step = rate / OUT_RATE */
    int rate;
    uint32_t loops, loops_done;      /* 0 = forever */
    float vol, pan;
} voice;
typedef struct {
    uint32_t handle;                 /* guest HSTREAM */
    int status, paused;
    float vol, pan;
    uint32_t callback, cb_pending;
#ifdef HLE_WITH_FFMPEG
    AVFormatContext *fmt; AVCodecContext *dec; SwrContext *swr; int stream_index;
    AVPacket *pkt; AVFrame *frame;
#endif
    int16_t *buf; uint32_t buf_frames, buf_pos, buf_cap;
    uint64_t played_frames;
    int eof;
} mstream;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static voice g_voice[MAX_SAMPLES];
static mstream g_str[MAX_STREAMS];
static int g_out = -1, g_released, g_thread_on;
static uint32_t g_driver;

static voice *voice_of(uint32_t h) { for (int i = 0; i < MAX_SAMPLES; i++) if (h && g_voice[i].handle == h) return &g_voice[i]; return NULL; }
static mstream *stream_of(uint32_t h) { for (int i = 0; i < MAX_STREAMS; i++) if (h && g_str[i].handle == h) return &g_str[i]; return NULL; }

static void gains(float vol, float pan, float *l, float *r) {
    if (!(vol >= 0)) vol = 0;
    if (vol > 1) vol = 1;
    if (!(pan >= 0)) pan = 0;
    if (pan > 1) pan = 1;
    *l = vol * (pan <= 0.5f ? 1.f : 2.f * (1.f - pan));
    *r = vol * (pan >= 0.5f ? 1.f : 2.f * pan);
}

#ifdef HLE_WITH_FFMPEG
/* Decode the next chunk of a stream into s->buf (44.1 kHz stereo s16). 0 at end. */
static int stream_fill(mstream *s) {
    s->buf_frames = s->buf_pos = 0;
    while (!s->eof) {
        int r = avcodec_receive_frame(s->dec, s->frame);
        if (r == 0) {
            int max = swr_get_out_samples(s->swr, s->frame->nb_samples);
            if (max > 0 && (uint32_t)max > s->buf_cap) { s->buf_cap = (uint32_t)max + 1024; s->buf = realloc(s->buf, (size_t)s->buf_cap * 4); }
            uint8_t *o[1] = { (uint8_t *)s->buf };
            int n = swr_convert(s->swr, o, max, (const uint8_t **)s->frame->extended_data, s->frame->nb_samples);
            av_frame_unref(s->frame);
            if (n > 0) { s->buf_frames = (uint32_t)n; return 1; }
            continue;
        }
        if (r == AVERROR_EOF) { s->eof = 1; break; }
        if (av_read_frame(s->fmt, s->pkt) < 0) { avcodec_send_packet(s->dec, NULL); continue; }
        if (s->pkt->stream_index == s->stream_index) avcodec_send_packet(s->dec, s->pkt);
        av_packet_unref(s->pkt);
    }
    return 0;
}
static void stream_free(mstream *s) {
    if (s->dec) avcodec_free_context(&s->dec);
    if (s->fmt) avformat_close_input(&s->fmt);
    if (s->swr) swr_free(&s->swr);
    if (s->pkt) av_packet_free(&s->pkt);
    if (s->frame) av_frame_free(&s->frame);
    free(s->buf); s->buf = NULL; s->buf_cap = 0;
}
static int stream_init(mstream *s, const char *host) {
    if (avformat_open_input(&s->fmt, host, NULL, NULL) < 0) return 0;
    if (avformat_find_stream_info(s->fmt, NULL) < 0) return 0;
    int si = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (si < 0) return 0;
    const AVCodec *c = avcodec_find_decoder(s->fmt->streams[si]->codecpar->codec_id);
    if (!c) return 0;
    s->dec = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(s->dec, s->fmt->streams[si]->codecpar);
    if (avcodec_open2(s->dec, c, NULL) < 0) return 0;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&s->swr, &stereo, AV_SAMPLE_FMT_S16, OUT_RATE, &s->dec->ch_layout, s->dec->sample_fmt, s->dec->sample_rate, 0, NULL) < 0 || swr_init(s->swr) < 0) return 0;
    s->stream_index = si; s->pkt = av_packet_alloc(); s->frame = av_frame_alloc();
    return 1;
}
#endif

/* ---------------------------------------------------------------- mixer thread */
static void mix_chunk(int16_t *out, int frames) {
    static float acc[2 * 2048];
    memset(acc, 0, sizeof(float) * 2 * (size_t)frames);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_SAMPLES; i++) {
        voice *v = &g_voice[i];
        if (!v->handle || v->status != SMP_PLAYING || !v->snd) continue;
        float gl, gr; gains(v->vol, v->pan, &gl, &gr);
        const pcm *p = v->snd;
        for (int f = 0; f < frames; f++) {
            if (v->pos >= p->frames) {
                v->loops_done++;
                if (v->loops && v->loops_done >= v->loops) { v->status = SMP_DONE; break; }
                v->pos -= p->frames;
            }
            uint32_t a = (uint32_t)v->pos, b = a + 1 < p->frames ? a + 1 : a;
            float t = (float)(v->pos - a);
            float l, r;
            if (p->channels == 2) { l = p->data[2 * a] + (p->data[2 * b] - p->data[2 * a]) * t; r = p->data[2 * a + 1] + (p->data[2 * b + 1] - p->data[2 * a + 1]) * t; }
            else l = r = p->data[a] + (p->data[b] - p->data[a]) * t;
            acc[2 * f] += l * gl; acc[2 * f + 1] += r * gr;
            v->pos += v->step;
        }
    }
#ifdef HLE_WITH_FFMPEG
    for (int i = 0; i < MAX_STREAMS; i++) {
        mstream *s = &g_str[i];
        if (!s->handle || s->status != SMP_PLAYING || s->paused || !s->dec) continue;
        float gl, gr; gains(s->vol, s->pan, &gl, &gr);
        for (int f = 0; f < frames; f++) {
            if (s->buf_pos >= s->buf_frames && !stream_fill(s)) { s->status = SMP_DONE; if (s->callback) s->cb_pending = 1; break; }
            acc[2 * f] += s->buf[2 * s->buf_pos] * gl; acc[2 * f + 1] += s->buf[2 * s->buf_pos + 1] * gr;
            s->buf_pos++; s->played_frames++;
        }
    }
#endif
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < 2 * frames; i++) { float x = acc[i]; out[i] = (int16_t)(x > 32767.f ? 32767 : x < -32768.f ? -32768 : x); }
}
static void *mixer(void *arg) {
    (void)arg;
    static int16_t chunk[2 * 1024];
    for (;;) {
        while (hle_audio_stream_queued(g_out) < 3072) {
            if (g_released) memset(chunk, 0, sizeof chunk); else mix_chunk(chunk, 1024);
            hle_audio_stream_push(g_out, chunk, 1024);
        }
        usleep(8000);
    }
    return NULL;
}

/* Deliver stream-end callbacks on the game's thread (from any Miles call). */
static void deliver_callbacks(void) {
    for (int i = 0; i < MAX_STREAMS; i++) {
        mstream *s = &g_str[i];
        if (s->handle && s->cb_pending && s->callback) { s->cb_pending = 0; uint32_t a = s->handle; hle_call_guest(s->callback, 1, &a); }
    }
}

/* ---------------------------------------------------------------- driver */
static void m_open_digital_driver(void) {          /* (freq, bits, channels, flags) -> HDIGDRIVER */
    if (!g_driver) {
        g_out = hle_audio_stream_open(OUT_RATE, 2);
        if (g_out < 0) { fprintf(stderr, "[miles] no audio output: running silent\n"); RET(0, 4); }
        g_driver = hle_va(hle_alloc(256));
        pthread_t t;
        if (!g_thread_on && pthread_create(&t, NULL, mixer, NULL) == 0) { pthread_detach(t); g_thread_on = 1; }
        fprintf(stderr, "[miles] digital driver (game asked %u Hz %u bit %u ch): mixing at %d Hz stereo\n", A32(0), A32(1), A32(2), OUT_RATE);
    }
    RET(g_driver, 4);
}
static void m_close_digital_driver(void) { RET(0, 1); }
static void m_digital_handle_release(void)  { g_released = 1; RET(1, 1); }
static void m_digital_handle_reacquire(void){ g_released = 0; RET(1, 1); }

/* ---------------------------------------------------------------- samples */
static void m_allocate_sample_handle(void) {
    deliver_callbacks();
    static uint32_t pool[MAX_SAMPLES];
    pthread_mutex_lock(&g_lock);
    voice *v = NULL;
    for (int i = 0; i < MAX_SAMPLES && !v; i++) if (!g_voice[i].handle) v = &g_voice[i];
    int idx = v ? (int)(v - g_voice) : -1;
    pthread_mutex_unlock(&g_lock);
    if (!v) { static int once; if (!once++) fprintf(stderr, "[miles] out of sample handles (%d)\n", MAX_SAMPLES); RET(0, 1); }
    if (!pool[idx]) pool[idx] = hle_va(hle_alloc(64));
    pthread_mutex_lock(&g_lock);
    memset(v, 0, sizeof *v);
    v->handle = pool[idx]; v->status = SMP_DONE; v->vol = 1.f; v->pan = 0.5f; v->loops = 1; v->step = 1.0;
    pthread_mutex_unlock(&g_lock);
    RET(v->handle, 1);
}
static void m_release_sample_handle(void) {
    pthread_mutex_lock(&g_lock);
    voice *v = voice_of(A32(0));
    if (v) { v->handle = 0; v->status = SMP_FREE; v->snd = NULL; }
    pthread_mutex_unlock(&g_lock);
    RET(0, 1);
}
static void m_init_sample(void) {
    pthread_mutex_lock(&g_lock);
    voice *v = voice_of(A32(0));
    if (v) { v->status = SMP_DONE; v->snd = NULL; v->pos = 0; v->vol = 1.f; v->pan = 0.5f; v->loops = 1; v->loops_done = 0; v->step = 1.0; v->rate = 0; }
    pthread_mutex_unlock(&g_lock);
    RET(0, 1);
}
static void m_set_sample_file(void) {              /* (S, file_image, block) -> 1 ok */
    uint32_t h = A32(0), img = A32(1), len = 0;
    if (img) { const uint8_t *d = hle_ptr(img); if (!memcmp(d, "RIFF", 4)) { memcpy(&len, d + 4, 4); len += 8; } }
    pcm *p = img && len ? decode_wav(img, len) : NULL;
    if (img && !p) { static int n; if (n++ < 5) { const uint8_t *d = hle_ptr(img); fprintf(stderr, "[miles] set_sample_file: not a decodable WAV (%02X %02X %02X %02X)\n", d[0], d[1], d[2], d[3]); } }
    pthread_mutex_lock(&g_lock);
    voice *v = voice_of(h);
    if (v) { v->snd = p; v->pos = 0; v->loops_done = 0; if (p) { v->rate = p->rate; v->step = (double)p->rate / OUT_RATE; } v->status = SMP_DONE; }
    pthread_mutex_unlock(&g_lock);
    RET(p ? 1 : 0, 3);
}
static void m_set_sample_playback_rate(void) {
    pthread_mutex_lock(&g_lock);
    voice *v = voice_of(A32(0));
    if (v && (int32_t)A32(1) > 0) { v->rate = (int)A32(1); v->step = (double)v->rate / OUT_RATE; }
    pthread_mutex_unlock(&g_lock);
    RET(0, 2);
}
static void m_set_sample_volume_pan(void) {
    pthread_mutex_lock(&g_lock);
    voice *v = voice_of(A32(0));
    if (v) { v->vol = f32(A32(1)); v->pan = f32(A32(2)); }
    { static int n; if (n++ < 6) fprintf(stderr, "[miles] sample volume %.3f pan %.3f\n", f32(A32(1)), f32(A32(2))); }
    pthread_mutex_unlock(&g_lock);
    RET(0, 3);
}
static void m_set_sample_loop_count(void) {
    pthread_mutex_lock(&g_lock);
    voice *v = voice_of(A32(0));
    if (v) v->loops = A32(1);
    pthread_mutex_unlock(&g_lock);
    RET(0, 2);
}
static void m_start_sample(void) {
    deliver_callbacks();
    pthread_mutex_lock(&g_lock);
    voice *v = voice_of(A32(0));
    if (v && v->snd) { v->pos = 0; v->loops_done = 0; v->status = SMP_PLAYING; }
    { static int n; if (v && v->snd && n++ < 3) fprintf(stderr, "[miles] play sample: %u frames %d Hz x%d, vol %.2f pan %.2f loops %u\n", v->snd->frames, v->rate, v->snd->channels, v->vol, v->pan, v->loops); }
    pthread_mutex_unlock(&g_lock);
    RET(0, 1);
}
static void m_stop_sample(void) {
    pthread_mutex_lock(&g_lock);
    voice *v = voice_of(A32(0));
    if (v && v->status == SMP_PLAYING) v->status = SMP_STOPPED;
    pthread_mutex_unlock(&g_lock);
    RET(0, 1);
}
static void m_resume_sample(void) {
    pthread_mutex_lock(&g_lock);
    voice *v = voice_of(A32(0));
    if (v && v->snd && (v->status == SMP_STOPPED || v->status == SMP_DONE)) v->status = SMP_PLAYING;
    pthread_mutex_unlock(&g_lock);
    RET(0, 1);
}
static void m_end_sample(void) {
    pthread_mutex_lock(&g_lock);
    voice *v = voice_of(A32(0));
    if (v) v->status = SMP_DONE;
    pthread_mutex_unlock(&g_lock);
    RET(0, 1);
}
static void m_sample_status(void) {
    pthread_mutex_lock(&g_lock);
    voice *v = voice_of(A32(0));
    int st = v ? v->status : SMP_FREE;
    pthread_mutex_unlock(&g_lock);
    RET((uint32_t)st, 1);
}

/* ---------------------------------------------------------------- streams */
static void m_open_stream(void) {                  /* (dig, filename, stream_mem) -> HSTREAM */
    deliver_callbacks();
    const char *name = A32(1) ? (const char *)APTR(1) : NULL;
    char host[1024];
    if (!g_driver || !name || !hle_host_path(name, host, sizeof host)) {
        static int n; if (n++ < 5) fprintf(stderr, "[miles] open_stream \"%s\": not found\n", name ? name : "(null)");
        RET(0, 3);
    }
#ifdef HLE_WITH_FFMPEG
    static uint32_t pool[MAX_STREAMS];
    pthread_mutex_lock(&g_lock);
    int idx = -1;
    for (int i = 0; i < MAX_STREAMS && idx < 0; i++) if (!g_str[i].handle) idx = i;
    pthread_mutex_unlock(&g_lock);
    if (idx < 0) RET(0, 3);
    mstream tmp; memset(&tmp, 0, sizeof tmp);
    if (!stream_init(&tmp, host)) {
        stream_free(&tmp);
        static int n; if (n++ < 5) fprintf(stderr, "[miles] open_stream \"%s\": cannot decode\n", name);
        RET(0, 3);
    }
    if (!pool[idx]) pool[idx] = hle_va(hle_alloc(64));
    tmp.handle = pool[idx]; tmp.status = SMP_STOPPED; tmp.vol = 1.f; tmp.pan = 0.5f;
    fprintf(stderr, "[miles] stream \"%s\" (%d Hz)\n", name, tmp.dec->sample_rate);
    pthread_mutex_lock(&g_lock);
    g_str[idx] = tmp;
    pthread_mutex_unlock(&g_lock);
    RET(tmp.handle, 3);
#else
    RET(0, 3);
#endif
}
static void m_close_stream(void) {
    pthread_mutex_lock(&g_lock);
    mstream *s = stream_of(A32(0));
#ifdef HLE_WITH_FFMPEG
    if (s) { stream_free(s); memset(s, 0, sizeof *s); }
#endif
    pthread_mutex_unlock(&g_lock);
    RET(0, 1);
}
static void m_start_stream(void) {
    pthread_mutex_lock(&g_lock);
    mstream *s = stream_of(A32(0));
    if (s) { s->status = SMP_PLAYING; s->paused = 0; }
    pthread_mutex_unlock(&g_lock);
    RET(0, 1);
}
static void m_pause_stream(void) {                 /* (S, onoff) */
    pthread_mutex_lock(&g_lock);
    mstream *s = stream_of(A32(0));
    if (s) s->paused = A32(1) != 0;
    pthread_mutex_unlock(&g_lock);
    RET(0, 2);
}
static void m_stream_status(void) {
    deliver_callbacks();
    pthread_mutex_lock(&g_lock);
    mstream *s = stream_of(A32(0));
    int st = !s ? SMP_FREE : s->status == SMP_PLAYING && s->paused ? SMP_STOPPED : s->status;
    pthread_mutex_unlock(&g_lock);
    RET((uint32_t)st, 1);
}
static void m_stream_position(void) {              /* bytes of 16-bit stereo output, as a position measure */
    pthread_mutex_lock(&g_lock);
    mstream *s = stream_of(A32(0));
    uint32_t p = s ? (uint32_t)(s->played_frames * 4) : 0;
    pthread_mutex_unlock(&g_lock);
    RET(p, 1);
}
static void m_set_stream_position(void) {          /* (S, offset): only a restart is supported */
    pthread_mutex_lock(&g_lock);
    mstream *s = stream_of(A32(0));
#ifdef HLE_WITH_FFMPEG
    if (s && s->fmt) {
        av_seek_frame(s->fmt, s->stream_index, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(s->dec);
        s->eof = 0; s->buf_frames = s->buf_pos = 0; s->played_frames = 0;
        if (A32(1)) { static int once; if (!once++) fprintf(stderr, "[miles] set_stream_position(%u): restarting instead\n", A32(1)); }
    }
#endif
    pthread_mutex_unlock(&g_lock);
    RET(0, 2);
}
static void m_stream_volume_pan(void) {            /* (S, F32 *vol, F32 *pan) */
    pthread_mutex_lock(&g_lock);
    mstream *s = stream_of(A32(0));
    float v = s ? s->vol : 0.f, p = s ? s->pan : 0.5f;
    pthread_mutex_unlock(&g_lock);
    if (A32(1)) memcpy(APTR(1), &v, 4);
    if (A32(2)) memcpy(APTR(2), &p, 4);
    RET(0, 3);
}
static void m_set_stream_volume_pan(void) {
    pthread_mutex_lock(&g_lock);
    mstream *s = stream_of(A32(0));
    if (s) { s->vol = f32(A32(1)); s->pan = f32(A32(2)); }
    { static int n; if (n++ < 6) fprintf(stderr, "[miles] stream volume %.3f pan %.3f\n", f32(A32(1)), f32(A32(2))); }
    pthread_mutex_unlock(&g_lock);
    RET(0, 3);
}
static void m_register_stream_callback(void) {     /* (S, cb) -> previous */
    pthread_mutex_lock(&g_lock);
    mstream *s = stream_of(A32(0));
    uint32_t prev = s ? s->callback : 0;
    if (s) s->callback = A32(1);
    pthread_mutex_unlock(&g_lock);
    RET(prev, 2);
}

static const win32hle_shim g_miles[] = {
    { "_AIL_open_digital_driver@16", m_open_digital_driver }, { "_AIL_close_digital_driver@4", m_close_digital_driver },
    { "_AIL_digital_handle_release@4", m_digital_handle_release }, { "_AIL_digital_handle_reacquire@4", m_digital_handle_reacquire },
    { "_AIL_allocate_sample_handle@4", m_allocate_sample_handle }, { "_AIL_release_sample_handle@4", m_release_sample_handle },
    { "_AIL_init_sample@4", m_init_sample }, { "_AIL_set_sample_file@12", m_set_sample_file },
    { "_AIL_set_sample_playback_rate@8", m_set_sample_playback_rate }, { "_AIL_set_sample_volume_pan@12", m_set_sample_volume_pan },
    { "_AIL_set_sample_loop_count@8", m_set_sample_loop_count }, { "_AIL_start_sample@4", m_start_sample },
    { "_AIL_stop_sample@4", m_stop_sample }, { "_AIL_resume_sample@4", m_resume_sample }, { "_AIL_end_sample@4", m_end_sample },
    { "_AIL_sample_status@4", m_sample_status },
    { "_AIL_open_stream@12", m_open_stream }, { "_AIL_close_stream@4", m_close_stream }, { "_AIL_start_stream@4", m_start_stream },
    { "_AIL_pause_stream@8", m_pause_stream }, { "_AIL_stream_status@4", m_stream_status },
    { "_AIL_stream_position@4", m_stream_position }, { "_AIL_set_stream_position@8", m_set_stream_position },
    { "_AIL_stream_volume_pan@12", m_stream_volume_pan }, { "_AIL_set_stream_volume_pan@12", m_set_stream_volume_pan },
    { "_AIL_register_stream_callback@8", m_register_stream_callback },
    { 0, 0 }
};

/* kbdhook.dll: int __cdecl DisableTaskKeys(int, int) -- nothing to disable here */
static void k_DisableTaskKeys(void) { g_eax = 1; g_esp += 4; }
/* binkw32 entries win32hle's Bink layer (FFmpeg) lacks */
static void b_OpenMiles(void) { RET(0, 1); }   /* sound provider: none (Bink plays to win32hle's mixer) */
/* BinkGetSummary(bink, BINKSUMMARY*): the game centres movies by its Width and
 * Height. BINKSUMMARY starts { Width, Height, TotalTime(ms), FileFrameRate,
 * FileFrameRateDiv, FrameRate, FrameRateDiv, TotalOpenTime, TotalFrames,
 * TotalPlayedFrames, ... }; the BINK handle starts { Width, Height, Frames,
 * FrameNum, LastFrameNum, FrameRate, FrameRateDiv } (win32hle/bink.c). */
static void b_GetSummary(void) {
    uint32_t bk = A32(0), sm = A32(1);
    if (sm) {
        memset(APTR(1), 0, 0x80);
        if (bk) {
            uint32_t w = MEM32(bk), h = MEM32(bk + 4), frames = MEM32(bk + 8), fr = MEM32(bk + 20), div = MEM32(bk + 24);
            MEM32(sm) = w; MEM32(sm + 4) = h;
            MEM32(sm + 8) = fr ? (uint32_t)((uint64_t)frames * 1000u * (div ? div : 1) / fr) : 0;
            MEM32(sm + 12) = fr; MEM32(sm + 16) = div; MEM32(sm + 20) = fr; MEM32(sm + 24) = div;
            MEM32(sm + 32) = frames; MEM32(sm + 36) = MEM32(bk + 12);
        }
    }
    RET(0, 2);
}
uint32_t rr_d3d8_surface_format(uint32_t obj);
static void b_DX8SurfaceType(void) {        /* (IDirect3DSurface8*) -> BINKSURFACE type */
    switch (rr_d3d8_surface_format(A32(0))) {
    case 21: RET(5, 1);                      /* A8R8G8B8 -> BINKSURFACE32A */
    case 22: RET(3, 1);                      /* X8R8G8B8 -> BINKSURFACE32 */
    case 23: RET(10, 1);                     /* R5G6B5   -> BINKSURFACE565 */
    case 24: case 25: RET(9, 1);             /* X1R5G5B5/A1R5G5B5 -> BINKSURFACE555 */
    default: RET(0xFFFFFFFFu, 1);
    }
}
static const win32hle_shim g_rf2_misc[] = {
    { "kbdhook.dll!?DisableTaskKeys@@YAHHH@Z", k_DisableTaskKeys },
    { "binkw32.dll!_BinkOpenMiles@4", b_OpenMiles },
    { "binkw32.dll!_BinkGetSummary@8", b_GetSummary },
    { "binkw32.dll!_BinkDX8SurfaceType@4", b_DX8SurfaceType },
    { 0, 0 }
};

void rr_register_miles(void) { win32hle_register(g_miles); win32hle_register(g_rf2_misc); }
