#ifndef UI_UI_H
#define UI_UI_H
/* The XMB-style interface: categories across the top, a vertical list under
 * the selected one, a mini player at the bottom. Platform-independent: the
 * host (PSP app or PC preview) fills the lists and the now-playing state,
 * passes the buttons in each frame and handles the events that come out. */
#include <stdint.h>
#include "render.h"
#include "theme.h"

enum {
    UI_CAT_SEARCH,
    UI_CAT_RECENT,
    UI_CAT_LIKED,
    UI_CAT_PLAYLISTS,
    UI_CAT_NOW,
    UI_CAT_QUEUE,
    UI_CAT_SETTINGS,
    UI_CATS
};

/* Buttons, as a bit mask of what is held this frame. */
enum {
    UI_BTN_UP = 1 << 0,
    UI_BTN_DOWN = 1 << 1,
    UI_BTN_LEFT = 1 << 2,
    UI_BTN_RIGHT = 1 << 3,
    UI_BTN_CROSS = 1 << 4,
    UI_BTN_CIRCLE = 1 << 5,
    UI_BTN_SQUARE = 1 << 6,
    UI_BTN_TRIANGLE = 1 << 7,
    UI_BTN_L = 1 << 8,
    UI_BTN_R = 1 << 9,
    UI_BTN_START = 1 << 10,
    UI_BTN_SELECT = 1 << 11
};

typedef struct ui_row {
    char title[128];
    char subtitle[96];
    uint32_t color;     /* cover placeholder while there is no image */
    r_tex *cover;       /* optional, owned by the host */
    uint8_t round;      /* artists: circular cover */
    uint8_t playing;    /* the row of the track that is playing */
    unsigned icon;      /* optional: an icon (ICON_*) on the color instead of a cover */
} ui_row;

typedef struct ui_list {
    char header[96];    /* shown above an opened list, e.g. the playlist name */
    ui_row *rows;       /* owned by the host */
    int count;
    int loading;
    int movable;        /* SELECT picks a row up and ↑/↓ move it (the queue) */
    int adjustable;     /* ←/→ change the selected row's value (Settings) */
    int plain;          /* text rows without covers (settings, options) */
} ui_list;

typedef struct ui_now {
    int active, paused, loading;
    int shuffle;        /* 1: queue plays in random order */
    int repeat;         /* 0 off, 1 the whole queue, 2 this track */
    int seeking;        /* pos_ms is a seek preview while L/R is held */
    int remote;         /* controlled from another device (Spotify Connect) */
    char title[128], artist[128], album[128], next[128];
    unsigned pos_ms, dur_ms;
    uint32_t color;
    r_tex *cover;
    float spectrum[16]; /* band levels 0..1 for the visualizer, low to high */
} ui_now;

typedef enum {
    UI_EV_OPEN,         /* category, level, index: ✕ on a row */
    UI_EV_BACK,         /* category: ○ left an opened list */
    UI_EV_PAUSE,
    UI_EV_PREV,
    UI_EV_NEXT,
    UI_EV_SEEK,         /* value: -1 or +1 while L/R is held; 0 when released */
    UI_EV_VOLUME,       /* value: new volume 0-100 */
    UI_EV_SEARCH,       /* □ in Search: open the keyboard */
    UI_EV_OPTIONS,      /* category, level, index: △ */
    UI_EV_SHUFFLE,      /* △ on Now Playing */
    UI_EV_REPEAT,       /* ✕ on Now Playing */
    UI_EV_MENU,         /* value: chosen menu item; category, level, index: its row */
    UI_EV_MOVE,         /* value: -1 or +1; index: the row before it moved */
    UI_EV_ADJUST,       /* value: -1 or +1: ←/→ or ✕ (+1) on a row of an adjustable list */
    UI_EV_QUIT
} ui_event_type;

typedef struct ui_event {
    ui_event_type type;
    int category, level, index, value;
} ui_event;

#define UI_MAX_EVENTS 8
#define UI_MENU_ITEMS 6

/* A small menu over the screen (△ on a row), opened by the host. */
typedef struct ui_menu {
    int open, count, sel;
    int category, level, index;     /* the row it belongs to */
    char items[UI_MENU_ITEMS][48];
} ui_menu;

/* How the interface looks, chosen by the host from the user's settings.
 * All zero is the default look. */
enum { UI_DENSITY_COMFORTABLE, UI_DENSITY_COMPACT, UI_DENSITY_TEXT };
enum { UI_TEXT_NORMAL, UI_TEXT_SMALL, UI_TEXT_LARGE };
enum { UI_ANIM_SMOOTH, UI_ANIM_FAST, UI_ANIM_OFF };
enum { UI_NOW_CLASSIC, UI_NOW_CENTERED, UI_NOW_TEXT };
enum { UI_LR_BOTH, UI_LR_SEEK, UI_LR_SKIP };

typedef struct ui_style {
    int density;        /* UI_DENSITY_*: list rows */
    int text_size;      /* UI_TEXT_*: list titles */
    int anim;           /* UI_ANIM_* */
    int hide_hints;     /* no button hints in the top bar */
    int now_layout;     /* UI_NOW_*: the Now Playing screen */
    int swap_confirm;   /* ○ confirms and ✕ goes back (Japanese layout) */
    int lr_mode;        /* UI_LR_*: a tap changes track and holding seeks, or one of them */
    int volume_step;    /* percent per ↑/↓ on Now Playing; 0 = 5 */
    int visualizer;     /* bars from now.spectrum along the bottom of Now Playing */
    /* Drawn stretched over the whole screen under everything (a wallpaper
     * or a blurred cover), then covered by the background color at
     * backdrop_dim (0..1). Owned by the host; NULL for a plain background. */
    r_tex *backdrop;
    float backdrop_dim;
} ui_style;

typedef struct ui_state {
    /* The host sets theme_target; theme eases towards it, so a theme or
     * cover-color change fades instead of jumping. Set both to switch at once. */
    ui_theme theme, theme_target;
    /* Level 0 is the category's own list; level 1 an opened item
     * (a playlist's tracks). The host fills both. */
    ui_list lists[UI_CATS][2];
    int level[UI_CATS];
    int sel[UI_CATS][2];
    ui_now now;
    ui_style style;
    int volume;
    char clock[12];         /* empty: no clock */
    char search_query[128];
    const char *search_hint;    /* shown while the query is empty */

    int cat;
    /* The categories shown, left to right (see ui_set_categories). */
    int cat_order[UI_CATS], cat_count;
    ui_menu menu;
    int moving;             /* a row of a movable list is picked up */
    char toast[64];         /* a short note at the bottom, e.g. "Added to queue" */
    float toast_left;
    ui_event events[UI_MAX_EVENTS];
    int event_count;

    /* Internal: animation and input state. */
    float cat_pos, scroll[UI_CATS][2], level_pos[UI_CATS], menu_pos;
    unsigned buttons;
    float repeat_at, lr_held, lr_seek_at;
} ui_state;

int ui_init(ui_state *ui);
/* Opens a menu for the row the last UI_EV_OPTIONS came from. */
void ui_menu_open(ui_state *ui, const char *const *items, int count);
/* Shows a short note for about two seconds. */
void ui_toast(ui_state *ui, const char *text);
/* Shows only these categories, in this order; Settings is always kept (at
 * the end if missing). If the current category is hidden, the first shown
 * one becomes current. */
void ui_set_categories(ui_state *ui, const int *order, int count);
/* Opens level 1 of a category (after the host filled lists[cat][1]). */
void ui_open_level(ui_state *ui, int cat);
/* Reads the buttons held this frame, emits events, advances animations. */
void ui_update(ui_state *ui, unsigned buttons, float dt);
void ui_draw(const ui_state *ui);
/* Bakes the font sizes the current style uses, so changing a setting does
 * not stall a frame later; call between the platform's begin/end frame. */
void ui_prebake(const ui_state *ui);

#endif
