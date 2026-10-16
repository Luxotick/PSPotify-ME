#include "player.h"
#include "audiodecrypt.h"
#include "http.h"
#include "log.h"
#include "memory.h"
#include "me_vorbis.h"
#include "../common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <pspaudio.h>
#include <psppower.h>
#include <pspthreadman.h>
#include <pspkernel.h>
#include <me-safe-task/me-stask.h>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PULLDATA_API
#include "stb_vorbis.c"

/* Decrypted Spotify Ogg files start with a 0xa7-byte custom header page. */
#define SPOTIFY_OGG_HEADER 0xa7
#define BLOCK_FRAMES 1024
#define OUTPUT_SLOTS 4
#define FETCH_CHUNK 16384
/* Input handed to one ME decode; widened if a frame does not fit. */
#define ME_WINDOW_MIN (32u * 1024u)
#define ME_WINDOW_MAX (256u * 1024u)
#define ME_TIMEOUT_US 2000000u
/* Stream buffer sizes to try, largest first. A file that fits is held
 * whole; a longer one streams through the buffer, about 150 s of 160 kbps
 * audio ahead at 3 MiB. Whole-file buffering failed on long tracks (a
 * 12.5 MB file found no 12.5 MB block in a fragmented heap). */
static const size_t ring_sizes[] = { 3u << 20, 3u << 19, 3u << 18 };
#define FETCH_MIN_ROOM 4096
/* Data needed after a seek target before decoding resumes there. */
#define SEEK_LOOKAHEAD (64u * 1024u)

/* ---- equalizer ---- */

const char *const player_eq_names[PLAYER_EQ_PRESETS] = {
    "Off", "Bass boost", "Treble boost", "Vocal", "Rock", "Pop", "Loudness",
};
/* Band gains in dB at 60, 230, 910, 3600 and 14000 Hz. */
static const float eq_gains[PLAYER_EQ_PRESETS][ME_EQ_BANDS] = {
    { 0, 0, 0, 0, 0 },
    { 6, 3, 0, 0, 0 },
    { 0, 0, 0, 3, 6 },
    { -2, -1, 3, 3, 0 },
    { 4, 2, -1, 2, 4 },
    { -1, 2, 4, 2, -1 },
    { 5, 2, 0, 1, 4 },
};
static const float eq_freqs[ME_EQ_BANDS] = { 60, 230, 910, 3600, 14000 };

/* Each slot is played by audio DMA after OutputBlocking returns, so the next
 * block is written into a different slot (one shared buffer crackled). */
static int16_t output_ring[OUTPUT_SLOTS][BLOCK_FRAMES * 2] __attribute__((aligned(64)));
/* All stb_vorbis setup and decode memory; no malloc or alloca. */
static char vorbis_memory[384 * 1024] __attribute__((aligned(64)));

/* The file streams through buf as a ring of `ring` bytes: absolute file
 * offset p lives at buf[p % ring]. When the file does not fit, ME_WINDOW_MAX
 * extra bytes after the ring mirror its start, so any decode window of up
 * to ME_WINDOW_MAX bytes is contiguous wherever it begins. */
typedef struct {
    http_stream http;
    audio_decrypt decrypt;
    uint8_t *buf;
    size_t capacity;            /* file size */
    size_t ring;                /* == capacity when the whole file fits */
    int wraps;
    volatile size_t buffered;   /* decrypted bytes received (absolute) */
    volatile size_t consumed;   /* bytes the decoder is done with (absolute) */
    size_t valid_lo;            /* first byte downloaded since the last (re)start */
    volatile int open;          /* fetch thread still running */
    volatile int stop;
} fetcher;

static int fetch_thread(SceSize args, void *argp)
{
    (void)args;
    fetcher *f = *(fetcher **)argp;
    while (!f->stop && f->buffered < f->capacity) {
        const size_t at = f->buffered, off = at % f->ring;
        /* Never overwrite what the decoder has not consumed yet. */
        size_t room = f->consumed + f->ring - at;
        if (room < FETCH_MIN_ROOM && at + room < f->capacity) {
            sceKernelDelayThread(10000);
            continue;
        }
        size_t chunk = FETCH_CHUNK;
        if (chunk > f->capacity - at) chunk = f->capacity - at;
        if (chunk > f->ring - off) chunk = f->ring - off;
        if (chunk > room) chunk = room;
        uint8_t *dst = f->buf + off;
        int r = http_stream_read(&f->http, dst, (unsigned int)chunk);
        if (r <= 0) break;
        /* Publish only decrypted bytes. */
        audio_decrypt_run(&f->decrypt, dst, (size_t)r);
        if (f->wraps && off < ME_WINDOW_MAX) {
            size_t mirror = ME_WINDOW_MAX - off;
            if (mirror > (size_t)r) mirror = (size_t)r;
            memcpy(f->buf + f->ring + off, dst, mirror);
        }
        f->buffered += (size_t)r;
    }
    f->open = 0;
    return 0;
}

/* ---- audio output ---- */

typedef struct {
    unsigned int slot, fill;
    unsigned long long frames;
} ring_state;

/* Appends interleaved stereo frames, playing each full 1024-frame slot. */
static int ring_push(ring_state *ring, const int16_t *pcm, int frames, int volume)
{
    for (int i = 0; i < frames; ++i) {
        output_ring[ring->slot][ring->fill * 2] = pcm[i * 2];
        output_ring[ring->slot][ring->fill * 2 + 1] = pcm[i * 2 + 1];
        if (++ring->fill == BLOCK_FRAMES) {
            int ar = sceAudioSRCOutputBlocking(volume, output_ring[ring->slot]);
            if (ar < 0) {
                log_printf("PLAYER: audio output 0x%08X\n", (unsigned int)ar);
                return ar;
            }
            ring->slot = (ring->slot + 1) % OUTPUT_SLOTS;
            ring->fill = 0;
        }
    }
    ring->frames += (unsigned long long)frames;
    return 0;
}

/* Spotify's header holds ReplayGain-style values at offset 144: track gain
 * (dB), track peak, album gain, album peak, as little-endian floats. The
 * gain is applied through the output volume, never above full scale. */
#define NORMALIZATION_OFFSET 144

static float read_le_float(const uint8_t *p)
{
    union { uint32_t u; float f; } v;
    v.u = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return v.f;
}

static float normalization_factor(const uint8_t *header)
{
    const float gain_db = read_le_float(header + NORMALIZATION_OFFSET);
    const float peak = read_le_float(header + NORMALIZATION_OFFSET + 4);
    if (!(gain_db > -40.0f && gain_db < 40.0f)) return 1.0f;    /* also NaN */
    float factor = powf(10.0f, gain_db / 20.0f);
    if (peak > 0.0f && peak <= 2.0f && factor * peak > 1.0f) factor = 1.0f / peak;
    log_printf("PLAYER: normalization %.2f dB, peak %.3f -> x%.2f\n", (double)gain_db,
               (double)peak, (double)factor);
    return factor;
}

static int output_volume(const player_control *control, float norm)
{
    if (!control->normalize) return control->volume;
    float v = (float)control->volume * norm;
    return v > (float)PSP_AUDIO_VOLUME_MAX ? PSP_AUDIO_VOLUME_MAX : (int)v;
}

static void ring_flush(ring_state *ring, int volume)
{
    if (ring->fill == 0) return;
    memset(&output_ring[ring->slot][ring->fill * 2], 0,
           (BLOCK_FRAMES - ring->fill) * 2 * sizeof(int16_t));
    sceAudioSRCOutputBlocking(volume, output_ring[ring->slot]);
}

/* ---- Media Engine decode jobs ---- */

static MeVorbisJob me_job;
static Task me_task;
static int16_t me_pcm[2][ME_VORBIS_MAX_FRAMES * 2] __attribute__((aligned(64)));
static uint32_t me_sequence;
static uint32_t me_pending_flush;

/* Peaking filters (RBJ cookbook), computed on the SC; the ME only runs them.
 * Called between jobs, when the ME is idle. */
static void eq_configure(int preset, const volatile int *custom, unsigned int rate)
{
    float gains[ME_EQ_BANDS];
    int flat = 1;
    for (int b = 0; b < ME_EQ_BANDS; ++b) {
        gains[b] = preset == PLAYER_EQ_CUSTOM ? (float)custom[b]
                 : preset > 0 && preset < PLAYER_EQ_PRESETS ? eq_gains[preset][b] : 0.0f;
        if (gains[b] != 0.0f) flat = 0;
    }
    if (flat || rate == 0) {
        me_job.eq_on = 0;
        return;
    }
    float boost = 0;
    for (int b = 0; b < ME_EQ_BANDS; ++b) {
        const float gain = gains[b];
        if (gain > boost) boost = gain;
        const float a = powf(10.0f, gain / 40.0f);
        const float w0 = 2.0f * 3.14159265f * eq_freqs[b] / (float)rate;
        const float alpha = sinf(w0) / (2.0f * 1.1f);
        const float cw = cosf(w0);
        const float a0 = 1.0f + alpha / a;
        me_job.eq_coef[b][0] = (1.0f + alpha * a) / a0;
        me_job.eq_coef[b][1] = -2.0f * cw / a0;
        me_job.eq_coef[b][2] = (1.0f - alpha * a) / a0;
        me_job.eq_coef[b][3] = -2.0f * cw / a0;
        me_job.eq_coef[b][4] = (1.0f - alpha / a) / a0;
    }
    /* Leave headroom for the boost so loud passages do not clip. */
    me_job.eq_preamp = powf(10.0f, -boost / 20.0f);
    memset(me_job.eq_state, 0, sizeof me_job.eq_state);
    me_job.eq_on = 1;
}

static int me_dispatch(stb_vorbis *v, const uint8_t *data, size_t len, int first,
                       int16_t *pcm)
{
    /* The fetch thread wrote the input through the SC cache. */
    sceKernelDcacheWritebackRange(data, len);
    me_job.magic = TASK_MAGIC;
    me_job.version = TASK_VERSION;
    me_job.first = (uint32_t)first;
    me_job.decoder = v;
    me_job.decoder_memory = vorbis_memory;
    me_job.decoder_memory_size = sizeof vorbis_memory;
    me_job.data = data;
    me_job.data_len = (int32_t)len;
    me_job.pcm = pcm;
    me_job.pcm_frames_cap = ME_VORBIS_MAX_FRAMES;
    me_job.used = me_job.frames = me_job.channels = 0;
    me_job.flush = me_pending_flush;
    me_pending_flush = 0;
    if (++me_sequence == 0) ++me_sequence;
    me_job.sequence = me_sequence;
    me_job.completed_sequence = 0;
    me_job.state = ME_VORBIS_READY;
    shared_sync();
    sceKernelDcacheWritebackInvalidateRange(&me_job, sizeof me_job);
    me_task.func = me_vorbis_task;
    me_task.param = &me_job;
    return meSafeTaskDispatch(&me_task);
}

/* Returns 0 with me_job results valid, or a negative error. A timeout
 * means the ME may still be running: the caller must not dispatch again. */
static int me_collect(SceInt64 *wait_us)
{
    SceInt64 t0 = sceKernelGetSystemTimeWide();
    int ret = meSafeTaskWaitReadyTimeout(ME_TIMEOUT_US);
    *wait_us += sceKernelGetSystemTimeWide() - t0;
    sceKernelDcacheInvalidateRange(&me_job, sizeof me_job);
    shared_sync();
    if (ret < 0) {
        log_printf("PLAYER: ME timeout 0x%08X (state %u)\n", (unsigned int)ret,
                   (unsigned int)me_job.state);
        return PLAYER_ERR_ME_TIMEOUT;
    }
    if (me_job.state != ME_VORBIS_DONE || me_job.completed_sequence != me_sequence) {
        log_printf("PLAYER: ME job error (state %u, seq %u/%u)\n", (unsigned int)me_job.state,
                   (unsigned int)me_job.completed_sequence, (unsigned int)me_sequence);
        return -20;
    }
    if (me_job.frames > 0)
        sceKernelDcacheInvalidateRange(me_job.pcm, ((uint32_t)me_job.frames * 4u + 63u) & ~63u);
    return 0;
}

static void report(const fetcher *f, const ring_state *ring, unsigned int rate,
                   unsigned int waits, SceInt64 started, SceInt64 me_wait_us)
{
    log_printf("PLAYER: %u s played, %u/%u KB fetched, %u waits, %u ms wall, SC waited %u ms on ME\n",
               (unsigned int)(ring->frames / rate),
               (unsigned int)(f->buffered / 1024), (unsigned int)(f->capacity / 1024), waits,
               (unsigned int)((sceKernelGetSystemTimeWide() - started) / 1000),
               (unsigned int)(me_wait_us / 1000));
}

static uint8_t ring_byte(const fetcher *f, size_t p)
{
    return f->buf[p % f->ring];
}

/* First Ogg page header at or after `from`, before `limit`; SIZE_MAX if none. */
static size_t find_page(const fetcher *f, size_t from, size_t limit)
{
    for (size_t p = from; p + 27 <= limit; ++p)
        if (ring_byte(f, p) == 'O' && ring_byte(f, p + 1) == 'g' && ring_byte(f, p + 2) == 'g' &&
            ring_byte(f, p + 3) == 'S' && ring_byte(f, p + 4) == 0)
            return p;
    return SIZE_MAX;
}

/* A page's granule position: the sample count at its end. */
static uint64_t page_granule(const fetcher *f, size_t page)
{
    uint64_t g = 0;
    for (int i = 7; i >= 0; --i) g = (g << 8) | ring_byte(f, page + 6 + (size_t)i);
    return g;
}

static void fetch_stop(fetcher *f, SceUID thid)
{
    f->stop = 1;
    SceUInt timeout = 20u * 1000u * 1000u;
    if (sceKernelWaitThreadEnd(thid, &timeout) < 0) sceKernelTerminateThread(thid);
    sceKernelDeleteThread(thid);
    http_stream_close(&f->http);
}

static SceUID fetch_start(fetcher **fp)
{
    /* Below the UI thread (0x20): the download runs well ahead of playback,
     * and at a higher priority its TLS and AES work made the UI stutter. */
    SceUID thid = sceKernelCreateThread("ogg_fetch", fetch_thread, 0x24, 0x10000, 0, NULL);
    if (thid < 0 || sceKernelStartThread(thid, sizeof *fp, fp) < 0) {
        log_printf("PLAYER: fetch thread failed 0x%08X\n", (unsigned int)thid);
        if (thid >= 0) sceKernelDeleteThread(thid);
        return -1;
    }
    return thid;
}

/* Restarts the download at file offset `at` with an HTTP range request. */
static SceUID fetch_restart(fetcher *f, fetcher **fp, SceUID thid, const char *urls,
                            unsigned int url_stride, int url_count, size_t at)
{
    fetch_stop(f, thid);
    char range[48];
    snprintf(range, sizeof range, "Range: bytes=%u-\r\n", (unsigned int)at);
    int status = -1;
    for (int u = 0; u < url_count && status != 206; ++u) {
        uint64_t length = 0;
        status = http_stream_open_download(&f->http, urls + (size_t)u * url_stride, range,
                                           &length);
        if (status > 0 && status != 206) http_stream_close(&f->http);
    }
    if (status != 206) {
        log_printf("PLAYER: range request failed (HTTP %d)\n", status);
        return -1;
    }
    audio_decrypt_seek(&f->decrypt, at);
    f->buffered = at;
    f->consumed = at;
    f->valid_lo = at;
    f->stop = 0;
    f->open = 1;
    return fetch_start(fp);
}

int spotify_play_ogg(const char *urls, unsigned int url_stride, int url_count,
                     const uint8_t key[16], player_control *control)
{
    static fetcher f;
    memset(&f, 0, sizeof f);
    f.http.sock = -1;
    uint64_t content_length = 0;
    int status = -1;
    log_printf("PLAYER: opening full file (%d CDN URLs)\n", url_count);
    for (int u = 0; u < url_count && status != 200; ++u) {
        status = http_stream_open_download(&f.http, urls + (size_t)u * url_stride, NULL,
                                           &content_length);
        if (status > 0 && status != 200) {
            log_printf("PLAYER: CDN %d HTTP %d\n", u, status);
            http_stream_close(&f.http);
        }
    }
    if (status != 200 || !f.http.length_known || f.http.chunked ||
        content_length <= SPOTIFY_OGG_HEADER || content_length > SIZE_MAX ||
        content_length > 32u * 1024 * 1024) {
        log_printf("PLAYER: track skipped: HTTP %d, missing/unknown/chunked or unsafe Content-Length=%llu\n",
                   status, (unsigned long long)content_length);
        if (status > 0) http_stream_close(&f.http);
        return -1;
    }
    f.capacity = (size_t)content_length;
    memory_log("pre-track");
    size_t budget = 0;
    if (spotify_device_model == 0) {
        /* Keep cover/TLS mallocs from racing the snapshot and allocation. */
        __malloc_lock(_REENT);
        budget = memory_track_budget(memory_read());
    }
    for (size_t i = 0; i < sizeof ring_sizes / sizeof ring_sizes[0] && !f.buf; ++i) {
        f.wraps = f.capacity > ring_sizes[i];
        f.ring = f.wraps ? ring_sizes[i] : f.capacity;
        size_t bytes = f.ring + (f.wraps ? ME_WINDOW_MAX : 0);
        if (spotify_device_model == 0 && bytes > budget) continue;
        f.buf = malloc(bytes);
    }
    if (spotify_device_model == 0) {
        __malloc_unlock(_REENT);
        log_printf("MEM track-budget: model=0 content_length=%u safe_budget=%u heap_margin=4194304 kernel_guard=1048576\n",
                   (unsigned int)f.capacity, (unsigned int)budget);
    }
    if (!f.buf) {
        log_printf("PLAYER: track skipped: no memory for a stream buffer (file %u KB)\n",
                   (unsigned int)(f.capacity / 1024));
        memory_log("track-allocation-failed");
        http_stream_close(&f.http);
        return PLAYER_ERR_NO_MEMORY;
    }
    memory_log("post-track-allocation");
    audio_decrypt_init(&f.decrypt, key);
    f.open = 1;
    if (f.wraps)
        log_printf("PLAYER: %u KB Ogg Vorbis file, streaming through %u KB\n",
                   (unsigned int)(f.capacity / 1024), (unsigned int)(f.ring / 1024));
    else
        log_printf("PLAYER: %u KB Ogg Vorbis file\n", (unsigned int)(f.capacity / 1024));


    fetcher *fp = &f;
    /* Above the player and UI threads: with the ME pipeline the download
     * starved at 0x30 (255 KB in the first 10 s, 44 decoder waits). It sleeps
     * in recv most of the time, so a higher priority costs nothing. */
    memory_log("pre-fetch-thread");
    SceUID thid = fetch_start(&fp);
    if (thid < 0) {
        http_stream_close(&f.http);
        free(f.buf);
        memory_log("track-cleanup");
        return -3;
    }

    int ret = 0;
    int me_used = 0;
    stb_vorbis *v = NULL;
    stb_vorbis_alloc alloc = { vorbis_memory, (int)sizeof vorbis_memory };
    size_t pos = SPOTIFY_OGG_HEADER, audio_start = SPOTIFY_OGG_HEADER;
    f.consumed = pos;
    int eq_applied = -1;
    unsigned int eq_version = 0;
    float norm = 1.0f;
    int channel = -1;
    unsigned int waits = 0;
    ring_state ring = { 0, 0, 0 };
    SceInt64 me_wait_us = 0;

    /* Open on the SC once enough of the headers (comment + codebooks) has
     * arrived: setup needs libm and runs only once. */
    for (;;) {
        size_t have = f.buffered;
        if (have > pos) {
            int used = 0, error = 0;
            /* The headers sit at the start of the ring, contiguous for up to
             * ME_WINDOW_MAX bytes past its end thanks to the mirror. */
            size_t avail = have - pos;
            if (f.wraps && avail > ME_WINDOW_MAX) avail = ME_WINDOW_MAX;
            v = stb_vorbis_open_pushdata(f.buf + pos, (int)avail, &used, &error, &alloc);
            if (v) { pos += (size_t)used; f.consumed = pos; audio_start = pos; break; }
            if (error != VORBIS_need_more_data) {
                log_printf("PLAYER: Vorbis open error %d\n", error);
                ret = -4;
                goto out;
            }
        }
        if (!f.open && f.buffered == have) {
            log_printf("PLAYER: stream ended before Vorbis headers\n");
            ret = -5;
            goto out;
        }
        sceKernelDelayThread(10000);
    }

    norm = normalization_factor(f.buf);
    stb_vorbis_info info = stb_vorbis_get_info(v);
    log_printf("PLAYER: %u Hz, %d ch, decoder memory %u KB\n", info.sample_rate, info.channels,
               (unsigned int)((info.setup_memory_required + info.temp_memory_required) / 1024));
    if (info.channels < 1 || info.channels > 2) { ret = -6; goto out; }

    channel = sceAudioSRCChReserve(BLOCK_FRAMES, (int)info.sample_rate, 2);
    if (channel < 0) {
        log_printf("PLAYER: audio reserve 0x%08X\n", (unsigned int)channel);
        ret = channel;
        goto out;
    }

    unsigned long long next_report = 10ull * info.sample_rate;
    SceInt64 started = sceKernelGetSystemTimeWide();

    log_printf("--- PLAYING (Vorbis decode on Media Engine) ---\n");
    /* From here the decoder state belongs to the ME: push the SC's setup
     * writes to RAM and keep none of it in the SC cache. */
    sceKernelDcacheWritebackInvalidateRange(vorbis_memory, sizeof vorbis_memory);
    {
        int cur = 0, first = 1, prev_frames = 0;
        size_t window = ME_WINDOW_MIN;
        for (;;) {
            if (control->stop) { ret = PLAYER_STOPPED; break; }
            if (control->eq_preset != eq_applied || control->eq_version != eq_version) {
                eq_applied = control->eq_preset;
                eq_version = control->eq_version;
                eq_configure(eq_applied, control->eq_custom, info.sample_rate);
            }
            if (control->seek_ms >= 0) {
                const unsigned int target = (unsigned int)control->seek_ms;
                control->seek_ms = -1;
                if (control->duration_ms > 0 && f.capacity > audio_start + 1) {
                    /* Aim by bitrate, then land on the next Ogg page. */
                    const size_t span = f.capacity - audio_start;
                    size_t at = audio_start +
                                (size_t)((uint64_t)span * target / control->duration_ms);
                    if (at >= f.capacity) at = f.capacity - 1;
                    size_t lo = f.valid_lo;
                    if (f.wraps && f.buffered > f.ring + ME_WINDOW_MAX &&
                        f.buffered - f.ring + ME_WINDOW_MAX > lo)
                        lo = f.buffered - f.ring + ME_WINDOW_MAX;
                    const int in_buffer = at >= lo &&
                        (at + SEEK_LOOKAHEAD <= f.buffered || (!f.open && at < f.buffered));
                    if (!in_buffer) {
                        thid = fetch_restart(&f, &fp, thid, urls, url_stride, url_count, at);
                        if (thid < 0) { ret = -7; goto out; }
                    }
                    for (int tries = 0; tries < 1000 && f.open && !control->stop &&
                                        f.buffered < at + SEEK_LOOKAHEAD; ++tries)
                        sceKernelDelayThread(10000);
                    size_t page = find_page(&f, at, f.buffered);
                    if (page != SIZE_MAX) {
                        pos = page;
                        f.consumed = pos;
                        prev_frames = 0;
                        window = ME_WINDOW_MIN;
                        me_pending_flush = 1;
                        uint64_t granule = page_granule(&f, page);
                        ring.frames = granule != UINT64_MAX ? granule
                                    : (unsigned long long)target * info.sample_rate / 1000u;
                        control->position_ms = (unsigned int)(ring.frames * 1000ull / info.sample_rate);
                        next_report = ring.frames + 10ull * info.sample_rate;
                        log_printf("PLAYER: seek to %u s (%s)\n", control->position_ms / 1000u,
                                   in_buffer ? "buffered" : "range request");
                    }
                }
            }
            if (control->paused) { sceKernelDelayThread(20000); continue; }
            size_t have = f.buffered;
            control->fetched_pct = (unsigned int)((uint64_t)have * 100u / f.capacity);
            if (have <= pos) {
                if (!f.open && f.buffered == have) break;   /* end of file */
                ++waits;
                if (prev_frames) {   /* keep audio going while waiting */
                    ret = ring_push(&ring, me_pcm[cur ^ 1], prev_frames, output_volume(control, norm));
                    if (ret < 0) goto out;
                    prev_frames = 0;
                }
                sceKernelDelayThread(5000);
                continue;
            }
            size_t len = have - pos;
            if (len > window) len = window;

            ret = me_dispatch(v, f.buf + pos % f.ring, len, first, me_pcm[cur]);
            if (ret < 0) {
                log_printf("PLAYER: ME dispatch 0x%08X\n", (unsigned int)ret);
                goto out;
            }
            me_used = 1;
            first = 0;
            /* The SC plays the previous frame while the ME decodes this one. */
            if (prev_frames) {
                ret = ring_push(&ring, me_pcm[cur ^ 1], prev_frames, output_volume(control, norm));
                prev_frames = 0;
                control->position_ms = (unsigned int)(ring.frames * 1000ull / info.sample_rate);
                if (ret < 0) { me_collect(&me_wait_us); goto out; }
            }
            if ((ret = me_collect(&me_wait_us)) < 0) goto out;

            if (me_job.used == 0 && me_job.frames == 0) {
                /* Needs more input: widen the window or wait for the network. */
                if (len == window && window < ME_WINDOW_MAX && have - pos > window) {
                    window *= 2;
                } else if (!f.open && f.buffered == have) {
                    break;
                } else {
                    ++waits;
                    sceKernelDelayThread(5000);
                }
                continue;
            }
            pos += (size_t)me_job.used;
            f.consumed = pos;   /* the fetch thread may now reuse that space */
            prev_frames = me_job.frames;
            cur ^= 1;
            if (ring.frames >= next_report) {
                next_report += 10ull * info.sample_rate;
                report(&f, &ring, info.sample_rate, waits, started, me_wait_us);
            }
        }
        if (ret == 0 && prev_frames) {
            ret = ring_push(&ring, me_pcm[cur ^ 1], prev_frames, output_volume(control, norm));
            if (ret < 0) goto out;
        }
    }
    if (ret == 0) ring_flush(&ring, output_volume(control, norm));
    log_printf("PLAYER: %s, %u s, %u waits, SC waited %u ms on ME\n",
               ret == PLAYER_STOPPED ? "stopped" : "finished",
               (unsigned int)(ring.frames / info.sample_rate), waits,
               (unsigned int)(me_wait_us / 1000));

out:
    if (channel >= 0) {
        sceKernelDelayThread(50000);
        sceAudioSRCChRelease();
    }
    /* With caller-provided memory close() frees nothing; skip it once the ME
     * owns the decoder state so the SC never touches it again. */
    if (v && !me_used) stb_vorbis_close(v);
    if (thid >= 0) fetch_stop(&f, thid);
    else http_stream_close(&f.http);
    free(f.buf);
    memory_log("track-cleanup");
    return ret;
}
