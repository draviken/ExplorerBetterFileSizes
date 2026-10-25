#include "feature_plan.h"

const struct feature_spec feature_specs[FEATURE_COUNT] = {
    [FEATURE_FOLDER_SIZES] = {
        L"folder sizes",
        SYMBOL_BIT(SYM_GETSIZE) | SYMBOL_BIT(SYM_PREPARE) | SYMBOL_BIT(SYM_DO),
        FEATURE_COUNT,
    },
    [FEATURE_FOLDER_PROPERTIES] = {
        L"folder size properties",
        SYMBOL_BIT(SYM_ITEM_GETVALUE) | SYMBOL_BIT(SYM_GETITEMPROPERTY),
        FEATURE_FOLDER_SIZES,
    },
    [FEATURE_MIXED_SORTING] = {
        L"mixed size sorting",
        SYMBOL_BIT(SYM_MAPCOLUMNTOSCID) | SYMBOL_BIT(SYM_GETDETAILSEX) | SYMBOL_BIT(SYM_COMPAREIDS),
        FEATURE_FOLDER_SIZES,
    },
    [FEATURE_FOLDER_VIEWS] = { L"folder size views", 0, FEATURE_FOLDER_SIZES },
    [FEATURE_KB] = { L"always KB", SYMBOL_BIT(SYM_GETFORMATFORDISPLAYFLAGS), FEATURE_COUNT },
};

const wchar_t *feature_status_name(enum feature_status status)
{
    switch (status) {
    case FEATURE_DISABLED:    return L"disabled by configuration";
    case FEATURE_WAITING:     return L"waiting for symbols";
    case FEATURE_UNAVAILABLE: return L"required symbol absent in this build";
    case FEATURE_BLOCKED:     return L"prerequisite feature is not active";
    case FEATURE_READY:       return L"ready to install";
    case FEATURE_ACTIVE:      return L"active";
    case FEATURE_FAILED:      return L"installation failed";
    case FEATURE_UNCERTAIN:   return L"rollback incomplete; restart the process";
    }
    return L"unknown";
}

void features_init(struct feature_state state[FEATURE_COUNT], int folder_sizes,
                   int mix_folders, int kb)
{
    const int requested[FEATURE_COUNT] = {
        [FEATURE_FOLDER_SIZES] = folder_sizes,
        [FEATURE_FOLDER_PROPERTIES] = folder_sizes,
        [FEATURE_MIXED_SORTING] = folder_sizes && mix_folders,
        [FEATURE_FOLDER_VIEWS] = folder_sizes,
        [FEATURE_KB] = kb,
    };
    for (int i = 0; i < FEATURE_COUNT; i++) {
        state[i].status = requested[i] ? FEATURE_WAITING : FEATURE_DISABLED;
        state[i].missing = 0;
    }
}

enum feature_status feature_resolve(struct feature_state state[FEATURE_COUNT],
                                   enum feature_id id, unsigned known, unsigned available)
{
    struct feature_state *s = &state[id];
    const struct feature_spec *spec = &feature_specs[id];

    if (s->status == FEATURE_DISABLED || s->status == FEATURE_ACTIVE ||
        s->status == FEATURE_FAILED || s->status == FEATURE_UNCERTAIN)
        return s->status;

    s->missing = spec->symbols & ~(known & available);
    if (spec->prerequisite != FEATURE_COUNT &&
        state[spec->prerequisite].status != FEATURE_ACTIVE)
        s->status = FEATURE_BLOCKED;
    else if (spec->symbols & known & ~available)
        s->status = FEATURE_UNAVAILABLE;
    else if (spec->symbols & ~known)
        s->status = FEATURE_WAITING;
    else
        s->status = FEATURE_READY;
    return s->status;
}

void feature_finish(struct feature_state state[FEATURE_COUNT], enum feature_id id,
                    enum hook_result result)
{
    switch (result) {
    case HOOK_ACTIVE:    state[id].status = FEATURE_ACTIVE; break;
    case HOOK_MISSING:   // An exported API, rather than a PDB symbol, may be missing.
    case HOOK_FAILED:    state[id].status = FEATURE_FAILED; break;
    case HOOK_UNCERTAIN: state[id].status = FEATURE_UNCERTAIN; break;
    }
}

int features_waiting(const struct feature_state state[FEATURE_COUNT])
{
    for (int i = 0; i < FEATURE_COUNT; i++) {
        if (state[i].status == FEATURE_WAITING)
            return 1;
    }
    return 0;
}
