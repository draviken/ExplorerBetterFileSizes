#include "size_cache.h"

#include <stdlib.h>
#include <string.h>

struct entry {
    unsigned char *pidl; // child ID, NULL if empty
    size_t len;
    uint32_t hash;
    struct size_result result;
};

static _Thread_local struct cache {
    unsigned char *folder;
    size_t folder_len;
    uint32_t used;         // GetTickCount of the last lookup
    struct entry *tab;  // open addressing, cap is a power of 2
    size_t cap, count;
} *tl_cache;

static uint32_t hash_bytes(const unsigned char *p, size_t n)
{
    uint32_t h = 2166136261u; // FNV-1a

    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

static void cache_clear(struct cache *c)
{
    for (size_t i = 0; i < c->cap; i++)
        free(c->tab[i].pidl);
    if (c->tab)
        memset(c->tab, 0, c->cap * sizeof *c->tab);
    c->count = 0;
}

static void cache_free(struct cache *c)
{
    if (!c)
        return;
    cache_clear(c);
    free(c->tab);
    free(c->folder);
    free(c);
}

static struct entry *cache_probe(struct entry *tab, size_t cap, const unsigned char *key, size_t len, uint32_t hash)
{
    for (size_t i = hash & (cap - 1);; i = (i + 1) & (cap - 1)) {
        struct entry *e = &tab[i];
        if (!e->pidl || (e->hash == hash && e->len == len && !memcmp(e->pidl, key, len)))
            return e;
    }
}

static int cache_grow(struct cache *c)
{
    if (c->cap > SIZE_MAX / 2 / sizeof(struct entry))
        return 0;
    size_t cap = c->cap ? c->cap * 2 : 64;
    struct entry *tab = calloc(cap, sizeof *tab);

    if (!tab)
        return 0;
    for (size_t i = 0; i < c->cap; i++) {
        struct entry *e = &c->tab[i];
        if (e->pidl)
            *cache_probe(tab, cap, e->pidl, e->len, e->hash) = *e;
    }
    free(c->tab);
    c->tab = tab;
    c->cap = cap;
    return 1;
}

struct size_result *size_cache_lookup(const void *parent, size_t parent_len,
                                     const void *child, size_t child_len,
                                     uint32_t now, int *fresh)
{
    struct cache *c = tl_cache;
    struct entry *e;
    uint32_t hash;

    *fresh = 0;
    if (!child || !child_len)
        return NULL;
    if (!c && !(c = tl_cache = calloc(1, sizeof *c)))
        return NULL;
    if (!parent || !parent_len || parent_len != c->folder_len ||
        memcmp(parent, c->folder, parent_len) || (uint32_t)(now - c->used) > 1000) {
        cache_clear(c);
        free(c->folder);
        c->folder = NULL;
        c->folder_len = 0;
        if (parent && parent_len && (c->folder = malloc(parent_len))) {
            memcpy(c->folder, parent, parent_len);
            c->folder_len = parent_len;
        }
    }
    c->used = now;

    if (c->count >= c->cap / 2 && !cache_grow(c))
        return NULL;
    hash = hash_bytes(child, child_len);
    e = cache_probe(c->tab, c->cap, child, child_len, hash);
    *fresh = !e->pidl;
    if (*fresh) {
        if (!(e->pidl = malloc(child_len)))
            return NULL;
        memcpy(e->pidl, child, child_len);
        e->len = child_len;
        e->hash = hash;
        e->result.known = 0;
        c->count++;
    }
    return &e->result;
}

void size_cache_thread_cleanup(void)
{
    cache_free(tl_cache);
    tl_cache = NULL;
}
