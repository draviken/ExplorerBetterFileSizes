#pragma once

#include <windows.h>

// ebfs.c
extern HMODULE g_self;
void dbg(const WCHAR *fmt, ...);

// symbols.c
#include "symbol_ids.h"

// Fills validated rva[] and the known mask. A known bit with rva == 0 is a
// confirmed absent symbol; an unset bit needs discovery or cache repair.
// FALSE means the cache is incomplete; valid partial results remain usable.
BOOL symbols_load(HMODULE storage, DWORD rva[SYM_COUNT], unsigned *known);

const WCHAR *symbols_name(int id);

// Starts rundll32 <dll>,UpdateSymbols. Returns its process handle, or NULL.
HANDLE symbols_request_update(void);

// everything.c
BOOL everything_folder_size(const WCHAR *path, ULONGLONG *size);
