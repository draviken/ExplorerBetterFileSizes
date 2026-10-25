#pragma once

#include <stdint.h>
#include <wchar.h>

// A literal zero records a confirmed missing symbol. Empty, malformed and
// overflowing values mean the cache must be refreshed, not "symbol absent".
static inline int symbol_rva_parse(const wchar_t *text, uint32_t *value)
{
    uint32_t n = 0;
    size_t len = wcslen(text);

    if (!len || len > 8)
        return 0;
    for (size_t i = 0; i < len; i++) {
        wchar_t c = text[i];
        unsigned digit;
        if (c >= L'0' && c <= L'9')
            digit = c - L'0';
        else if (c >= L'a' && c <= L'f')
            digit = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F')
            digit = c - L'A' + 10;
        else
            return 0;
        n = (n << 4) | digit;
    }
    *value = n;
    return 1;
}
