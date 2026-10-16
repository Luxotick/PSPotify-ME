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
/* player_eq_names, then the user's own. */
static const char *const eq_names[PLAYER_EQ_PRESETS + 1] = {
    "Off", "Bass boost", "Treble boost", "Vocal", "Rock", "Pop", "Loudness", "Custom"
};
static const char *const repeat_names[] = { "Off", "All", "One" };
/* The order of ui.c's sleep_minutes[]. */
static const char *const sleep_names[] = { "Off", "15 min", "30 min", "45 min", "60 min",
                                           "90 min" };
static const char *const cpu_names[] = { "333 MHz (fastest)", "266 MHz", "222 MHz (saves battery)" };
static const char *const cpu_tokens[] = { "333", "266", "222" };
static const char *const confirm_names[] = { "Cross", "Circle" };
static const char *const confirm_tokens[] = { "cross", "circle" };
static const char *const lr_names[] = { "Tap skips, hold seeks", "Seek", "Skip" };
static const char *const lr_tokens[] = { "both", "seek", "skip" };
static const char *const step_names[] = { "2%", "5%", "10%" };
static const char *const step_tokens[] = { "2", "5", "10" };

#define CHOICE(k, l, g, n, t, def) \
    { .key = k, .label = l, .group = g, .count = (int)(sizeof n / sizeof n[0]), .names = n, \
      .tokens = t, .fallback = def }
#define BAND(k, l) \
    { .key = k, .label = l, .group = PREF_GROUP_SOUND, .count = 13, .fallback = 6, \
      .min = EQ_BAND_MIN, .step = EQ_BAND_STEP, .unit = " dB" }

static pref_def defs[PREF_COUNT] = {
    [PREF_COVER_COLORS] = CHOICE("cover_colors", "Cover colors", PREF_GROUP_APPEARANCE, off_on,
                                 on_off_tokens, 1),
    [PREF_BACKGROUND] = CHOICE("background", "Background", PREF_GROUP_APPEARANCE,
                               background_names, background_tokens, 0),
    [PREF_COVER_SHAPE] = CHOICE("cover_shape", "Cover shape", PREF_GROUP_APPEARANCE, shape_names,
                                shape_tokens, 0),
    [PREF_TEXT_SIZE] = CHOICE("text_size", "Text size", PREF_GROUP_APPEARANCE, text_names,
                              text_tokens, 0),
    [PREF_DENSITY] = CHOICE("list_style", "List style", PREF_GROUP_APPEARANCE, density_names,
                            density_tokens, 0),
    [PREF_ANIMATIONS] = CHOICE("animations", "Animations", PREF_GROUP_APPEARANCE, anim_names,
                               anim_tokens, 0),
    [PREF_CLOCK] = CHOICE("clock", "Clock", PREF_GROUP_APPEARANCE, clock_names, clock_tokens, 0),
    [PREF_HINTS] = CHOICE("button_hints", "Button hints", PREF_GROUP_APPEARANCE, hint_names,
                          hint_tokens, 0),
    [PREF_START] = CHOICE("start_screen", "Start screen", PREF_GROUP_LAYOUT, start_names,
                          start_tokens, 0),
    [PREF_NOW_LAYOUT] = CHOICE("now_playing", "Now Playing", PREF_GROUP_LAYOUT, now_names,
                               now_tokens, 0),
    [PREF_PLAYLIST_SORT] = CHOICE("playlist_sort", "Sort playlists", PREF_GROUP_LAYOUT,
                                  sort_names, sort_tokens, 0),
    [PREF_EQUALIZER] = CHOICE("equalizer", "Equalizer", PREF_GROUP_SOUND, eq_names, NULL, 0),
    [PREF_EQ_60] = BAND("eq_60", "60 Hz (bass)"),
    [PREF_EQ_230] = BAND("eq_230", "230 Hz"),
    [PREF_EQ_910] = BAND("eq_910", "910 Hz (voices)"),
    [PREF_EQ_3600] = BAND("eq_3600", "3.6 kHz"),
    [PREF_EQ_14K] = BAND("eq_14000", "14 kHz (treble)"),
    [PREF_NORMALIZE] = CHOICE("normalize", "Same loudness for every song", PREF_GROUP_SOUND,
                              off_on, on_off_tokens, 0),
    [PREF_SLEEP] = { .key = "sleep_timer", .label = "Sleep timer", .group = PREF_GROUP_PLAYBACK,
                     .count = 6, .names = sleep_names, .transient = 1 },
    [PREF_BATTERY] = CHOICE("battery_saver", "Battery saver", PREF_GROUP_PLAYBACK, off_on,
                            on_off_tokens, 0),
    [PREF_CPU] = CHOICE("cpu_speed", "CPU speed", PREF_GROUP_PLAYBACK, cpu_names, cpu_tokens, 0),
    [PREF_CONNECT] = CHOICE("connect", "Spotify Connect", PREF_HIDDEN, off_on, on_off_tokens, 0),
    [PREF_CONFIRM] = CHOICE("confirm_button", "Confirm button", PREF_GROUP_CONTROLS,
                            confirm_names, confirm_tokens, 0),
    [PREF_LR] = CHOICE("lr_buttons", "L and R", PREF_GROUP_CONTROLS, lr_names, lr_tokens, 0),
    [PREF_VOLUME_STEP] = CHOICE("volume_step", "Volume step", PREF_GROUP_CONTROLS, step_names,
                                step_tokens, 1),
    [PREF_REMOTE] = CHOICE("headphone_remote", "Headphone remote", PREF_GROUP_CONTROLS, off_on,
                           on_off_tokens, 1),
    [PREF_SHUFFLE] = CHOICE("shuffle", "Shuffle", PREF_HIDDEN, off_on, NULL, 0),
    [PREF_REPEAT] = CHOICE("repeat", "Repeat", PREF_HIDDEN, repeat_names, NULL, 0),
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

static int step_of(const pref_def *d)
{
    return d->step ? d->step : 1;
}

int prefs_read(const char *key, const char *value)
{
    for (int i = 0; i < PREF_COUNT; ++i) {
        const pref_def *d = &defs[i];
        if (strcmp(d->key, key) != 0) continue;
        if (d->transient) return 1;
        if (d->tokens) {
            for (int v = 0; v < d->count; ++v)
                if (!strcmp(d->tokens[v], value)) prefs[i] = v;
        } else {
            char *end;
            long n = strtol(value, &end, 10);
            long v = (n - d->min) / step_of(d);
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
        if (d->transient) continue;
        if (d->tokens) fprintf(f, "%s = %s\n", d->key, d->tokens[prefs[i]]);
        else fprintf(f, "%s = %d\n", d->key, pref_number((pref_id)i));
    }
}

void prefs_step(pref_id id, int dir)
{
    const pref_def *d = &defs[id];
    const int n = d->count;
    if (!d->names) {
        const int v = prefs[id] + dir;
        prefs[id] = v < 0 ? 0 : v >= n ? n - 1 : v;
        return;
    }
    prefs[id] = ((prefs[id] + dir) % n + n) % n;
}

int pref_number(pref_id id)
{
    return defs[id].min + prefs[id] * step_of(&defs[id]);
}

const char *pref_value_name(pref_id id)
{
    const pref_def *d = &defs[id];
    if (d->names) return d->names[prefs[id]];
    static char text[24];
    const int v = pref_number(id);
    snprintf(text, sizeof text, v > 0 ? "+%d%s" : "%d%s", v, d->unit ? d->unit : "");
    return text;
}

void pref_set_shown(pref_id id, int shown)
{
    defs[id].group = shown ? shown_group[id] : PREF_HIDDEN;
}

int pref_shown(pref_id id)
{
    return defs[id].group != PREF_HIDDEN;
}
