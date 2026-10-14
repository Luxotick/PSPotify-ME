#ifndef SPOTIFY_CONNECT_H
#define SPOTIFY_CONNECT_H
/* Spotify Connect: the PSP shows up as a device in the Spotify apps, which
 * can then start playback on it, transfer what they are playing, and pause,
 * skip, seek, shuffle or change the volume. A background thread keeps the
 * dealer websocket open and publishes this device's state to connect-state;
 * the UI thread takes the commands and reports what it plays. */
#include "session.h"
#include "webapi.h"

typedef enum {
    CONNECT_PLAY,           /* tracks/count/index; position_ms, paused */
    CONNECT_PAUSE,
    CONNECT_RESUME,
    CONNECT_NEXT,
    CONNECT_PREV,
    CONNECT_SEEK,           /* value: ms */
    CONNECT_SHUFFLE,        /* value: 0/1 */
    CONNECT_REPEAT_CONTEXT, /* value: 0/1 */
    CONNECT_REPEAT_TRACK,   /* value: 0/1 */
    CONNECT_VOLUME,         /* value: 0-100 */
    CONNECT_RELEASE         /* another device took over playback */
} connect_command_type;

typedef struct {
    connect_command_type type;
    int value;
    /* CONNECT_PLAY: valid until connect_command_done() */
    const spotify_track *tracks;
    int count, index, position_ms, paused;
    int shuffle, repeat;    /* -1: leave as is */
    char context_uri[96];
} connect_command;

/* What the UI is playing, sent to the Spotify apps when it changes. */
typedef struct {
    int active;             /* a track is loaded (playing or paused) */
    int playing;
    char track_id[24];
    char context_uri[96];
    unsigned int position_ms, duration_ms;
    int shuffle, repeat, volume;
} connect_status;

void connect_start(spotify_ctx *ctx, const char *device_name);
void connect_stop(void);
/* 1 while the device is registered and reachable from the apps. */
int connect_online(void);
/* UI thread: takes the next command, if any. */
int connect_poll(connect_command *out);
/* UI thread: a CONNECT_PLAY command's tracks are no longer needed. */
void connect_command_done(void);
/* UI thread, every frame: what is playing now. */
void connect_report(const connect_status *status);

#endif
