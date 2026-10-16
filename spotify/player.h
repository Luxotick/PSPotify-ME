#ifndef SPOTIFY_PLAYER_H
#define SPOTIFY_PLAYER_H

#include <stdint.h>

/* The ME did not finish a decode in time; it may still be running, so no
 * further ME work may be dispatched and the app must reboot to exit. */
#define PLAYER_ERR_ME_TIMEOUT (-100)
/* Not even the smallest stream buffer could be allocated. */
#define PLAYER_ERR_NO_MEMORY (-101)
/* Playback ended because control->stop was set. */
#define PLAYER_STOPPED 1

/* Shared between the UI thread (writes stop/paused/volume) and the player
 * thread (writes the status fields). */
typedef struct {
    volatile int stop;
    volatile int paused;
    volatile int volume;            /* 0 .. PSP_AUDIO_VOLUME_MAX */
    volatile unsigned int position_ms;
    volatile unsigned int fetched_pct;
    /* >= 0: jump to this time; the player sets it back to -1. */
    volatile int seek_ms;
    /* Set by the caller from the metadata; aims seeks within the file. */
    volatile unsigned int duration_ms;
    /* Equalizer preset, an index into player_eq_names (0 = off), or
     * PLAYER_EQ_CUSTOM for the gains in eq_custom (dB at 60, 230, 910,
     * 3600 and 14000 Hz). Bump eq_version after changing eq_custom. */
    volatile int eq_preset;
    volatile int eq_custom[5];
    volatile unsigned int eq_version;
    /* 1: play every track at the same loudness, from the gain Spotify
     * stores in each file's header. */
    volatile int normalize;
    /* 1: fill spectrum about 30 times a second with the level of
     * PLAYER_BANDS log-spaced bands (0..1) of what is playing. */
    volatile int visualizer;
    volatile float spectrum[16];
} player_control;

#define PLAYER_BANDS 16

#define PLAYER_EQ_PRESETS 7
#define PLAYER_EQ_CUSTOM PLAYER_EQ_PRESETS
extern const char *const player_eq_names[PLAYER_EQ_PRESETS];

/* Streams one Spotify Ogg Vorbis file: downloads from the first working CDN
 * URL on a background thread, AES-CTR decrypts as it arrives, skips the
 * 0xa7-byte Spotify header, decodes with stb_vorbis on the Media Engine and
 * plays it. urls points at url_count NUL-terminated strings, url_stride
 * bytes apart. Returns 0 at the end of the track, PLAYER_STOPPED, or < 0. */
int spotify_play_ogg(const char *urls, unsigned int url_stride, int url_count,
                     const uint8_t key[16], player_control *control);

#endif /* SPOTIFY_PLAYER_H */
