#ifndef SPOTIFY_PREFS_H
#define SPOTIFY_PREFS_H
/* The user's choices in Settings: one table gives each its settings.ini key,
 * its label and its values, so the Settings screen, loading and saving all
 * come from the same place. */
#include <stdio.h>

/* The groups on the Settings screen's first level, in this order. Themes and
 * Categories are lists of their own; the others list prefs. */
enum {
    PREF_GROUP_THEMES,
    PREF_GROUP_APPEARANCE,
    PREF_GROUP_LAYOUT,
    PREF_GROUP_CATEGORIES,
    PREF_GROUP_SOUND,
    PREF_GROUP_PLAYBACK,
    PREF_GROUP_CONTROLS,
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
    /* Layout */
    PREF_START,
    PREF_NOW_LAYOUT,
    PREF_PLAYLIST_SORT,
    /* Sound */
    PREF_EQUALIZER,
    /* Playback */
    PREF_CONNECT,
    /* Remembered between runs, changed elsewhere */
    PREF_SHUFFLE,
    PREF_REPEAT,
    PREF_COUNT
} pref_id;

/* Values of the choices the host acts on. */
enum { BACKGROUND_PLAIN, BACKGROUND_BLURRED_COVER, BACKGROUND_WALLPAPER };
enum { CLOCK_24H, CLOCK_12H, CLOCK_HIDDEN };
enum { SORT_SPOTIFY, SORT_NAME, SORT_SIZE };

typedef struct pref_def {
    const char *key;            /* in settings.ini */
    const char *label;          /* in Settings */
    int group;                  /* PREF_GROUP_* or PREF_HIDDEN */
    int count;                  /* number of values, 0 .. count-1 */
    const char *const *names;   /* shown in Settings */
    /* Written to settings.ini; NULL writes the value as a number. */
    const char *const *tokens;
    int fallback;               /* default */
} pref_def;

extern int prefs[PREF_COUNT];

const pref_def *pref_info(pref_id id);
void prefs_defaults(void);
/* One settings.ini line; returns 0 for a key that is not a pref. */
int prefs_read(const char *key, const char *value);
void prefs_write(FILE *f);
/* Moves a pref by dir (+1/-1), wrapping around. */
void prefs_step(pref_id id, int dir);
const char *pref_value_name(pref_id id);

/* Shows or hides a pref in Settings (Spotify Connect is only shown when
 * built in). */
void pref_set_shown(pref_id id, int shown);
int pref_shown(pref_id id);

#endif
