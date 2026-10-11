#ifndef SPOTIFY_DOWNLOAD_SAFETY_H
#define SPOTIFY_DOWNLOAD_SAFETY_H
#include <stdint.h>
#include <stddef.h>

/* HTTP decimal grammar, including optional surrounding SP/HTAB only.
 * UINT64_MAX is reserved by the transport as the until-close sentinel. */
static inline int download_parse_length(const char *text, uint64_t *out)
{
    uint64_t value = 0;
    while (*text == ' ' || *text == '\t') ++text;
    if (*text < '0' || *text > '9') return -1;
    do {
        unsigned int digit = (unsigned int)(*text++ - '0');
        if (value > (UINT64_MAX - digit) / 10u) return -1;
        value = value * 10u + digit;
    } while (*text >= '0' && *text <= '9');
    while (*text == ' ' || *text == '\t') ++text;
    if (*text || value == UINT64_MAX) return -1;
    *out = value;
    return 0;
}

/* Tail space is contiguous; total free heap can include unusable fragments.
 * Do not count the kernel partition's free memory as malloc space. */
static inline size_t download_budget(size_t heap_free, size_t heap_tail,
                                     size_t margin, size_t kernel_free,
                                     size_t kernel_max)
{
    if (kernel_free < 1024u * 1024u || kernel_max < 1024u * 1024u) return 0;
    size_t available = heap_free < heap_tail ? heap_free : heap_tail;
    return available > margin ? available - margin : 0;
}
#endif
