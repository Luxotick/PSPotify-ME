#ifndef SPOTIFY_PREFS_H
#define SPOTIFY_PREFS_H
/* The user's choices in Settings: one table gives each its settings.ini key,
 * its label and its values, so the Settings screen, loading and saving all
 * come from the same place. */
#include <stdio.h>

/* The groups on the Settings screen's first level, in this order. Themes,
 * the theme gallery, Categories and About are lists of their own; the others
 * list prefs. */
enum {
    PREF_GROUP_THEMES,
    PREF_GROUP_GALLERY,
    PREF_GROUP_APPEARANCE,
    PREF_GROUP_LAYOUT,
    PREF_GROUP_CATEGORIES,
    PREF_GROUP_SOUND,
    PREF_GROUP_PLAYBACK,
    PREF_GROUP_CONTROLS,
    PREF_GROUP_ABOUT,
    PREF_GROUPS,
    PREF_HIDDEN = -1        /* saved, but not shown in Settings */
};

typedef enum {
    /* Appearance */
    PREF_COVER_COLORS,
    PREF_BACKGROUND,
    PREF_COVER_SHAPE,
    PREF_TEXT_SIZE,
    PREF_DENSITY,
    PREF_ANIMATIONS,
    PREF_CLOCK,
    PREF_HINTS,
    PREF_VISUALIZER,
    /* Layout */
    PREF_START,
    PREF_NOW_LAYOUT,
    PREF_PLAYLIST_SORT,
    /* Sound */
    PREF_EQUALIZER,
    PREF_EQ_60,             /* the five custom equalizer bands, in order */
    PREF_EQ_230,
    PREF_EQ_910,
    PREF_EQ_3600,
    PREF_EQ_14K,
    PREF_NORMALIZE,
    /* Playback */
    PREF_SLEEP,
    PREF_BATTERY,
    PREF_CPU,
    PREF_CONNECT,
    /* Controls */
    PREF_CONFIRM,
    PREF_LR,
    PREF_VOLUME_STEP,
    PREF_REMOTE,
    /* About */
    PREF_CHECK_UPDATES,
    /* Remembered between runs, changed elsewhere */
    PREF_SHUFFLE,
    PREF_REPEAT,
    PREF_COUNT
} pref_id;

/* Values of the choices the host acts on. */
enum { BACKGROUND_PLAIN, BACKGROUND_BLURRED_COVER, BACKGROUND_WALLPAPER };
enum { CLOCK_24H, CLOCK_12H, CLOCK_HIDDEN };
enum { SORT_SPOTIFY, SORT_NAME, SORT_SIZE };
#define EQ_BAND_MIN (-12)
#define EQ_BAND_STEP 2

typedef struct pref_def {
    const char *key;            /* in settings.ini */
    const char *label;          /* in Settings */
    int group;                  /* PREF_GROUP_* or PREF_HIDDEN */
    int count;                  /* number of values, 0 .. count-1 */
    const char *const *names;   /* shown in Settings */
    /* Written to settings.ini; NULL writes the value as a number. */
    const char *const *tokens;
    int fallback;               /* default */
    /* Numbers (names NULL): value v stands for min + v * step, shown with
     * unit after it and written to settings.ini as that number. */
    int min, step;
    const char *unit;
    int transient;              /* not saved: starts at the default every run */
} pref_def;

extern int prefs[PREF_COUNT];

const pref_def *pref_info(pref_id id);
void prefs_defaults(void);
/* One settings.ini line; returns 0 for a key that is not a pref. */
int prefs_read(const char *key, const char *value);
void prefs_write(FILE *f);
/* Moves a pref by dir (+1/-1): choices wrap around, numbers stop at the ends. */
void prefs_step(pref_id id, int dir);
/* The number a numeric pref stands for (min + value * step). */
int pref_number(pref_id id);
const char *pref_value_name(pref_id id);

/* Shows or hides a pref in Settings (Spotify Connect is only shown when
 * built in). */
void pref_set_shown(pref_id id, int shown);
int pref_shown(pref_id id);

#endif
