#define COBJMACROS
#include "ebfs.h"

#include <olectl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propvarutil.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "MinHook.h"
#include "hooks.h"
#include "feature_plan.h"
#include "size_cache.h"

HMODULE g_self;

static enum { PROC_OTHER, PROC_EXPLORER, PROC_EVERYTHING, PROC_HELPER } g_proc;

static struct {
    enum { FS_DISABLED, FS_EVERYTHING, FS_SHIFT, FS_ALWAYS } folder_sizes;
    BOOL mix_folders;                                   // mixFilesAndFoldersWhenSortingBySize
    enum { UNITS_DEFAULT, UNITS_MBGB, UNITS_KB } units; // fileSizeUnits
    BOOL iec;                                           // useIecTerms
    BOOL bytes_as_b;                                    // bytesAsB
} cfg;

void dbg(const WCHAR *fmt, ...)
{
    WCHAR buf[1024] = L"ExplorerBetterFileSizes: ";
    size_t n = wcslen(buf);
    va_list ap;

    va_start(ap, fmt);
    vswprintf(buf + n, 1024 - n - 1, fmt, ap);
    va_end(ap);
    wcscat_s(buf, 1024, L"\n");
    OutputDebugStringW(buf);
}

// <dll dir>\file
static void self_path(WCHAR *buf, DWORD cch, const WCHAR *file)
{
    DWORD n = GetModuleFileNameW(g_self, buf, cch);
    WCHAR *slash;

    if (!n || n >= cch || !(slash = wcsrchr(buf, L'\\'))) {
        *buf = 0;
        return;
    }
    slash[1] = 0;
    wcscat_s(buf, cch, file);
}

static void load_config(void)
{
    WCHAR file[MAX_PATH], v[32];

    self_path(file, MAX_PATH, L"config.ini");

    GetPrivateProfileStringW(L"config", L"folderSizeMode", L"disabled", v, 32, file);
    cfg.folder_sizes = !_wcsicmp(v, L"everything")   ? FS_EVERYTHING
                     : !_wcsicmp(v, L"scanWithShift") ? FS_SHIFT
                     : !_wcsicmp(v, L"scan")         ? FS_ALWAYS
                     :                                 FS_DISABLED;
    if (cfg.folder_sizes == FS_DISABLED && _wcsicmp(v, L"disabled"))
        dbg(L"config: invalid folderSizeMode=%ls; using disabled", v);

    cfg.mix_folders = GetPrivateProfileIntW(L"config", L"mixFilesAndFoldersWhenSortingBySize", 1, file) != 0;

    GetPrivateProfileStringW(L"config", L"fileSizeUnits", L"adaptive", v, 32, file);
    cfg.units = !_wcsicmp(v, L"adaptive")   ? UNITS_MBGB
              : !_wcsicmp(v, L"kb")         ? UNITS_KB
              :                               UNITS_DEFAULT;
    if (cfg.units == UNITS_DEFAULT && _wcsicmp(v, L"system"))
        dbg(L"config: invalid fileSizeUnits=%ls; using system", v);

    cfg.iec = GetPrivateProfileIntW(L"config", L"useIecTerms", 0, file) != 0;
    cfg.bytes_as_b = GetPrivateProfileIntW(L"config", L"bytesAsB", 1, file) != 0;
}

static const PROPERTYKEY PKEY_SIZE = {
    { 0xB725F130, 0x47EF, 0x101A, { 0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC } }, 12
};

static BOOL is_size_key(const PROPERTYKEY *key)
{
    return key->pid == PKEY_SIZE.pid && IsEqualGUID(&key->fmtid, &PKEY_SIZE.fmtid);
}

static void *proc(HMODULE mod, const char *name)
{
    return mod ? (void *)GetProcAddress(mod, name) : NULL;
}

// ---- Folder sizes

static _Thread_local BOOL tl_in_prepare, tl_in_do, tl_in_item_getvalue_size, tl_in_getsize;

static struct size_result *cache_lookup(IShellFolder2 *folder, PCUITEMID_CHILD child, int *fresh)
{
    PIDLIST_ABSOLUTE id = NULL;
    UINT len = 0;
    struct size_result *result;

    if (SUCCEEDED(SHGetIDListFromObject((IUnknown *)folder, &id)))
        len = ILGetSize(id);
    result = size_cache_lookup(id, len, child, ILGetSize((PCUIDLIST_RELATIVE)child),
                               GetTickCount(), fresh);
    CoTaskMemFree(id);
    return result;
}

struct walker {
    INamespaceWalkCB2 iface;
    ULONGLONG total;
};

static HRESULT STDMETHODCALLTYPE walker_QueryInterface(INamespaceWalkCB2 *self, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_INamespaceWalkCB) ||
        IsEqualIID(riid, &IID_INamespaceWalkCB2)) {
        *ppv = self;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

// The walker lives on walk_size's stack.
static ULONG STDMETHODCALLTYPE walker_AddRef(INamespaceWalkCB2 *self) { return 2; }
static ULONG STDMETHODCALLTYPE walker_Release(INamespaceWalkCB2 *self) { return 1; }

static HRESULT STDMETHODCALLTYPE walker_FoundItem(INamespaceWalkCB2 *self, IShellFolder *psf, PCUITEMID_CHILD pidl)
{
    struct walker *w = (struct walker *)self;
    IShellFolder2 *sf2;
    VARIANT v;
    HRESULT hr = IShellFolder_QueryInterface(psf, &IID_IShellFolder2, (void **)&sf2);

    if (FAILED(hr))
        return hr;
    VariantInit(&v);
    if (SUCCEEDED(IShellFolder2_GetDetailsEx(sf2, pidl, &PKEY_SIZE, &v)) && V_VT(&v) == VT_UI8)
        w->total += V_UI8(&v);
    VariantClear(&v);
    IShellFolder2_Release(sf2);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE walker_EnterFolder(INamespaceWalkCB2 *self, IShellFolder *psf, PCUITEMID_CHILD pidl) { return S_OK; }
static HRESULT STDMETHODCALLTYPE walker_LeaveFolder(INamespaceWalkCB2 *self, IShellFolder *psf, PCUITEMID_CHILD pidl) { return S_OK; }
static HRESULT STDMETHODCALLTYPE walker_InitializeProgressDialog(INamespaceWalkCB2 *self, LPWSTR *title, LPWSTR *cancel) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE walker_WalkComplete(INamespaceWalkCB2 *self, HRESULT hr) { return S_OK; }

static INamespaceWalkCB2Vtbl walker_vtbl = {
    .QueryInterface = walker_QueryInterface,
    .AddRef = walker_AddRef,
    .Release = walker_Release,
    .FoundItem = walker_FoundItem,
    .EnterFolder = walker_EnterFolder,
    .LeaveFolder = walker_LeaveFolder,
    .InitializeProgressDialog = walker_InitializeProgressDialog,
    .WalkComplete = walker_WalkComplete,
};

static BOOL walk_size(IShellFolder2 *folder, ULONGLONG *size)
{
    struct walker w = { { &walker_vtbl }, 0 };
    INamespaceWalk *walk;
    HRESULT hr = CoCreateInstance(&CLSID_NamespaceWalker, NULL, CLSCTX_INPROC,
                                  &IID_INamespaceWalk, (void **)&walk);

    if (FAILED(hr))
        return FALSE;
    hr = INamespaceWalk_Walk(walk, (IUnknown *)folder,
                             NSWF_DONT_ACCUMULATE_RESULT | NSWF_DONT_TRAVERSE_LINKS, 255,
                             (INamespaceWalkCB *)&w.iface);
    INamespaceWalk_Release(walk);
    *size = w.total;
    return SUCCEEDED(hr);
}

// Without the \\?\ prefix. Caller frees.
static WCHAR *folder_path(IShellFolder2 *folder)
{
    PIDLIST_ABSOLUTE id;
    WCHAR *path = NULL, *p;
    DWORD cch;

    if (FAILED(SHGetIDListFromObject((IUnknown *)folder, &id)))
        return NULL;
    for (cch = MAX_PATH; cch <= 2 * 32768; cch *= 2) {
        if (!(p = realloc(path, cch * sizeof *path)))
            break;
        path = p;
        if (SHGetPathFromIDListEx(id, path, cch, GPFIDL_DEFAULT)) {
            CoTaskMemFree(id);
            if (!wcsncmp(path, L"\\\\?\\UNC\\", 8))
                memmove(path + 2, path + 8, (wcslen(path + 8) + 1) * sizeof *path);
            else if (!wcsncmp(path, L"\\\\?\\", 4))
                memmove(path, path + 4, (wcslen(path + 4) + 1) * sizeof *path);
            return path;
        }
    }
    CoTaskMemFree(id);
    free(path);
    return NULL;
}

static BOOL folder_size(IShellFolder2 *parent, PCUITEMID_CHILD child, ULONGLONG *size)
{
    IShellFolder2 *folder;
    WCHAR *path;
    BOOL ok = FALSE;

    if (FAILED(IShellFolder2_BindToObject(parent, child, NULL, &IID_IShellFolder2, (void **)&folder)))
        return FALSE;
    if (cfg.folder_sizes != FS_EVERYTHING) {
        ok = walk_size(folder, size);
    } else if ((path = folder_path(folder))) {
        ok = everything_folder_size(path, size);
        free(path);
    }
    IShellFolder2_Release(folder);
    return ok;
}

static HRESULT (WINAPI *GetSize_orig)(void *, PCUITEMID_CHILD, const void *, PROPVARIANT *);
static HRESULT WINAPI GetSize_hook(void *fsfolder, PCUITEMID_CHILD child, const void *idfolder, PROPVARIANT *pv)
{
    HRESULT ret = GetSize_orig(fsfolder, child, idfolder, pv);
    IShellFolder2 *sf;
    struct size_result *e;
    int fresh;

    // Only sizeless items, and never during copy or delete. Not while sizing a
    // folder either: a nested lookup could free e.
    if (tl_in_prepare || tl_in_do || tl_in_getsize || ret != S_OK || pv->vt != VT_EMPTY)
        return ret;
    if (cfg.folder_sizes == FS_DISABLED ||
        (cfg.folder_sizes == FS_SHIFT && GetAsyncKeyState(VK_SHIFT) >= 0))
        return ret;

    if (FAILED(IUnknown_QueryInterface((IUnknown *)fsfolder, &IID_IShellFolder2, (void **)&sf)))
        return S_OK;
    e = cache_lookup(sf, child, &fresh);
    if (e && fresh) {
        tl_in_getsize = TRUE;
        ULONGLONG size;
        e->known = folder_size(sf, child, &size);
        if (e->known)
            e->size = size;
        tl_in_getsize = FALSE;
    }
    if (e && e->known) {
        pv->uhVal.QuadPart = e->size;
        pv->vt = VT_UI8;
    }
    IShellFolder2_Release(sf);
    return S_OK;
}

static HRESULT (WINAPI *Prepare_orig)(void *);
static HRESULT WINAPI Prepare_hook(void *self)
{
    HRESULT ret;
    BOOL prev = tl_in_prepare;

    tl_in_prepare = TRUE;
    ret = Prepare_orig(self);
    tl_in_prepare = prev;
    return ret;
}

static HRESULT (WINAPI *Do_orig)(void *);
static HRESULT WINAPI Do_hook(void *self)
{
    HRESULT ret;
    BOOL prev = tl_in_do;

    tl_in_do = TRUE;
    ret = Do_orig(self);
    tl_in_do = prev;
    return ret;
}

static HRESULT (WINAPI *Item_GetValue_orig)(void *, const void *, int, const PROPERTYKEY *, PROPVARIANT *, int *);
static HRESULT WINAPI Item_GetValue_hook(void *self, const void *item, int mode, const PROPERTYKEY *key,
                                         PROPVARIANT *pv, int *state)
{
    BOOL prev = tl_in_item_getvalue_size;
    HRESULT ret;

    tl_in_item_getvalue_size = is_size_key(key);
    ret = Item_GetValue_orig(self, item, mode, key, pv, state);
    tl_in_item_getvalue_size = prev;
    return ret;
}

#define GIPTYPE_EXTRACT 0 // extract unless cached and clean

static HRESULT (WINAPI *GetItemProperty_orig)(void *, void *, const void *, int, const PROPERTYKEY *, int *, PROPVARIANT *);
static HRESULT WINAPI GetItemProperty_hook(void *self, void *store, const void *item, int type,
                                           const PROPERTYKEY *key, int *dirty, PROPVARIANT *pv)
{
    HRESULT ret = GetItemProperty_orig(self, store, item, type, key, dirty, pv);

    // On a cache miss Item_GetValue takes the size from WIN32_FIND_DATA,
    // which has none for folders. Extract it instead, through _GetSize.
    if (!tl_in_item_getvalue_size || type == GIPTYPE_EXTRACT ||
        (SUCCEEDED(ret) && !(dirty && *dirty)) || !is_size_key(key))
        return ret;

    PropVariantClear(pv);
    ret = GetItemProperty_orig(self, store, item, GIPTYPE_EXTRACT, key, NULL, pv);
    if (SUCCEEDED(ret) && dirty)
        *dirty = 0;
    return ret;
}

static HRESULT (WINAPI *MapColumnToSCID_orig)(void *, UINT, PROPERTYKEY *);
static HRESULT (WINAPI *GetDetailsEx_orig)(void *, PCUITEMID_CHILD, const PROPERTYKEY *, VARIANT *);

static HRESULT (WINAPI *CompareIDs_orig)(void *, LPARAM, PCUIDLIST_RELATIVE, PCUIDLIST_RELATIVE);
static HRESULT WINAPI CompareIDs_hook(void *self, LPARAM column, PCUIDLIST_RELATIVE id1, PCUIDLIST_RELATIVE id2)
{
    PROPERTYKEY key;
    VARIANT v1, v2;
    BOOL ok;
    HRESULT ret;

    if (!id1 || !id2 || !cfg.mix_folders ||
        FAILED(MapColumnToSCID_orig(self, (UINT)column, &key)) || !is_size_key(&key))
        return CompareIDs_orig(self, column, id1, id2);

    VariantInit(&v1);
    VariantInit(&v2);
    ok = SUCCEEDED(GetDetailsEx_orig(self, (PCUITEMID_CHILD)id1, &key, &v1)) && V_VT(&v1) == VT_UI8 &&
         SUCCEEDED(GetDetailsEx_orig(self, (PCUITEMID_CHILD)id2, &key, &v2)) && V_VT(&v2) == VT_UI8;
    if (!ok)
        ret = CompareIDs_orig(self, column, id1, id2);
    else
        ret = V_UI8(&v1) > V_UI8(&v2) ? 1 : V_UI8(&v1) < V_UI8(&v2) ? 0xFFFF : 0;
    VariantClear(&v1);
    VariantClear(&v2);
    return ret;
}

// ---- Folder sizes in other views
//
// Adds System.Size to the property lists of the Tiles and Content views, the
// details pane and the status bar.
// https://github.com/ramensoftware/windhawk-mods/issues/2491#issuecomment-3762519795
static const struct {
    const WCHAR *name;
    const WCHAR *subkey;    // under HKCR
    const WCHAR *from;      // "" also matches a missing value
    const WCHAR *to;
} view_props[] = {
    { L"ContentViewModeForBrowse", L"Folder",
      L"prop:~System.ItemNameDisplay;~System.LayoutPattern.PlaceHolder;~System.LayoutPattern.PlaceHolder;~System.LayoutPattern.PlaceHolder;System.DateModified",
      L"prop:~System.ItemNameDisplay;~System.LayoutPattern.PlaceHolder;~System.LayoutPattern.PlaceHolder;~System.LayoutPattern.PlaceHolder;System.DateModified;System.Size" },
    { L"ContentViewModeForSearch", L"Folder",
      L"prop:~System.ItemNameDisplay;System.DateModified;~System.ItemFolderPathDisplay",
      L"prop:~System.ItemNameDisplay;System.DateModified;~System.ItemFolderPathDisplay;System.Size" },
    { L"TileInfo", L"Folder",
      L"prop:System.Title",
      L"prop:System.Title;System.Size" },
    { L"TileInfo", L"Folder",
      L"prop:System.Title;System.HomeGroupSharingStatus",
      L"prop:System.Title;System.HomeGroupSharingStatus;System.Size" },
    { L"PreviewDetails", L"Directory",
      L"prop:System.DateModified;*System.SharedWith;*System.StorageProviderState;*System.OfflineAvailability;*System.OfflineStatus",
      L"prop:System.DateModified;System.Size;System.DateCreated;*System.SharedWith;*System.StorageProviderState;*System.OfflineAvailability;*System.OfflineStatus" },
    { L"StatusBar", L"Directory",
      L"",
      L"prop:~System.StatusBarViewItemCount;~System.Size;~System.StatusBarSelectedItemCount;~System.StatusBarItemAvailability;System.StatusBarStorageProviderError;System.StatusIcons;System.Sync.ItemState;~System.Sync.GlobalActivityMessage;~System.Sync.LastSyncedMessage" },
};

static const WCHAR *view_prop(const WCHAR *name, const WCHAR *value)
{
    for (size_t i = 0; i < ARRAYSIZE(view_props); i++) {
        if (!_wcsicmp(name, view_props[i].name) && !wcscmp(value, view_props[i].from))
            return view_props[i].to;
    }
    return NULL;
}

static const WCHAR *view_prop_key(const WCHAR *name)
{
    for (size_t i = 0; i < ARRAYSIZE(view_props); i++) {
        if (!_wcsicmp(name, view_props[i].name))
            return view_props[i].subkey;
    }
    return NULL;
}

static DWORD view_prop_max(const WCHAR *name)
{
    DWORD max = 0;

    for (size_t i = 0; i < ARRAYSIZE(view_props); i++) {
        DWORD n = (DWORD)(wcslen(view_props[i].to) + 1) * sizeof(WCHAR);
        if (!_wcsicmp(name, view_props[i].name) && n > max)
            max = n;
    }
    return max;
}

// Is key HKLM\SOFTWARE\Classes\<subkey> or HKU\<SID>_Classes\<subkey>?
static BOOL is_class_key(HKEY key, const WCHAR *subkey)
{
    static const WCHAR machine[] = L"\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\";
    static const WCHAR user[] = L"\\REGISTRY\\USER\\";
    static LONG (NTAPI *NtQueryKey)(HANDLE, int, void *, ULONG, ULONG *);
    union {
        struct { ULONG len; WCHAR name[1]; } info; // KEY_NAME_INFORMATION
        BYTE space[sizeof(ULONG) + 512 * sizeof(WCHAR)];
    } buf;
    ULONG size;
    const WCHAR *name, *end, *sep;
    size_t n;

    if (!NtQueryKey)
        NtQueryKey = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryKey");
    // Longer names fail here; they are not ours anyway.
    if (!key || !NtQueryKey || NtQueryKey(key, 3, &buf, sizeof buf, &size) < 0 ||
        size < sizeof(ULONG) || buf.info.len > size - sizeof(ULONG))
        return FALSE;
    name = buf.info.name;
    end = name + buf.info.len / sizeof(WCHAR);

    if ((size_t)(end - name) >= wcslen(machine) && !_wcsnicmp(name, machine, wcslen(machine))) {
        name += wcslen(machine);
    } else if ((size_t)(end - name) >= wcslen(user) && !_wcsnicmp(name, user, wcslen(user))) {
        name += wcslen(user);
        for (sep = name; sep < end && *sep != L'\\'; sep++)
            ;
        if (sep == end || sep - name <= 8 || _wcsnicmp(sep - 8, L"_Classes", 8))
            return FALSE;
        name = sep + 1;
    } else {
        return FALSE;
    }
    n = wcslen(subkey);
    return (size_t)(end - name) == n && !_wcsnicmp(name, subkey, n);
}

static __typeof__(&RegQueryValueExW) RegQueryValueExW_orig;
static LSTATUS WINAPI RegQueryValueExW_hook(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type,
                                            LPBYTE data, LPDWORD cb)
{
    DWORD buf_size = data && cb ? *cb : 0, need;
    LSTATUS ret = RegQueryValueExW_orig(key, name, reserved, type, data, cb);
    const WCHAR *subkey, *value, *to;

    if ((ret != ERROR_SUCCESS && ret != ERROR_MORE_DATA && ret != ERROR_FILE_NOT_FOUND) ||
        !name || !(subkey = view_prop_key(name)))
        return ret;
    if (ret == ERROR_FILE_NOT_FOUND && !view_prop(name, L""))
        return ret;
    if (!is_class_key(key, subkey))
        return ret;

    if (ret == ERROR_FILE_NOT_FOUND) {
        to = view_prop(name, L"");
        need = (DWORD)(wcslen(to) + 1) * sizeof(WCHAR);
        if (type)
            *type = REG_SZ;
        if (!data || !cb) {
            if (data) // invalid per the docs
                return ret;
            if (cb)
                *cb = need;
            return ERROR_SUCCESS;
        }
        if (buf_size < need) {
            *cb = need;
            return ERROR_MORE_DATA;
        }
        memcpy(data, to, need);
        *cb = need;
        return ERROR_SUCCESS;
    }

    if (ret == ERROR_MORE_DATA || !data || !cb) {
        // Make the caller's next buffer big enough for the replacement.
        if (cb && *cb < view_prop_max(name))
            *cb = view_prop_max(name);
        return ret;
    }

    value = (const WCHAR *)data;
    if (*cb < sizeof(WCHAR) || value[*cb / sizeof(WCHAR) - 1])
        return ret; // not NUL-terminated
    if (!(to = view_prop(name, value)))
        return ret;
    need = (DWORD)(wcslen(to) + 1) * sizeof(WCHAR);
    if (buf_size < need) {
        *cb = need;
        return ERROR_MORE_DATA;
    }
    memcpy(data, to, need);
    *cb = need;
    return ret;
}

// ---- Everything.exe
//
// Everything opens folders on its UI thread, and the new Explorer window asks
// that thread for folder sizes: a deadlock until the query times out. Open
// them on a new thread. https://voidtools.com/forum/viewtopic.php?p=71433#p71433
struct open_folder {
    PIDLIST_ABSOLUTE folder;
    UINT count;
    DWORD flags;
    PCUITEMID_CHILD items[];
};

static __typeof__(&SHOpenFolderAndSelectItems) SHOpenFolderAndSelectItems_orig;

static DWORD WINAPI open_folder_thread(void *param)
{
    struct open_folder *p = param;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SHOpenFolderAndSelectItems_orig(p->folder, p->count, p->items, p->flags);
    CoUninitialize();
    ILFree(p->folder);
    for (UINT i = 0; i < p->count; i++)
        ILFree((LPITEMIDLIST)p->items[i]);
    free(p);
    return 0;
}

static BOOL CALLBACK find_everything_window(HWND wnd, LPARAM found)
{
    static const WCHAR prefix[] = L"EVERYTHING_TASKBAR_NOTIFICATION";
    WCHAR cls[64];

    if (GetClassNameW(wnd, cls, ARRAYSIZE(cls)) && !_wcsnicmp(cls, prefix, ARRAYSIZE(prefix) - 1)) {
        *(BOOL *)found = TRUE;
        return FALSE;
    }
    return TRUE;
}

static HRESULT WINAPI SHOpenFolderAndSelectItems_hook(PCIDLIST_ABSOLUTE folder, UINT count,
                                                      PCUITEMID_CHILD_ARRAY items, DWORD flags)
{
    BOOL ui_thread = FALSE;
    struct open_folder *p;
    HANDLE thread;

    EnumThreadWindows(GetCurrentThreadId(), find_everything_window, (LPARAM)&ui_thread);
    if (!ui_thread || !(p = calloc(1, sizeof *p + count * sizeof *p->items)))
        return SHOpenFolderAndSelectItems_orig(folder, count, items, flags);

    p->folder = ILClone(folder);
    p->count = count;
    p->flags = flags;
    for (UINT i = 0; i < count; i++)
        p->items[i] = ILClone((PCUIDLIST_RELATIVE)items[i]);

    thread = CreateThread(NULL, 0, open_folder_thread, p, 0, NULL);
    if (thread)
        CloseHandle(thread);
    else
        open_folder_thread(p);
    return S_OK;
}

// ---- Units

static HMODULE g_propsys;

// The localized unit of sizes below 1 KB, e.g. "bytes".
static WCHAR g_bytes_word[16];

static BOOL in_windows_dir(HMODULE mod)
{
    WCHAR path[MAX_PATH], win[MAX_PATH];
    UINT n = GetSystemWindowsDirectoryW(win, MAX_PATH);
    DWORD len = mod ? GetModuleFileNameW(mod, path, MAX_PATH) : 0;

    return n && n < MAX_PATH && len && len < MAX_PATH &&
           !_wcsnicmp(path, win, n) && path[n] == L'\\';
}

static HMODULE module_from(void *addr)
{
    HMODULE mod;

    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              addr, &mod) ? mod : NULL;
}

// Another hook on top of ours becomes the direct caller, so check a few more
// frames, up to the first one in a system module.
static __attribute__((noinline)) BOOL called_from(void *ret_addr, const WCHAR *module)
{
    HMODULE want = GetModuleHandleW(module), mod = module_from(ret_addr);
    void *frames[4];
    WORD n;

    if (!want)
        return FALSE;
    if (mod == want)
        return TRUE;
    if (in_windows_dir(mod))
        return FALSE;

    // Skip this function, the hook and the caller.
    n = CaptureStackBackTrace(3, ARRAYSIZE(frames), frames, NULL);
    for (WORD i = 0; i < n; i++) {
        mod = module_from(frames[i]);
        if (mod == want)
            return TRUE;
        if (in_windows_dir(mod))
            return FALSE;
    }
    return FALSE;
}

// MB/GB for large files: drop PDFF_ALWAYSKB for Explorer.
static __typeof__(&PSFormatForDisplayAlloc) PSFormatForDisplayAlloc_orig;
static HRESULT WINAPI PSFormatForDisplayAlloc_hook(REFPROPERTYKEY key, REFPROPVARIANT pv,
                                                   PROPDESC_FORMAT_FLAGS flags, PWSTR *out)
{
    if ((flags & PDFF_ALWAYSKB) && called_from(__builtin_return_address(0), L"explorerframe.dll"))
        flags &= ~PDFF_ALWAYSKB;
    return PSFormatForDisplayAlloc_orig(key, pv, flags, out);
}

// The same for older file dialogs, e.g. Regedit's export dialog.
static __typeof__(&PSFormatForDisplay) PSFormatForDisplay_orig;
static HRESULT WINAPI PSFormatForDisplay_hook(REFPROPERTYKEY key, REFPROPVARIANT pv,
                                              PROPDESC_FORMAT_FLAGS flags, LPWSTR text, DWORD cch)
{
    if ((flags & PDFF_ALWAYSKB) && called_from(__builtin_return_address(0), L"shell32.dll"))
        flags &= ~PDFF_ALWAYSKB;
    return PSFormatForDisplay_orig(key, pv, flags, text, cch);
}

// Always KB: without flags from the folder, the view uses its KB-only default.
static HRESULT (WINAPI *GetFormatForDisplayFlags_orig)(void *, const PROPERTYKEY *, PROPDESC_FORMAT_FLAGS *);
static HRESULT WINAPI GetFormatForDisplayFlags_hook(void *self, const PROPERTYKEY *key, PROPDESC_FORMAT_FLAGS *flags)
{
    HRESULT ret = GetFormatForDisplayFlags_orig(self, key, flags);

    if (FAILED(ret) || !is_size_key(key))
        return ret;
    *flags = PDFF_DEFAULT;
    return E_NOTIMPL;
}

static BOOL is_unit_letter(WCHAR c)
{
    return c && wcschr(L"KMGTPE", c);
}

// "KB" -> "KiB", if it fits.
static void add_i(WCHAR *s, size_t len, size_t cch)
{
    if (cch >= len + 2) {
        s[len - 1] = L'i';
        s[len] = L'B';
        s[len + 1] = 0;
    }
}

// "123 bytes" -> "123 B"
static BOOL bytes_to_b(WCHAR *s, size_t len)
{
    size_t n = wcslen(g_bytes_word);

    if (!n || len < n || wcscmp(s + len - n, g_bytes_word))
        return FALSE;
    s[len - n] = L'B';
    s[len - n + 1] = 0;
    return TRUE;
}

// propsys ordinal 421, PSStrFormatByteSizeW
static void *(WINAPI *FormatByteSize_orig)(ULONGLONG, LPWSTR, DWORD);
static void *WINAPI FormatByteSize_hook(ULONGLONG size, LPWSTR text, DWORD cch)
{
    void *ret = FormatByteSize_orig(size, text, cch);
    size_t len;

    if (!text || !cch)
        return ret;
    len = wcslen(text);
    if (cfg.bytes_as_b && size < 1024 && bytes_to_b(text, len))
        return ret;
    if (cfg.iec && len >= 2 && text[len - 1] == L'B' && is_unit_letter(text[len - 2]))
        add_i(text, len, cch);
    return ret;
}

// propsys ordinal 422, PSStrFormatKBSizeW
static void *(WINAPI *FormatKBSize_orig)(ULONGLONG, LPWSTR, DWORD);
static void *WINAPI FormatKBSize_hook(ULONGLONG size, LPWSTR text, DWORD cch)
{
    void *ret = FormatKBSize_orig(size, text, cch);
    size_t len;

    if (!text || !cch)
        return ret;
    len = wcslen(text);
    if (len >= 2 && text[len - 2] == L'K' && text[len - 1] == L'B')
        add_i(text, len, cch);
    return ret;
}

// propsys's "%s KB" and "%s bytes" templates
static __typeof__(&LoadStringW) LoadStringW_orig;
static int WINAPI LoadStringW_hook(HINSTANCE inst, UINT id, LPWSTR buf, int cch)
{
    int ret = LoadStringW_orig(inst, id, buf, cch);
    WCHAR *p = buf;

    if (!ret || inst != g_propsys || cch == 0)
        return ret;

    // "%s[ |U+00A0][KMGTPE]B" or "%s[ |U+00A0]<bytes word>"
    if (*p++ != L'%' || *p++ != L's')
        return ret;
    if (*p == L' ' || *p == L'\u00A0')
        p++;

    if (cfg.bytes_as_b && *g_bytes_word && !wcscmp(p, g_bytes_word)) {
        p[0] = L'B';
        p[1] = 0;
        return (int)(p + 1 - buf);
    }
    if (cfg.iec && is_unit_letter(p[0]) && p[1] == L'B' && !p[2] && cch >= p + 4 - buf) {
        p[1] = L'i';
        p[2] = L'B';
        p[3] = 0;
        return (int)(p + 3 - buf);
    }
    return ret;
}

// Formats 1 byte and keeps the unit: "1 bytes" -> "bytes".
static void learn_bytes_word(void *format_byte_size)
{
    WCHAR s[64] = L"", *p = s;

    ((void *(WINAPI *)(ULONGLONG, LPWSTR, DWORD))format_byte_size)(1, s, ARRAYSIZE(s));
    while (*p >= L'0' && *p <= L'9')
        p++;
    if (*p == L' ' || *p == L'\u00A0')
        p++;
    if (p != s && *p && wcslen(p) < ARRAYSIZE(g_bytes_word) && wcscmp(p, L"B"))
        wcscpy_s(g_bytes_word, ARRAYSIZE(g_bytes_word), p);
}

// ---- Installing

// The initial install and the symbol-update callback can overlap. Keep
// MinHook group installation and feature state changes in one sequence.
static SRWLOCK install_lock = SRWLOCK_INIT;

static enum hook_result hook(const WCHAR *feature, void *target, void *detour, void *orig)
{
    const struct hook_spec spec = { target, detour, orig };
    return hooks_install(feature, &spec, 1);
}

static void *kernel_export(const char *name)
{
    void *p = proc(GetModuleHandleW(L"kernelbase.dll"), name);
    return p ? p : proc(GetModuleHandleW(L"kernel32.dll"), name);
}

static struct feature_state storage_features[FEATURE_COUNT];

static void log_storage_features(unsigned known)
{
    for (int i = 0; i < FEATURE_COUNT; i++) {
        const struct feature_state *state = &storage_features[i];
        if (state->status == FEATURE_DISABLED)
            continue;
        dbg(L"features: %ls: %ls", feature_specs[i].name, feature_status_name(state->status));
        if (state->status == FEATURE_BLOCKED) {
            enum feature_id parent = feature_specs[i].prerequisite;
            dbg(L"  requires %ls", feature_specs[parent].name);
        }
        for (int sym = 0; sym < SYM_COUNT; sym++) {
            if (state->missing & SYMBOL_BIT(sym))
                dbg(L"  %ls: %ls", symbols_name(sym),
                    known & SYMBOL_BIT(sym) ? L"absent" : L"not resolved");
        }
    }
}

static enum hook_result install_storage_feature(enum feature_id id, HMODULE storage,
                                               const DWORD rva[SYM_COUNT])
{
    BYTE *base = (BYTE *)storage;
    const WCHAR *name = feature_specs[id].name;
#define AT(sym) ((void *)(base + rva[sym]))

    switch (id) {
    case FEATURE_FOLDER_SIZES: {
        const struct hook_spec sizes[] = {
            { AT(SYM_PREPARE), Prepare_hook, &Prepare_orig },
            { AT(SYM_DO), Do_hook, &Do_orig },
            { AT(SYM_GETSIZE), GetSize_hook, &GetSize_orig },
        };
        return hooks_install(name, sizes, ARRAYSIZE(sizes));
    }
    case FEATURE_FOLDER_PROPERTIES: {
        const struct hook_spec properties[] = {
            { AT(SYM_GETITEMPROPERTY), GetItemProperty_hook, &GetItemProperty_orig },
            { AT(SYM_ITEM_GETVALUE), Item_GetValue_hook, &Item_GetValue_orig },
        };
        return hooks_install(name, properties, ARRAYSIZE(properties));
    }
    case FEATURE_MIXED_SORTING:
        MapColumnToSCID_orig = AT(SYM_MAPCOLUMNTOSCID);
        GetDetailsEx_orig = AT(SYM_GETDETAILSEX);
        return hook(name, AT(SYM_COMPAREIDS), CompareIDs_hook, &CompareIDs_orig);
    case FEATURE_FOLDER_VIEWS:
        return hook(name, kernel_export("RegQueryValueExW"),
                    RegQueryValueExW_hook, &RegQueryValueExW_orig);
    case FEATURE_KB:
        return hook(name, AT(SYM_GETFORMATFORDISPLAYFLAGS),
                    GetFormatForDisplayFlags_hook, &GetFormatForDisplayFlags_orig);
    case FEATURE_COUNT:
        break;
    }
#undef AT
    return HOOK_FAILED;
}

// Install every requested feature that has its own prerequisites. Incomplete
// caches do not hold back unrelated features. TRUE means discovery is needed.
static BOOL install_storage_features(void)
{
    static HMODULE storage;
    DWORD rva[SYM_COUNT];
    unsigned known = 0, available = 0;

    if (!storage)
        storage = LoadLibraryExW(L"windows.storage.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!storage) {
        dbg(L"features: cannot load windows.storage.dll: %lu", GetLastError());
        for (int i = 0; i < FEATURE_COUNT; i++) {
            if (storage_features[i].status != FEATURE_DISABLED)
                feature_finish(storage_features, i, HOOK_FAILED);
        }
        log_storage_features(0);
        return FALSE;
    }

    symbols_load(storage, rva, &known);
    for (int i = 0; i < SYM_COUNT; i++) {
        if (rva[i])
            available |= SYMBOL_BIT(i);
    }
    for (int i = 0; i < FEATURE_COUNT; i++) {
        if (feature_resolve(storage_features, i, known, available) == FEATURE_READY)
            feature_finish(storage_features, i, install_storage_feature(i, storage, rva));
    }
    log_storage_features(known);
    return features_waiting(storage_features);
}

// The UpdateSymbols process has exited. Retry pending features only; active
// features keep their existing trampolines even if the helper failed.
static void CALLBACK symbols_ready(PTP_CALLBACK_INSTANCE inst, void *process, PTP_WAIT wait,
                                   TP_WAIT_RESULT result)
{
    DWORD code = 0;
    if (!GetExitCodeProcess(process, &code))
        dbg(L"symbols: cannot read updater exit code: %lu", GetLastError());
    else if (code)
        dbg(L"symbols: updater failed with exit code %lu", code);
    CloseThreadpoolWait(wait);
    CloseHandle(process);
    AcquireSRWLockExclusive(&install_lock);
    if (install_storage_features())
        dbg(L"symbols: pending features remain off; rerun install.cmd to retry");
    ReleaseSRWLockExclusive(&install_lock);
}

static void init_explorer(void)
{
    HANDLE update;
    PTP_WAIT wait;
    void *fmt_byte, *fmt_kb;

    if ((cfg.folder_sizes != FS_DISABLED || cfg.units == UNITS_KB) && install_storage_features()) {
        // New Windows build: fetch offsets in another process, hook when it exits.
        if ((update = symbols_request_update())) {
            if ((wait = CreateThreadpoolWait(symbols_ready, update, NULL)))
                SetThreadpoolWait(wait, update, NULL);
            else {
                dbg(L"symbols: cannot wait for updater: %lu; restart Explorer after it finishes", GetLastError());
                CloseHandle(update);
            }
        } else {
            dbg(L"symbols: cannot start updater: %lu", GetLastError());
        }
    }

    g_propsys = LoadLibraryExW(L"propsys.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (cfg.units == UNITS_MBGB) {
        hook(L"MB/GB formatting", proc(g_propsys, "PSFormatForDisplayAlloc"), PSFormatForDisplayAlloc_hook,
             &PSFormatForDisplayAlloc_orig);
        hook(L"legacy MB/GB formatting", proc(g_propsys, "PSFormatForDisplay"), PSFormatForDisplay_hook, &PSFormatForDisplay_orig);
    }

    if (cfg.iec || cfg.bytes_as_b) {
        fmt_byte = proc(g_propsys, MAKEINTRESOURCEA(421));
        fmt_kb = proc(g_propsys, MAKEINTRESOURCEA(422));
        if (cfg.bytes_as_b && fmt_byte)
            learn_bytes_word(fmt_byte);
        hook(L"byte units", fmt_byte, FormatByteSize_hook, &FormatByteSize_orig);
        if (cfg.iec)
            hook(L"KB units", fmt_kb, FormatKBSize_hook, &FormatKBSize_orig);
        hook(L"unit resources", kernel_export("LoadStringW"), LoadStringW_hook, &LoadStringW_orig);
    }
}

static BOOL CALLBACK init(INIT_ONCE *once, void *param, void **ctx)
{
    HMODULE pinned;
    MH_STATUS st;

    load_config();
    features_init(storage_features, cfg.folder_sizes != FS_DISABLED, cfg.mix_folders, cfg.units == UNITS_KB);
    if (g_proc == PROC_EVERYTHING && cfg.folder_sizes != FS_EVERYTHING)
        return TRUE;

    // Hooks jump into this DLL: never unload it.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (const WCHAR *)g_self, &pinned);

    if ((st = MH_Initialize()) != MH_OK) {
        dbg(L"MH_Initialize: %hs", MH_StatusToString(st));
        return TRUE;
    }
    AcquireSRWLockExclusive(&install_lock);
    if (g_proc == PROC_EVERYTHING)
        hook(L"Everything deadlock fix", proc(LoadLibraryExW(L"shell32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "SHOpenFolderAndSelectItems"),
             SHOpenFolderAndSelectItems_hook, &SHOpenFolderAndSelectItems_orig);
    else
        init_explorer();
    ReleaseSRWLockExclusive(&install_lock);
    return TRUE;
}

// ---- Entry points

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **ppv)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

    if (ppv)
        *ppv = NULL;
    if (g_proc == PROC_EXPLORER || g_proc == PROC_EVERYTHING)
        InitOnceExecuteOnce(&once, init, NULL, NULL);
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

// regsvr32 [/u]. Never change the CLSID: every version must be able to
// unregister every other.
#define CLSID_KEY L"SOFTWARE\\Classes\\CLSID\\{B8C4F290-3A71-4D82-9F1E-65C2A10E8B01}"
#define EXT_KEY   L"SOFTWARE\\Classes\\Drive\\shellex\\FolderExtensions\\{B8C4F290-3A71-4D82-9F1E-65C2A10E8B01}"

static LSTATUS set_value(const WCHAR *key, const WCHAR *name, DWORD type, const void *data, DWORD size)
{
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, key, name, type, data, size);
}

static LSTATUS set_string(const WCHAR *key, const WCHAR *name, const WCHAR *s)
{
    return set_value(key, name, REG_SZ, s, (DWORD)(wcslen(s) + 1) * sizeof *s);
}

HRESULT WINAPI DllRegisterServer(void)
{
    static const DWORD all_drives = 0xFF;
    WCHAR dll[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, dll, MAX_PATH);

    if (!n || n >= MAX_PATH)
        return E_FAIL;
    if (set_string(CLSID_KEY, NULL, L"ExplorerBetterFileSizes") ||
        set_string(CLSID_KEY L"\\InProcServer32", NULL, dll) ||
        set_string(CLSID_KEY L"\\InProcServer32", L"ThreadingModel", L"Apartment") ||
        set_string(EXT_KEY, NULL, L"ExplorerBetterFileSizes") ||
        set_value(EXT_KEY, L"DriveMask", REG_DWORD, &all_drives, sizeof all_drives)) {
        DllUnregisterServer();
        return SELFREG_E_CLASS;
    }
    return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    LSTATUS a = RegDeleteTreeW(HKEY_LOCAL_MACHINE, EXT_KEY);
    LSTATUS b = RegDeleteTreeW(HKEY_LOCAL_MACHINE, CLSID_KEY);

    return (a && a != ERROR_FILE_NOT_FOUND) || (b && b != ERROR_FILE_NOT_FOUND) ? SELFREG_E_CLASS : S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    WCHAR exe[MAX_PATH];
    const WCHAR *name;
    DWORD n;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_self = inst;
        n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        name = n && n < MAX_PATH && wcsrchr(exe, L'\\') ? wcsrchr(exe, L'\\') + 1 : L"";
        g_proc = !_wcsicmp(name, L"explorer.exe")     ? PROC_EXPLORER
               : !_wcsicmp(name, L"Everything.exe")   ? PROC_EVERYTHING
               : !_wcsicmp(name, L"Everything64.exe") ? PROC_EVERYTHING
               : !_wcsicmp(name, L"rundll32.exe")     ? PROC_HELPER
               : !_wcsicmp(name, L"regsvr32.exe")     ? PROC_HELPER
               :                                        PROC_OTHER;
        // Every program that browses drives loads us: refuse where we have no job.
        return g_proc != PROC_OTHER;

    case DLL_THREAD_DETACH:
        size_cache_thread_cleanup();
        break;
    }
    return TRUE;
}
