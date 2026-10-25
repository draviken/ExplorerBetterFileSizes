#pragma once

#include <stddef.h>
#include <stdint.h>

struct size_result {
    int known;
    uint64_t size;
};

// Per-thread, including negative results. Invalidated on parent change or
// after one second without lookups (not a per-entry TTL). The returned entry
// is borrowed until the next lookup/cleanup; callers must prevent reentry.
struct size_result *size_cache_lookup(const void *parent, size_t parent_len,
                                     const void *child, size_t child_len,
                                     uint32_t now, int *fresh);
void size_cache_thread_cleanup(void);
