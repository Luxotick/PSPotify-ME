#ifndef SPOTIFY_MEMORY_H
#define SPOTIFY_MEMORY_H
#include <stddef.h>
typedef struct {
    size_t kernel_free, kernel_max, heap_free, heap_tail, heap_capacity;
} memory_snapshot;
extern int spotify_device_model;
memory_snapshot memory_read(void);
void memory_log(const char *stage);
size_t memory_track_budget(memory_snapshot state);
/* Only observes libcglue's actual heap allocation; delegates every request. */
int __wrap_sceKernelAllocPartitionMemory(int partition, const char *name,
                                       int type, unsigned int size, void *address);
#endif
