#pragma once

#include "symbol_ids.h"
#include "hooks.h"

// Dependency order: a prerequisite is always listed before its consumers.
enum feature_id {
    FEATURE_FOLDER_SIZES,
    FEATURE_FOLDER_PROPERTIES,
    FEATURE_MIXED_SORTING,
    FEATURE_FOLDER_VIEWS,
    FEATURE_KB,
    FEATURE_COUNT
};

enum feature_status {
    FEATURE_DISABLED,
    FEATURE_WAITING,
    FEATURE_UNAVAILABLE,
    FEATURE_BLOCKED,
    FEATURE_READY,
    FEATURE_ACTIVE,
    FEATURE_FAILED,
    FEATURE_UNCERTAIN
};

struct feature_spec {
    const wchar_t *name;
    unsigned symbols;
    enum feature_id prerequisite; // FEATURE_COUNT means independent
};

struct feature_state {
    enum feature_status status;
    unsigned missing; // unknown or confirmed absent symbols required by this feature
};

extern const struct feature_spec feature_specs[FEATURE_COUNT];
const wchar_t *feature_status_name(enum feature_status status);
void features_init(struct feature_state state[FEATURE_COUNT], int folder_sizes,
                   int mix_folders, int kb);
// Revisit pending features after a cache update, without reinstalling active
// hooks or retrying an installation whose rollback may have failed.
enum feature_status feature_resolve(struct feature_state state[FEATURE_COUNT],
                                   enum feature_id id, unsigned known, unsigned available);
void feature_finish(struct feature_state state[FEATURE_COUNT], enum feature_id id,
                    enum hook_result result);
int features_waiting(const struct feature_state state[FEATURE_COUNT]);
