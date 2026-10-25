#include "ebfs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define TIMEOUT 1000 // ms, per pipe operation and per 1.4 query

enum { Q_OK, Q_NO_INDEX, Q_UNAVAILABLE };

// ---- 1.5: SDK3 named pipe

#define SDK3_GET_FOLDER_SIZE 18
#define SDK3_OK              200
#define SDK3_OK_MORE_DATA    100

struct sdk3_msg {
    DWORD code;
    DWORD size;
};

// Transfers exactly len bytes.
static BOOL pipe_io(HANDLE pipe, HANDLE ev, void *buf, DWORD len, BOOL write)
{
    BYTE *p = buf;

    while (len) {
        OVERLAPPED ov = { .hEvent = ev };
        DWORD n = 0;
        BOOL ok = write ? WriteFile(pipe, p, len, NULL, &ov)
                        : ReadFile(pipe, p, len, NULL, &ov);

        if (!ok && GetLastError() != ERROR_IO_PENDING)
            return FALSE;
        if (WaitForSingleObject(ev, TIMEOUT) != WAIT_OBJECT_0) {
            CancelIo(pipe);
            GetOverlappedResult(pipe, &ov, &n, TRUE); // ov is on our stack
            return FALSE;
        }
        if (!GetOverlappedResult(pipe, &ov, &n, FALSE) || !n)
            return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
}

// UTF-16 to WTF-8 (lone surrogates kept), so any NTFS name round-trips.
// With out == NULL, only counts.
static DWORD wtf8(const WCHAR *s, BYTE *out)
{
    DWORD n = 0;

    while (*s) {
        uint32_t c = *s++;

        if (c >= 0xD800 && c < 0xDC00 && *s >= 0xDC00 && *s < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (*s++ - 0xDC00);

        if (c < 0x80) {
            if (out) out[n] = (BYTE)c;
            n += 1;
        } else if (c < 0x800) {
            if (out) {
                out[n]     = (BYTE)(0xC0 | c >> 6);
                out[n + 1] = (BYTE)(0x80 | (c & 0x3F));
            }
            n += 2;
        } else if (c < 0x10000) {
            if (out) {
                out[n]     = (BYTE)(0xE0 | c >> 12);
                out[n + 1] = (BYTE)(0x80 | (c >> 6 & 0x3F));
                out[n + 2] = (BYTE)(0x80 | (c & 0x3F));
            }
            n += 3;
        } else {
            if (out) {
                out[n]     = (BYTE)(0xF0 | c >> 18);
                out[n + 1] = (BYTE)(0x80 | (c >> 12 & 0x3F));
                out[n + 2] = (BYTE)(0x80 | (c >> 6 & 0x3F));
                out[n + 3] = (BYTE)(0x80 | (c & 0x3F));
            }
            n += 4;
        }
    }
    return n;
}

// Once connected, any failure means "not indexed", as in the SDK.
static int query_sdk3(const WCHAR *pipe_name, const WCHAR *path, ULONGLONG *size)
{
    int r = Q_NO_INDEX;
    HANDLE pipe, ev = NULL;
    BYTE *req = NULL;
    DWORD len = wtf8(path, NULL);
    struct sdk3_msg msg = { SDK3_GET_FOLDER_SIZE, len };
    ULONGLONG value;

    pipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (pipe == INVALID_HANDLE_VALUE)
        return Q_UNAVAILABLE;

    ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    req = malloc(len ? len : 1);
    if (!ev || !req)
        goto out;
    wtf8(path, req);

    if (!pipe_io(pipe, ev, &msg, sizeof msg, TRUE) ||
        !pipe_io(pipe, ev, req, len, TRUE) ||
        !pipe_io(pipe, ev, &msg, sizeof msg, FALSE))
        goto out;
    if ((msg.code != SDK3_OK && msg.code != SDK3_OK_MORE_DATA) ||
        msg.size != sizeof value ||
        !pipe_io(pipe, ev, &value, sizeof value, FALSE))
        goto out;

    if (value != UINT64_MAX) {
        *size = value;
        r = Q_OK;
    }
out:
    free(req);
    if (ev)
        CloseHandle(ev);
    CloseHandle(pipe);
    return r;
}

// ---- 1.4: WM_COPYDATA

#define IPC_COPYDATA_QUERY2W  18
#define IPC_MATCHCASE         0x00000001
#define IPC_MATCHDIACRITICS   0x00000010
#define IPC_REQUEST_SIZE      0x00000010
#define IPC_SORT_NAME_ASC     1

struct ipc_query2 {
    DWORD reply_hwnd;
    DWORD reply_copydata_message;
    DWORD search_flags;
    DWORD offset;
    DWORD max_results;
    DWORD request_flags;
    DWORD sort_type;
    // WCHAR search[] follows
};

struct ipc_list2 {
    DWORD totitems;
    DWORD numitems;
    DWORD offset;
    DWORD request_flags;
    DWORD sort_type;
    // struct ipc_item2 items[numitems] follows
};

struct ipc_item2 {
    DWORD flags;
    DWORD data_offset; // from the start of ipc_list2
};

static struct {
    INIT_ONCE once;
    CRITICAL_SECTION query_lock;    // one query at a time
    CRITICAL_SECTION reply_lock;    // guards the rest
    HWND wnd;                       // message-only reply window
    HANDLE done;
    DWORD id, reply_id, next_id;
    BOOL found;
    ULONGLONG size;
} v14;

static LRESULT CALLBACK reply_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
    const struct ipc_list2 *list;
    const struct ipc_item2 *item;

    if (msg != WM_COPYDATA)
        return DefWindowProcW(hwnd, msg, wp, lp);

    // Anyone can send us WM_COPYDATA: check lengths first.
    if (cds->cbData < sizeof *list)
        return TRUE;
    list = cds->lpData;

    EnterCriticalSection(&v14.reply_lock);
    if (v14.id && cds->dwData == v14.id) {
        v14.found = FALSE;
        if (list->numitems && cds->cbData >= sizeof *list + sizeof *item) {
            item = (const struct ipc_item2 *)(list + 1);
            if (item->data_offset <= cds->cbData - sizeof(ULONGLONG)) {
                memcpy(&v14.size, (const BYTE *)list + item->data_offset, sizeof v14.size);
                v14.found = TRUE;
            }
        }
        v14.reply_id = v14.id;
        SetEvent(v14.done);
    }
    LeaveCriticalSection(&v14.reply_lock);
    return TRUE;
}

static DWORD WINAPI reply_thread(void *ready)
{
    WNDCLASSW wc = {
        .lpfnWndProc = reply_wndproc,
        .hInstance = g_self,
        .lpszClassName = L"EbfsEverythingReply",
    };
    MSG msg;

    RegisterClassW(&wc);
    v14.wnd = CreateWindowExW(0, wc.lpszClassName, NULL, 0, 0, 0, 0, 0,
                              HWND_MESSAGE, NULL, g_self, NULL);
    if (v14.wnd)
        ChangeWindowMessageFilterEx(v14.wnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);
    SetEvent(ready);
    if (!v14.wnd)
        return 1;

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);
    return 0;
}

static BOOL CALLBACK v14_init(INIT_ONCE *once, void *param, void **ctx)
{
    HANDLE ready, thread;

    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&v14.query_lock);
    InitializeCriticalSection(&v14.reply_lock);
    v14.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!v14.done || !ready)
        return TRUE; // no window: no 1.4 queries

    thread = CreateThread(NULL, 0, reply_thread, ready, 0, NULL);
    if (thread) {
        WaitForSingleObject(ready, INFINITE);
        CloseHandle(thread);
    }
    CloseHandle(ready);
    return TRUE;
}

static int query_v14(HWND everything, const WCHAR *path, ULONGLONG *size)
{
    static const WCHAR prefix[] = L"folder:wfn:\"", suffix[] = L"\"";
    size_t chars = wcslen(prefix) + wcslen(path) + wcslen(suffix) + 1;
    DWORD bytes = (DWORD)(sizeof(struct ipc_query2) + chars * sizeof(WCHAR));
    struct ipc_query2 *q;
    COPYDATASTRUCT cds;
    DWORD id;
    int r = Q_UNAVAILABLE;

    InitOnceExecuteOnce(&v14.once, v14_init, NULL, NULL);
    if (!v14.wnd || !(q = calloc(1, bytes)))
        return Q_UNAVAILABLE;

    EnterCriticalSection(&v14.query_lock);

    EnterCriticalSection(&v14.reply_lock);
    if (++v14.next_id == 0) // 0 means no query
        ++v14.next_id;
    id = v14.id = v14.next_id;
    ResetEvent(v14.done);
    LeaveCriticalSection(&v14.reply_lock);

    q->reply_hwnd = (DWORD)(ULONG_PTR)v14.wnd;
    q->reply_copydata_message = id;
    q->search_flags = IPC_MATCHCASE | IPC_MATCHDIACRITICS;
    q->max_results = 1;
    q->request_flags = IPC_REQUEST_SIZE;
    q->sort_type = IPC_SORT_NAME_ASC;
    swprintf((WCHAR *)(q + 1), chars, L"%ls%ls%ls", prefix, path, suffix);

    cds.dwData = IPC_COPYDATA_QUERY2W;
    cds.cbData = bytes;
    cds.lpData = q;

    if (SendMessageTimeoutW(everything, WM_COPYDATA, (WPARAM)v14.wnd, (LPARAM)&cds,
                            SMTO_BLOCK | SMTO_ABORTIFHUNG, TIMEOUT, NULL) &&
        WaitForSingleObject(v14.done, TIMEOUT) == WAIT_OBJECT_0) {
        EnterCriticalSection(&v14.reply_lock);
        if (v14.reply_id == id) {
            r = v14.found ? Q_OK : Q_NO_INDEX;
            *size = v14.size;
        }
        LeaveCriticalSection(&v14.reply_lock);
    }

    EnterCriticalSection(&v14.reply_lock);
    v14.id = 0; // ignore late replies
    LeaveCriticalSection(&v14.reply_lock);

    LeaveCriticalSection(&v14.query_lock);
    free(q);
    return r;
}

// ---- Query and fallbacks

static int query(const WCHAR *path, ULONGLONG *size)
{
    HWND wnd;
    int r;

    r = query_sdk3(L"\\\\.\\PIPE\\Everything IPC", path, size);
    if (r == Q_UNAVAILABLE)
        r = query_sdk3(L"\\\\.\\PIPE\\Everything IPC (1.5a)", path, size);
    if (r != Q_UNAVAILABLE)
        return r;

    wnd = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION_(1.5a)", NULL);
    if (!wnd)
        wnd = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", NULL);
    return wnd ? query_v14(wnd, path, size) : Q_UNAVAILABLE;
}

static BOOL is_reparse_point(const WCHAR *path)
{
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
}

static BOOL is_network_path(const WCHAR *path)
{
    WCHAR root[] = { path[0], L':', L'\\', 0 };

    if (path[0] == L'\\' && path[1] == L'\\')
        return TRUE;
    return path[0] && path[1] == L':' && GetDriveTypeW(root) == DRIVE_REMOTE;
}

// Final path, without the \\?\ prefix. Caller frees.
static WCHAR *resolve_path(const WCHAR *path)
{
    HANDLE h;
    DWORD n;
    WCHAR *buf = NULL;

    h = CreateFileW(path, FILE_READ_EA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;

    n = GetFinalPathNameByHandleW(h, NULL, 0, 0);
    if (n && (buf = malloc(n * sizeof *buf)) &&
        GetFinalPathNameByHandleW(h, buf, n, 0) == n - 1) {
        if (!wcsncmp(buf, L"\\\\?\\UNC\\", 8)) {
            memmove(buf + 2, buf + 8, (wcslen(buf + 8) + 1) * sizeof *buf);    // \\server\share
        } else if (!wcsncmp(buf, L"\\\\?\\", 4)) {
            memmove(buf, buf + 4, (wcslen(buf + 4) + 1) * sizeof *buf);
        }
    } else {
        free(buf);
        buf = NULL;
    }
    CloseHandle(h);
    return buf;
}

BOOL everything_folder_size(const WCHAR *path, ULONGLONG *size)
{
    BOOL zero_link;
    WCHAR *real;
    int r = query(path, size);

    // Links are indexed with size 0 and their subfolders not at all: ask
    // about the target. Unindexed network folders are slow to resolve.
    zero_link = r == Q_OK && *size == 0 && is_reparse_point(path);
    if (!zero_link && !(r == Q_NO_INDEX && !is_network_path(path)))
        return r == Q_OK;

    real = resolve_path(path);
    if (!real)
        return FALSE;
    if (wcscmp(real, path)) {
        r = query(real, size);
    } else {
        // Links that resolve to themselves (OneDrive) have their real size.
        r = zero_link ? Q_OK : r;
        if (zero_link)
            *size = 0;
    }
    free(real);
    return r == Q_OK;
}
