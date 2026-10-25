#include "hooks.h"
#include "MinHook.h"

extern void dbg(const wchar_t *fmt, ...);

enum hook_result hooks_install(const wchar_t *feature, const struct hook_spec *spec, size_t count)
{
    size_t created = 0;
    size_t enabled = 0;
    MH_STATUS status;

    for (size_t i = 0; i < count; i++) {
        if (!spec[i].target) {
            dbg(L"%ls: required function missing", feature);
            return HOOK_MISSING;
        }
    }
    for (; created < count; created++) {
        status = MH_CreateHook(spec[created].target, spec[created].detour,
                               spec[created].original);
        if (status != MH_OK)
            goto failed;
    }
    // In particular, copy/delete guards must be active before _GetSize.
    for (size_t i = 0; i < count; i++) {
        status = MH_EnableHook(spec[i].target);
        if (status != MH_OK)
            goto failed;
        enabled++;
    }
    dbg(L"%ls: active", feature);
    return HOOK_ACTIVE;

failed:
    dbg(L"%ls: installation failed: %hs", feature, MH_StatusToString(status));
    // Reverse dependency order. If disabling a consumer fails, keep its
    // prerequisites and trampolines alive rather than leave dangling jumps.
    // Once any hook ran, an in-flight detour may still call any original in
    // this group. Disabling stops new entries but does not drain those calls.
    // Retain all of the group's trampolines until process exit in that case.
    while (created) {
        void *target = spec[--created].target;
        status = MH_DisableHook(target);
        if (status != MH_OK && status != MH_ERROR_DISABLED) {
            dbg(L"%ls: rollback stopped: %hs", feature, MH_StatusToString(status));
            return HOOK_UNCERTAIN;
        }
        if (!enabled) {
            status = MH_RemoveHook(target);
            if (status != MH_OK) {
                dbg(L"%ls: cannot remove hook: %hs", feature, MH_StatusToString(status));
                return HOOK_UNCERTAIN;
            }
        }
    }
    if (enabled)
        dbg(L"%ls: disabled; retaining trampolines until process exit", feature);
    return HOOK_FAILED;
}
