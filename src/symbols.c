#include "ebfs.h"
#include "symbol_cache.h"

#include <winhttp.h>
#include <dbghelp.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const struct {
    const WCHAR *key;   // in symbols.ini
    const WCHAR *name;  // decorated, in the PDB
    const WCHAR *alt;   // older builds' name, or NULL
} syms[SYM_COUNT] = {
    [SYM_GETSIZE] = { L"CFSFolder::_GetSize",
        L"?_GetSize@CFSFolder@@KAJPEAV1@PEFBU_ITEMID_CHILD@@PEFBUIDFOLDER@@PEAUtagPROPVARIANT@@@Z",
        NULL },
    [SYM_PREPARE] = { L"CRecursiveFolderOperation::Prepare",
        L"?Prepare@CRecursiveFolderOperation@@QEAAJXZ",
        NULL },
    [SYM_DO] = { L"CRecursiveFolderOperation::Do",
        L"?Do@CRecursiveFolderOperation@@UEAAJXZ",
        NULL },
    [SYM_ITEM_GETVALUE] = { L"CDefCollection::Item_GetValue",
        L"?Item_GetValue@CDefCollection@@UEAAJPEBUtagITEMKEY@@W4VALUE_ACCESS_MODE@@AEBU_tagpropertykey@@PEAUtagPROPVARIANT@@PEAW4VALUE_STATE@@@Z",
        L"?Item_GetValue@CDefCollection@@UEAAJPEBUtagITEMKEY@@W4tagACCESS_MODE@@AEBU_tagpropertykey@@PEAUtagPROPVARIANT@@PEAW4tagVALUE_STATE@@@Z" },
    [SYM_GETITEMPROPERTY] = { L"CDefCollection::_GetItemProperty",
        L"?_GetItemProperty@CDefCollection@@AEAAJPEAUIItemStore@@PEBUtagITEMKEY@@W4GIPTYPE@@AEBU_tagpropertykey@@PEAHPEAUtagPROPVARIANT@@@Z",
        NULL },
    [SYM_MAPCOLUMNTOSCID] = { L"CFSFolder::MapColumnToSCID",
        L"?MapColumnToSCID@CFSFolder@@UEAAJIPEAU_tagpropertykey@@@Z",
        NULL },
    [SYM_GETDETAILSEX] = { L"CFSFolder::GetDetailsEx",
        L"?GetDetailsEx@CFSFolder@@UEAAJPEFBU_ITEMID_CHILD@@PEBU_tagpropertykey@@PEAUtagVARIANT@@@Z",
        NULL },
    [SYM_COMPAREIDS] = { L"CFSFolder::CompareIDs",
        L"?CompareIDs@CFSFolder@@UEAAJ_JPEFBU_ITEMIDLIST_RELATIVE@@1@Z",
        NULL },
    [SYM_GETFORMATFORDISPLAYFLAGS] = { L"CFSFolder::GetFormatForDisplayFlags",
        L"?GetFormatForDisplayFlags@CFSFolder@@UEAAJAEBU_tagpropertykey@@PEAW4PROPDESC_FORMAT_FLAGS@@@Z",
        NULL },
};

const WCHAR *symbols_name(int id)
{
    return syms[id].key;
}

// Typed GetProcAddress, for DLLs only UpdateSymbols loads.
#define IMPORT(mod, fn) \
    __typeof__(&fn) p##fn = (__typeof__(&fn))(void (*)(void))GetProcAddress(mod, #fn)

static const IMAGE_NT_HEADERS *nt_headers(HMODULE mod)
{
    const BYTE *base = (const BYTE *)mod;
    return (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
}

// "<GUID><age>" as symbol servers spell it, and the PDB file name.
static BOOL pdb_id(HMODULE mod, WCHAR id[64], WCHAR pdb[MAX_PATH])
{
    const BYTE *base = (const BYTE *)mod;
    const IMAGE_DATA_DIRECTORY *dir =
        &nt_headers(mod)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    const IMAGE_DEBUG_DIRECTORY *d = (const void *)(base + dir->VirtualAddress);
    const struct {
        DWORD signature; // "RSDS"
        GUID guid;
        DWORD age;
        char name[];
    } *cv;
    const char *name;

    for (DWORD i = 0; i < dir->Size / sizeof *d; i++) {
        if (d[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !d[i].AddressOfRawData)
            continue;
        cv = (const void *)(base + d[i].AddressOfRawData);
        if (cv->signature != 0x53445352)
            continue;

        swprintf(id, 64, L"%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX",
                 cv->guid.Data1, cv->guid.Data2, cv->guid.Data3,
                 cv->guid.Data4[0], cv->guid.Data4[1], cv->guid.Data4[2], cv->guid.Data4[3],
                 cv->guid.Data4[4], cv->guid.Data4[5], cv->guid.Data4[6], cv->guid.Data4[7],
                 cv->age);
        name = strrchr(cv->name, '\\');
        name = name ? name + 1 : cv->name;
        return MultiByteToWideChar(CP_UTF8, 0, name, -1, pdb, MAX_PATH) > 1;
    }
    return FALSE;
}

// Is rva a function entry: in an executable section and the start of a
// .pdata entry? Keeps a bad cache from patching random code in Explorer.
static BOOL rva_valid(HMODULE mod, DWORD rva)
{
    const IMAGE_NT_HEADERS *nt = nt_headers(mod);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    const IMAGE_DATA_DIRECTORY *dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const RUNTIME_FUNCTION *fn = (const void *)((const BYTE *)mod + dir->VirtualAddress);
    size_t n = dir->Size / sizeof *fn, lo = 0, hi = n;
    BOOL exec = FALSE;

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (rva >= sec[i].VirtualAddress && rva - sec[i].VirtualAddress < sec[i].Misc.VirtualSize)
            exec = (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    }
    if (!exec)
        return FALSE;

    while (lo < hi) { // .pdata is sorted
        size_t mid = lo + (hi - lo) / 2;
        if (fn[mid].BeginAddress < rva)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && fn[lo].BeginAddress == rva;
}

// %LOCALAPPDATA%\ExplorerBetterFileSizes[\file]
static BOOL cache_path(WCHAR path[MAX_PATH], const WCHAR *file)
{
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);

    if (!n || n >= MAX_PATH)
        return FALSE;
    return !wcscat_s(path, MAX_PATH, L"\\ExplorerBetterFileSizes") &&
           (!file || (!wcscat_s(path, MAX_PATH, L"\\") && !wcscat_s(path, MAX_PATH, file)));
}

// TRUE if every symbol has a valid address or an explicit "not present" (0).
// Completeness describes the cache schema, not feature availability. In
// particular the optional always-KB symbol can be 0 on older Windows builds.
static BOOL ini_read(const WCHAR *file, const WCHAR *id, HMODULE mod, DWORD rva[SYM_COUNT], unsigned *known)
{
    WCHAR v[32];
    *known = 0;

    for (int i = 0; i < SYM_COUNT; i++) {
        uint32_t parsed;
        rva[i] = 0;
        if (!GetPrivateProfileStringW(id, syms[i].key, L"", v, 32, file))
            continue;
        if (!symbol_rva_parse(v, &parsed)) {
            dbg(L"symbols: malformed %ls=%ls, refresh required", syms[i].key, v);
            continue;
        }
        rva[i] = parsed;
        if (rva[i] && !rva_valid(mod, rva[i])) {
            dbg(L"symbols: %ls=%ls is not a function start, ignored", syms[i].key, v);
            rva[i] = 0;
            continue;
        }
        *known |= SYMBOL_BIT(i);
    }
    return *known == SYMBOLS_ALL;
}

BOOL symbols_load(HMODULE storage, DWORD rva[SYM_COUNT], unsigned *known)
{
    WCHAR id[64], pdb[MAX_PATH], file[MAX_PATH];

    memset(rva, 0, SYM_COUNT * sizeof *rva);
    *known = 0;
    if (!pdb_id(storage, id, pdb)) {
        dbg(L"symbols: windows.storage.dll has no PDB id");
        return FALSE;
    }
    if (!cache_path(file, L"symbols.ini") || !ini_read(file, id, storage, rva, known)) {
        dbg(L"symbols: no complete entry for windows.storage.dll %ls", id);
        return FALSE;
    }
    return TRUE;
}

HANDLE symbols_request_update(void)
{
    WCHAR exe[MAX_PATH], dll[MAX_PATH], cmd[3 * MAX_PATH];
    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;

    GetSystemDirectoryW(exe, MAX_PATH);
    wcscat_s(exe, MAX_PATH, L"\\rundll32.exe");
    GetModuleFileNameW(g_self, dll, MAX_PATH);
    swprintf(cmd, 3 * MAX_PATH, L"\"%ls\" \"%ls\",UpdateSymbols", exe, dll);

    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        NULL, NULL, &si, &pi))
        return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// ---- Only in rundll32 from here on

static BOOL http_download(const WCHAR *url, const WCHAR *dest)
{
    HMODULE wh = LoadLibraryExW(L"winhttp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    IMPORT(wh, WinHttpCrackUrl);
    IMPORT(wh, WinHttpOpen);
    IMPORT(wh, WinHttpSetTimeouts);
    IMPORT(wh, WinHttpConnect);
    IMPORT(wh, WinHttpOpenRequest);
    IMPORT(wh, WinHttpSendRequest);
    IMPORT(wh, WinHttpReceiveResponse);
    IMPORT(wh, WinHttpQueryHeaders);
    IMPORT(wh, WinHttpReadData);
    IMPORT(wh, WinHttpCloseHandle);
    WCHAR host[256], path[2048];
    URL_COMPONENTS uc = {
        .dwStructSize = sizeof uc,
        .lpszHostName = host, .dwHostNameLength = 256,
        .lpszUrlPath = path, .dwUrlPathLength = 2048,
    };
    HINTERNET ses = NULL, con = NULL, req = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD status = 0, len = sizeof status, n, written;
    static BYTE buf[64 * 1024];
    BOOL ok = FALSE;

    if (!pWinHttpCrackUrl || !pWinHttpOpen || !pWinHttpSetTimeouts || !pWinHttpConnect ||
        !pWinHttpOpenRequest || !pWinHttpSendRequest || !pWinHttpReceiveResponse ||
        !pWinHttpQueryHeaders || !pWinHttpReadData || !pWinHttpCloseHandle)
        goto out;
    if (!pWinHttpCrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        dbg(L"symbols: not an https URL: %ls", url);
        goto out;
    }

    ses = pWinHttpOpen(L"ExplorerBetterFileSizes", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses || !pWinHttpSetTimeouts(ses, 10000, 10000, 30000, 60000))
        goto out;
    con = pWinHttpConnect(ses, host, uc.nPort, 0);
    req = con ? pWinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (!req ||
        !pWinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !pWinHttpReceiveResponse(req, NULL) ||
        !pWinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX))
        goto out;
    if (status != 200) {
        dbg(L"symbols: HTTP %lu for %ls", status, url);
        goto out;
    }

    file = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        goto out;
    for (;;) {
        if (!pWinHttpReadData(req, buf, sizeof buf, &n))
            goto out;
        if (!n)
            break;
        if (!WriteFile(file, buf, n, &written, NULL) || written != n)
            goto out;
    }
    ok = TRUE;
out:
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        if (!ok)
            DeleteFileW(dest);
    }
    if (req) pWinHttpCloseHandle(req);
    if (con) pWinHttpCloseHandle(con);
    if (ses) pWinHttpCloseHandle(ses);
    if (wh) FreeLibrary(wh);
    return ok;
}

// Succeeds if the PDB loaded, even with functions missing: saving that keeps
// Explorer from asking for this build again.
static BOOL from_pdb(HMODULE mod, const WCHAR *id, const WCHAR *pdb, DWORD rva[SYM_COUNT])
{
    HMODULE dh = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    IMPORT(dh, SymSetOptions);
    IMPORT(dh, SymInitializeW);
    IMPORT(dh, SymLoadModuleExW);
    IMPORT(dh, SymFromNameW);
    IMPORT(dh, SymCleanup);
    HANDLE proc = (HANDLE)(ULONG_PTR)0xEBF5; // any unique value; no real process
    WCHAR dir[MAX_PATH] = L"", file[MAX_PATH] = L"", url[1024], image[MAX_PATH];
    union {
        SYMBOL_INFOW info;
        BYTE space[sizeof(SYMBOL_INFOW) + 1024 * sizeof(WCHAR)];
    } sym;
    DWORD64 base;
    BOOL ok = FALSE;

    memset(rva, 0, SYM_COUNT * sizeof *rva);
    if (!pSymSetOptions || !pSymInitializeW || !pSymLoadModuleExW || !pSymFromNameW || !pSymCleanup)
        goto out;

    GetTempPathW(MAX_PATH, dir);
    wcscat_s(dir, MAX_PATH, L"ebfs-");
    wcscat_s(dir, MAX_PATH, id);
    CreateDirectoryW(dir, NULL);
    swprintf(file, MAX_PATH, L"%ls\\%ls", dir, pdb);
    swprintf(url, 1024, L"https://msdl.microsoft.com/download/symbols/%ls/%ls/%ls", pdb, id, pdb);
    if (!http_download(url, file))
        goto out;

    // No SYMOPT_UNDNAME: lookups use decorated names.
    pSymSetOptions(SYMOPT_EXACT_SYMBOLS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!pSymInitializeW(proc, dir, FALSE))
        goto out;
    GetModuleFileNameW(mod, image, MAX_PATH);
    base = pSymLoadModuleExW(proc, NULL, image, NULL, 0x10000000, 0, NULL, 0);
    for (int i = 0; base && i < SYM_COUNT; i++) {
        memset(&sym, 0, sizeof sym);
        sym.info.SizeOfStruct = sizeof sym.info;
        sym.info.MaxNameLen = 1024;
        if (pSymFromNameW(proc, syms[i].name, &sym.info) ||
            (syms[i].alt && pSymFromNameW(proc, syms[i].alt, &sym.info)))
            rva[i] = (DWORD)(sym.info.Address - base);
        if (rva[i] && !rva_valid(mod, rva[i])) {
            dbg(L"symbols: %ls at %lX is not a function start, ignored", syms[i].key, rva[i]);
            rva[i] = 0;
        }
    }
    pSymCleanup(proc);
    ok = base != 0;
out:
    if (*file)
        DeleteFileW(file);
    if (*dir)
        RemoveDirectoryW(dir);
    if (dh)
        FreeLibrary(dh);
    return ok;
}

static BOOL ini_write(const WCHAR *file, const WCHAR *id, const DWORD rva[SYM_COUNT])
{
    WCHAR section[4096], *p = section;
    size_t remaining = ARRAYSIZE(section);

    for (int i = 0; i < SYM_COUNT; i++) {
        int n = swprintf(p, remaining, L"%ls=%lX", syms[i].key, rva[i]);
        if (n < 0 || (size_t)n + 2 > remaining)
            return FALSE;
        p += n + 1;
        remaining -= n + 1;
    }
    *p = 0; // double NUL: publish the complete section in one profile API call
    return WritePrivateProfileSectionW(id, section, file);
}

// NULL on success, else why not.
static const WCHAR *update(void)
{
    HMODULE mod = LoadLibraryExW(L"windows.storage.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    WCHAR id[64], pdb[MAX_PATH], dir[MAX_PATH], file[MAX_PATH];
    DWORD rva[SYM_COUNT];
    unsigned known;

    if (!mod || !pdb_id(mod, id, pdb))
        return L"Cannot read the PDB id of windows.storage.dll.";
    if (!cache_path(dir, NULL) || !cache_path(file, L"symbols.ini"))
        return L"%LOCALAPPDATA% is not set.";

    if (ini_read(file, id, mod, rva, &known))
        return NULL;

    if (!from_pdb(mod, id, pdb, rva))
        return L"Cannot get symbols for this build of windows.storage.dll "
               L"from the Microsoft symbol server (msdl.microsoft.com).";

    CreateDirectoryW(dir, NULL);
    if (!ini_write(file, id, rva))
        return L"Cannot write %LOCALAPPDATA%\\ExplorerBetterFileSizes\\symbols.ini.";
    dbg(L"symbols: saved windows.storage.dll %ls", id);
    // Symbol discovery succeeded even if a particular feature is unavailable.
    // Only the feature installer knows which settings require which symbols.
    return NULL;
}

// rundll32 ExplorerBetterFileSizes.dll,UpdateSymbols [/v]
// One run at a time. /v shows failures in a message box.
void CALLBACK UpdateSymbolsW(HWND hwnd, HINSTANCE inst, LPWSTR cmdline, int show)
{
    BOOL verbose = cmdline && wcsstr(cmdline, L"/v");
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Local\\ExplorerBetterFileSizes.UpdateSymbols");
    const WCHAR *err;

    (void)inst; (void)show;
    if (!mutex)
        ExitProcess(1);
    DWORD waited = WaitForSingleObject(mutex, INFINITE);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
        CloseHandle(mutex);
        ExitProcess(1);
    }

    err = update();
    if (err) {
        dbg(L"symbols: %ls", err);
        if (verbose)
            MessageBoxW(hwnd, err, L"ExplorerBetterFileSizes", MB_ICONWARNING);
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    ExitProcess(err ? 1 : 0); // rundll32 /wait callers can detect update failures
}
