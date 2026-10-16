#include "prefs.h"
#include "player.h"
#include <stdlib.h>
#include <string.h>

int prefs[PREF_COUNT];

static const char *const on_off_tokens[] = { "off", "on" };
static const char *const off_on[] = { "Off", "On" };
static const char *const background_names[] = { "Plain", "Blurred cover", "Theme wallpaper" };
static const char *const background_tokens[] = { "plain", "blurred_cover", "wallpaper" };
static const char *const shape_names[] = { "Square", "Rounded", "Circle" };
static const char *const shape_tokens[] = { "square", "rounded", "circle" };
static const char *const text_names[] = { "Normal", "Small", "Large" };
static const char *const text_tokens[] = { "normal", "small", "large" };
static const char *const density_names[] = { "Comfortable", "Compact", "Text only" };
static const char *const density_tokens[] = { "comfortable", "compact", "text" };
static const char *const anim_names[] = { "Smooth", "Fast", "Off" };
static const char *const anim_tokens[] = { "smooth", "fast", "off" };
static const char *const clock_names[] = { "24-hour", "12-hour", "Hidden" };
static const char *const clock_tokens[] = { "24h", "12h", "hidden" };
static const char *const hint_names[] = { "Shown", "Hidden" };
static const char *const hint_tokens[] = { "shown", "hidden" };
/* The order of ui.c's start_categories[]. */
static const char *const start_names[] = { "Playlists", "Liked Songs", "Search", "Recent",
                                           "Now Playing" };
static const char *const start_tokens[] = { "playlists", "liked", "search", "recent", "now" };
static const char *const now_names[] = { "Classic", "Centered", "Text only" };
static const char *const now_tokens[] = { "classic", "centered", "text" };
static const char *const sort_names[] = { "Spotify order", "Name", "Most songs" };
static const char *const sort_tokens[] = { "spotify", "name", "size" };
static const char *const repeat_names[] = { "Off", "All", "One" };

static pref_def defs[PREF_COUNT] = {
    [PREF_COVER_COLORS] = { "cover_colors", "Cover colors", PREF_GROUP_APPEARANCE, 2, off_on,
                            on_off_tokens, 1 },
    [PREF_BACKGROUND] = { "background", "Background", PREF_GROUP_APPEARANCE, 3, background_names,
                          background_tokens, 0 },
    [PREF_COVER_SHAPE] = { "cover_shape", "Cover shape", PREF_GROUP_APPEARANCE, 3, shape_names,
                           shape_tokens, 0 },
    [PREF_TEXT_SIZE] = { "text_size", "Text size", PREF_GROUP_APPEARANCE, 3, text_names,
                         text_tokens, 0 },
    [PREF_DENSITY] = { "list_style", "List style", PREF_GROUP_APPEARANCE, 3, density_names,
                       density_tokens, 0 },
    [PREF_ANIMATIONS] = { "animations", "Animations", PREF_GROUP_APPEARANCE, 3, anim_names,
                          anim_tokens, 0 },
    [PREF_CLOCK] = { "clock", "Clock", PREF_GROUP_APPEARANCE, 3, clock_names, clock_tokens, 0 },
    [PREF_HINTS] = { "button_hints", "Button hints", PREF_GROUP_APPEARANCE, 2, hint_names,
                     hint_tokens, 0 },
    [PREF_START] = { "start_screen", "Start screen", PREF_GROUP_LAYOUT, 5, start_names,
                     start_tokens, 0 },
    [PREF_NOW_LAYOUT] = { "now_playing", "Now Playing", PREF_GROUP_LAYOUT, 3, now_names,
                          now_tokens, 0 },
    [PREF_PLAYLIST_SORT] = { "playlist_sort", "Sort playlists", PREF_GROUP_LAYOUT, 3, sort_names,
                             sort_tokens, 0 },
    [PREF_EQUALIZER] = { "equalizer", "Equalizer", PREF_GROUP_SOUND, PLAYER_EQ_PRESETS,
                         player_eq_names, NULL, 0 },
    [PREF_CONNECT] = { "connect", "Spotify Connect", PREF_HIDDEN, 2, off_on, on_off_tokens, 0 },
    [PREF_SHUFFLE] = { "shuffle", "Shuffle", PREF_HIDDEN, 2, off_on, NULL, 0 },
    [PREF_REPEAT] = { "repeat", "Repeat", PREF_HIDDEN, 3, repeat_names, NULL, 0 },
};

/* Where a hidden-by-default pref goes when shown. */
static const int shown_group[PREF_COUNT] = { [PREF_CONNECT] = PREF_GROUP_PLAYBACK };

const pref_def *pref_info(pref_id id)
{
    return &defs[id];
}

void prefs_defaults(void)
{
    for (int i = 0; i < PREF_COUNT; ++i) prefs[i] = defs[i].fallback;
}

int prefs_read(const char *key, const char *value)
{
    for (int i = 0; i < PREF_COUNT; ++i) {
        const pref_def *d = &defs[i];
        if (strcmp(d->key, key) != 0) continue;
        if (d->tokens) {
            for (int v = 0; v < d->count; ++v)
                if (!strcmp(d->tokens[v], value)) prefs[i] = v;
        } else {
            char *end;
            long v = strtol(value, &end, 10);
            if (end != value && v >= 0 && v < d->count) prefs[i] = (int)v;
        }
        return 1;
    }
    return 0;
}

void prefs_write(FILE *f)
{
    for (int i = 0; i < PREF_COUNT; ++i) {
        const pref_def *d = &defs[i];
        if (d->tokens) fprintf(f, "%s = %s\n", d->key, d->tokens[prefs[i]]);
        else fprintf(f, "%s = %d\n", d->key, prefs[i]);
    }
}

void prefs_step(pref_id id, int dir)
{
    const int n = defs[id].count;
    prefs[id] = ((prefs[id] + dir) % n + n) % n;
}

const char *pref_value_name(pref_id id)
{
    return defs[id].names[prefs[id]];
}

void pref_set_shown(pref_id id, int shown)
{
    defs[id].group = shown ? shown_group[id] : PREF_HIDDEN;
}

int pref_shown(pref_id id)
{
    return defs[id].group != PREF_HIDDEN;
}
