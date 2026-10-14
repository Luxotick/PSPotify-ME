#include "player.h"
#include "audiodecrypt.h"
#include "http.h"
#include "log.h"
#include "memory.h"
#include "me_vorbis.h"
#include "../common.h"
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
        status = http_stream_open(&f.http, urls + (size_t)u * url_stride, &content_length);
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
    /* Below the UI thread (0x20): the download runs well ahead of playback,
     * and at a higher priority its TLS and AES work made the UI stutter. */
    SceUID thid = sceKernelCreateThread("ogg_fetch", fetch_thread, 0x24, 0x10000, 0, NULL);
    if (thid < 0 || sceKernelStartThread(thid, sizeof fp, &fp) < 0) {
        log_printf("PLAYER: fetch thread failed 0x%08X\n", (unsigned int)thid);
        if (thid >= 0) sceKernelDeleteThread(thid);
        http_stream_close(&f.http);
        free(f.buf);
        memory_log("track-cleanup");
        return -3;
    }

    int ret = 0;
    int me_used = 0;
    stb_vorbis *v = NULL;
    stb_vorbis_alloc alloc = { vorbis_memory, (int)sizeof vorbis_memory };
    size_t pos = SPOTIFY_OGG_HEADER;
    f.consumed = pos;
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
            if (v) { pos += (size_t)used; f.consumed = pos; break; }
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
            if (control->paused) { sceKernelDelayThread(20000); continue; }
            size_t have = f.buffered;
            control->fetched_pct = (unsigned int)((uint64_t)have * 100u / f.capacity);
            if (have <= pos) {
                if (!f.open && f.buffered == have) break;   /* end of file */
                ++waits;
                if (prev_frames) {   /* keep audio going while waiting */
                    ret = ring_push(&ring, me_pcm[cur ^ 1], prev_frames, control->volume);
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
                ret = ring_push(&ring, me_pcm[cur ^ 1], prev_frames, control->volume);
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
            ret = ring_push(&ring, me_pcm[cur ^ 1], prev_frames, control->volume);
            if (ret < 0) goto out;
        }
    }
    if (ret == 0) ring_flush(&ring, control->volume);
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
    f.stop = 1;
    SceUInt timeout = 20u * 1000u * 1000u;
    if (sceKernelWaitThreadEnd(thid, &timeout) < 0) sceKernelTerminateThread(thid);
    sceKernelDeleteThread(thid);
    http_stream_close(&f.http);
    free(f.buf);
    memory_log("track-cleanup");
    return ret;
}
