#ifndef SPOTIFY_LOG_H
#define SPOTIFY_LOG_H

/* Prints to the debug screen and, in debug mode, appends the same text to
 * spotify.log in the application directory so a run can be read back over
 * USB. Debug mode is off by default (no Memory Stick writes); it is turned
 * on with "debug = on" in settings.ini. */

/* dir: the application directory. The log path is absolute because the
 * current directory set with sceIoChdir does not carry over to threads.
 * Reads the debug switch from dir/settings.ini. */
void log_init(const char *dir);
/* 1 when spotify.log is being written. */
int log_enabled(void);
int log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* 0: file only (the UI owns the screen); 1: also the debug screen. */
void log_set_echo(int on);
/* 1: collect lines in RAM and write them in batches (every 2 s, or when
 * 6 KB are waiting) instead of one Memory Stick write per line. Start-up
 * keeps writing line by line, so a crash there still leaves a full log. */
void log_set_buffered(int on);
/* Writes out buffered lines if they have waited long enough; call often. */
void log_tick(void);
/* Writes out buffered lines now. */
void log_flush(void);

#endif /* SPOTIFY_LOG_H */
