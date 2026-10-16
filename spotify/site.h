#ifndef SPOTIFY_SITE_H
#define SPOTIFY_SITE_H
/* The theme gallery and app updates, both from the project's GitHub Pages
 * site (built by scripts/build_site.py). One host keeps it to one TLS chain
 * the PSP already trusts (ISRG Root X1). All calls block: run them on a
 * worker thread. */
#include <stdint.h>

#define SITE_URL "https://luxotick.github.io/PSPotify-ME/"

#define SITE_THEME_FILES 4

typedef struct site_theme {
    char folder[32];
    uint32_t accent;                    /* 0xAARRGGBB, for the swatch */
    char files[SITE_THEME_FILES][48];
    int file_count;
    int has_wallpaper;
} site_theme;

/* Reads the gallery's list (themes/psp.txt). Returns the number of themes
 * (at most max) or < 0. */
int site_fetch_themes(site_theme *out, int max);
/* Downloads a theme's files into themes_dir/<folder>/, each through a .part
 * file so a failed download leaves no half file. Returns 0 or < 0. */
int site_download_theme(const site_theme *t, const char *themes_dir);

typedef struct site_update {
    char version[16];                   /* "0.3.1" */
    char tag[32];                       /* "v0.3.1-alpha" */
    unsigned int size;
    uint8_t sha256[32];
    char eboot[64];                     /* path under SITE_URL */
} site_update;

/* Reads update.txt. Returns 1 when its version is newer than ours, 0 when
 * not, < 0 on error. */
int site_check_update(site_update *u);
/* Downloads the new EBOOT to app_dir/EBOOT.NEW, checks its size and
 * SHA-256, then swaps it in, keeping the old one as EBOOT.OLD. *progress
 * goes 0..100 while downloading. Returns 0 or < 0; on failure the running
 * EBOOT.PBP is left as it was. */
int site_install_update(const site_update *u, const char *app_dir, volatile int *progress);

/* < 0, 0 or > 0 as version a is older, the same or newer than b ("1.2.3"). */
int version_compare(const char *a, const char *b);

#endif
