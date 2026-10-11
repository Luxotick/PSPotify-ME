#include "memory.h"
#include "download_safety.h"
#include "log.h"
#include <stdint.h>
#include <malloc.h>
#include <pspsysmem.h>
#include <pspmodulemgr.h>
#include <string.h>

int spotify_device_model = -1;
static uintptr_t heap_begin, heap_end;
static size_t heap_capacity;
void *_sbrk(ptrdiff_t increment);
int __real_sceKernelAllocPartitionMemory(int partition, const char *name,
                                       int type, unsigned int size, void *address);

int __wrap_sceKernelAllocPartitionMemory(int partition, const char *name,
                                       int type, unsigned int size, void *address)
{
    /* Observe the exact successful libcglue block request, not an estimate
     * from kernel free-memory deltas. Every allocation is forwarded intact. */
    int result = __real_sceKernelAllocPartitionMemory(partition, name, type, size, address);
    if (result > 0 && partition == PSP_MEMORY_PARTITION_USER && type == PSP_SMEM_Low &&
        name && strcmp(name, "heap_block") == 0 && !heap_begin) {
        uintptr_t start = (uintptr_t)sceKernelGetBlockHeadAddr(result);
        if (start && size && start <= UINTPTR_MAX - size) {
            heap_begin = start;
            heap_capacity = size;
            heap_end = start + size;
        }
    }
    return result;
}

memory_snapshot memory_read(void)
{
    memory_snapshot state = {0};
    /* Hold the allocator's recursive lock across the two observations. */
    __malloc_lock(_REENT);
    struct mallinfo info = mallinfo();
    uintptr_t current = (uintptr_t)_sbrk(0);
    if (heap_begin && current >= heap_begin && current < heap_end) {
        size_t unused = heap_end - current;
        /* libcglue's upper bound is exclusive; newlib grows by pages. */
        unused = unused > 4096u ? unused - 4096u : 0;
        state.heap_capacity = heap_capacity;
        if (info.arena <= heap_capacity && info.fordblks <= info.arena &&
            info.fordblks <= SIZE_MAX - unused && info.keepcost <= info.fordblks) {
            state.heap_free = info.fordblks + unused;
            state.heap_tail = info.keepcost + unused;
        }
    }
    __malloc_unlock(_REENT);
    state.kernel_free = sceKernelTotalFreeMemSize();
    state.kernel_max = sceKernelMaxFreeMemSize();
    return state;
}

void memory_log(const char *stage)
{
    memory_snapshot state = memory_read();
    log_printf("MEM %s: total_free=%u max_block=%u heap_capacity=%u heap_free=%u heap_tail=%u reserve=8388608\n",
               stage, (unsigned int)state.kernel_free, (unsigned int)state.kernel_max,
               (unsigned int)state.heap_capacity, (unsigned int)state.heap_free,
               (unsigned int)state.heap_tail);
}

size_t memory_track_budget(memory_snapshot state)
{
    /* Model 0 only: 3.5 MiB for bounded 512x512, <=3-component JPEG
     * planes/progressive coefficients/RGBA, 256 KiB for concurrent TLS,
     * and 256 KiB for allocator/runtime headroom.
     * Vorbis (384 KiB), ME stack and PCM are already static allocations.
     * Kernel guard leaves >=1 MiB for the 64 KiB fetch stack/audio/overhead. */
    return download_budget(state.heap_free, state.heap_tail, 4u * 1024u * 1024u,
                           state.kernel_free, state.kernel_max);
}
