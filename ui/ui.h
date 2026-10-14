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
    UI_CAT_LIKED,
    UI_CAT_PLAYLISTS,
    UI_CAT_NOW,
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
    UI_BTN_START = 1 << 10
};

typedef struct ui_row {
    char title[128];
    char subtitle[96];
    uint32_t color;     /* cover placeholder while there is no image */
    r_tex *cover;       /* optional, owned by the host */
    uint8_t round;      /* artists: circular cover */
    uint8_t playing;    /* the row of the track that is playing */
} ui_row;

typedef struct ui_list {
    char header[96];    /* shown above an opened list, e.g. the playlist name */
    ui_row *rows;       /* owned by the host */
    int count;
    int loading;
} ui_list;

typedef struct ui_now {
    int active, paused, loading;
    char title[128], artist[128], album[128], next[128];
    unsigned pos_ms, dur_ms;
    uint32_t color;
    r_tex *cover;
} ui_now;

typedef enum {
    UI_EV_OPEN,         /* category, level, index: ✕ on a row */
    UI_EV_BACK,         /* category: ○ left an opened list */
    UI_EV_PAUSE,
    UI_EV_PREV,
    UI_EV_NEXT,
    UI_EV_SEEK,         /* value: -1 or +1, repeated while L/R is held */
    UI_EV_VOLUME,       /* value: new volume 0-100 */
    UI_EV_SEARCH,       /* □ in Search: open the keyboard */
    UI_EV_OPTIONS,      /* category, level, index: △ */
    UI_EV_QUIT
} ui_event_type;

typedef struct ui_event {
    ui_event_type type;
    int category, level, index, value;
} ui_event;

#define UI_MAX_EVENTS 8

typedef struct ui_state {
    ui_theme theme;
    /* Level 0 is the category's own list; level 1 an opened item
     * (a playlist's tracks). The host fills both. */
    ui_list lists[UI_CATS][2];
    int level[UI_CATS];
    int sel[UI_CATS][2];
    ui_now now;
    int volume;
    char clock[8];
    char search_query[128];

    int cat;
    ui_event events[UI_MAX_EVENTS];
    int event_count;

    /* Internal: animation and input state. */
    float cat_pos, scroll[UI_CATS][2], level_pos[UI_CATS];
    unsigned buttons;
    float repeat_at, lr_held, lr_seek_at;
} ui_state;

int ui_init(ui_state *ui);
/* Opens level 1 of a category (after the host filled lists[cat][1]). */
void ui_open_level(ui_state *ui, int cat);
/* Reads the buttons held this frame, emits events, advances animations. */
void ui_update(ui_state *ui, unsigned buttons, float dt);
void ui_draw(const ui_state *ui);

#endif
