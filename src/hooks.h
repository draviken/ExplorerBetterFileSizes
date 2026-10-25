#pragma once

#include <stddef.h>

struct hook_spec {
    void *target;
    void *detour;
    void *original;
};

enum hook_result { HOOK_FAILED, HOOK_ACTIVE, HOOK_MISSING, HOOK_UNCERTAIN };

// Callers serialize installation. Put prerequisites before their consumers.
// Creates the entire group before enabling anything; failures roll back this
// group only. Once any hook has been enabled, rollback retains the disabled
// group's trampolines for in-flight calls. Do not retry failed groups in this
// process. Other features' hooks are left alone.
enum hook_result hooks_install(const wchar_t *feature, const struct hook_spec *spec, size_t count);
