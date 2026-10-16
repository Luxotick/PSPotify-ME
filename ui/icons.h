#ifndef UI_ICONS_H
#define UI_ICONS_H
/* Lucide icon font codepoints (lucide-static 1.48.0, font/codepoints.json). */
#define ICON_SEARCH         0xe151u
#define ICON_HEART          0xe0f2u
#define ICON_LIST_MUSIC     0xe2e0u
#define ICON_MUSIC          0xe122u
#define ICON_SLIDERS        0xe29au
#define ICON_PLAY           0xe13cu
#define ICON_PAUSE          0xe12eu
#define ICON_SKIP_BACK      0xe15fu
#define ICON_SKIP_FORWARD   0xe160u
#define ICON_VOLUME         0xe1abu
#define ICON_VOLUME_MUTE    0xe1acu
#define ICON_CROSS          0xe1b2u
#define ICON_CIRCLE         0xe076u
#define ICON_TRIANGLE       0xe192u
#define ICON_SQUARE         0xe167u
#define ICON_USER           0xe19fu
#define ICON_LOADER         0xe10au
#define ICON_SHUFFLE        0xe15eu
#define ICON_REPEAT         0xe146u
#define ICON_REPEAT_ONE     0xe1fdu
#define ICON_HISTORY        0xe1f5u
#define ICON_CAST           0xe066u
#define ICON_QUEUE          0xe1d1u
#define ICON_PALETTE        0xe1ddu
#define ICON_SPARKLES       0xe412u
#define ICON_LAYOUT         0xe1c1u
#define ICON_REORDER        0xe37du
#define ICON_DISC           0xe494u
#define ICON_GAMEPAD        0xe0dfu
#define ICON_PIN            0xe259u

/* Every icon the UI bakes; keep in sync with the defines above. */
#define ICON_LIST ICON_SEARCH, ICON_HEART, ICON_LIST_MUSIC, ICON_MUSIC, ICON_SLIDERS, \
    ICON_PLAY, ICON_PAUSE, ICON_SKIP_BACK, ICON_SKIP_FORWARD, ICON_VOLUME,           \
    ICON_VOLUME_MUTE, ICON_CROSS, ICON_CIRCLE, ICON_TRIANGLE, ICON_SQUARE, ICON_USER, \
    ICON_LOADER, ICON_SHUFFLE, ICON_REPEAT, ICON_REPEAT_ONE, ICON_HISTORY, ICON_CAST, \
    ICON_QUEUE, ICON_PALETTE, ICON_SPARKLES, ICON_LAYOUT, ICON_REORDER, ICON_DISC,     \
    ICON_GAMEPAD, ICON_PIN

#endif
