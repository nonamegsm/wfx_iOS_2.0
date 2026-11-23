/*
 * util.c - small helpers used by every module:
 *          text conversion, time conversion, ini file, log file, error box.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "wfx_ios.h"

HINSTANCE      g_inst = NULL;
int            g_plugin_nr = 0;
tProgressProcW g_progress_proc = NULL;
tLogProcW      g_log_proc = NULL;
char           g_ini[MAX_PATH] = "wfx_iOS.ini";
WCHAR          g_log_path[MAX_PATH] = L"";

static BOOL             g_log_enabled = FALSE;
static CRITICAL_SECTION g_message_lock;         /* protects the "last message" state */

void util_init(void)
{
    InitializeCriticalSection(&g_message_lock);
}

/* ------------------------------------------------------------------ text */

/* UTF-8 -> UTF-16. The result is always terminated; too long text is cut. */
void utf8_to_wide(const char* src, WCHAR* dst, int dst_len)
{
    if (dst_len <= 0) return;
    dst[0] = 0;
    if (!src || !*src) return;
    if (MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dst_len) == 0)
        dst[dst_len - 1] = 0;                   /* buffer was too small */
}

/* UTF-16 -> UTF-8. The result is always terminated; too long text is cut. */
void wide_to_utf8(const WCHAR* src, char* dst, int dst_len)
{
    if (dst_len <= 0) return;
    dst[0] = 0;
    if (!src || !*src) return;
    if (WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_len, NULL, NULL) == 0)
        dst[dst_len - 1] = 0;
}

void copy_text(char* dst, int dst_len, const char* src)
{
    if (dst_len <= 0) return;
    strncpy(dst, src ? src : "", dst_len - 1);
    dst[dst_len - 1] = 0;
}

void copy_wide(WCHAR* dst, int dst_len, const WCHAR* src)
{
    if (dst_len <= 0) return;
    wcsncpy(dst, src ? src : L"", dst_len - 1);
    dst[dst_len - 1] = 0;
}

/* swprintf that always terminates the result. */
void format_wide(WCHAR* dst, int dst_len, const WCHAR* fmt, ...)
{
    va_list args;
    if (dst_len <= 0) return;
    va_start(args, fmt);
    _vsnwprintf(dst, dst_len - 1, fmt, args);
    va_end(args);
    dst[dst_len - 1] = 0;
}

/* Like format_wide(), but adds to the text already in dst. */
void append_wide(WCHAR* dst, int dst_len, const WCHAR* fmt, ...)
{
    va_list args;
    int used = (int)wcslen(dst);
    if (used >= dst_len - 1) return;
    va_start(args, fmt);
    _vsnwprintf(dst + used, dst_len - used - 1, fmt, args);
    va_end(args);
    dst[dst_len - 1] = 0;
}

/* ------------------------------------------------------------------ time */

/* Offset between the Windows epoch (1601) and the Unix epoch (1970), in 100 ns units. */
#define EPOCH_DIFFERENCE 116444736000000000ULL

/* Nanoseconds since 1970 -> FILETIME. 0 becomes Total Commander's "no time" value. */
FILETIME unix_ns_to_filetime(uint64_t ns)
{
    FILETIME ft;
    if (ns == 0) {
        ft.dwLowDateTime = 0xFFFFFFFE;
        ft.dwHighDateTime = 0xFFFFFFFF;
    } else {
        uint64_t ticks = ns / 100 + EPOCH_DIFFERENCE;
        ft.dwLowDateTime = (DWORD)ticks;
        ft.dwHighDateTime = (DWORD)(ticks >> 32);
    }
    return ft;
}

uint64_t filetime_to_unix_ns(const FILETIME* ft)
{
    uint64_t ticks = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (ticks < EPOCH_DIFFERENCE) return 0;
    return (ticks - EPOCH_DIFFERENCE) * 100;
}

/* Local date and time in the user's format, or "-" for an unknown time. */
void format_time(uint64_t ns, WCHAR* dst, int dst_len)
{
    FILETIME ft;
    SYSTEMTIME utc, local;
    WCHAR date[64] = L"", time[64] = L"";

    if (ns == 0) { copy_wide(dst, dst_len, L"-"); return; }
    ft = unix_ns_to_filetime(ns);
    FileTimeToSystemTime(&ft, &utc);
    if (!SystemTimeToTzSpecificLocalTime(NULL, &utc, &local)) local = utc;
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &local, NULL, date, 64);
    GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &local, NULL, time, 64);
    format_wide(dst, dst_len, L"%s %s", date, time);
}

/* -------------------------------------------------------------- ini file */

BOOL ini_get_bool(const char* key, BOOL default_value)
{
    return GetPrivateProfileIntA(INI_SECTION, key, default_value ? 1 : 0, g_ini) != 0;
}

void ini_get_text(const char* key, char* dst, int dst_len)
{
    GetPrivateProfileStringA(INI_SECTION, key, "", dst, dst_len, g_ini);
}

void ini_set_text(const char* key, const char* value)
{
    WritePrivateProfileStringA(INI_SECTION, key, value, g_ini);
}

/* -------------------------------------------------------------- log file */

/*
 * The log is stderr redirected to a file, because libimobiledevice writes its
 * own trace to stderr. Settings:
 *   Log=0      no log at all
 *   Debug=1    add the full protocol trace (large and slow)
 *   LogFile=   file name, default %TEMP%\wfx_iOS.log
 */
void log_open(void)
{
    char custom[MAX_PATH];
    WCHAR path[MAX_PATH];

    if (g_log_enabled || !ini_get_bool("Log", TRUE)) return;

    ini_get_text("LogFile", custom, sizeof(custom));
    if (custom[0]) {
        MultiByteToWideChar(CP_ACP, 0, custom, -1, path, MAX_PATH);
    } else {
        GetTempPathW(MAX_PATH, path);
        append_wide(path, MAX_PATH, L"wfx_iOS.log");
    }
    if (!_wfreopen(path, L"w", stderr)) return;

    setvbuf(stderr, NULL, _IONBF, 0);
    copy_wide(g_log_path, MAX_PATH, path);
    g_log_enabled = TRUE;
    if (ini_get_bool("Debug", FALSE)) {
        idevice_set_debug_level(1);
        libusbmuxd_set_debug_level(1);
    }
    log_line("wfx_iOS %s (built %s %s, %d-bit), ini %s",
             WFX_VERSION_STR, __DATE__, __TIME__, (int)(sizeof(void*) * 8), g_ini);
}

/* One time-stamped line in the log. Text arguments must be UTF-8 or ASCII. */
void log_line(const char* fmt, ...)
{
    SYSTEMTIME t;
    va_list args;

    if (!g_log_enabled) return;
    GetLocalTime(&t);
    fprintf(stderr, "%02d:%02d:%02d.%03d wfx_iOS: ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------- error box */

/* TRUE for the messages that mean "there is no usable device". */
static BOOL is_no_device_message(const WCHAR* message)
{
    return wcsncmp(message, L"No iOS device", 13) == 0 ||
           wcsncmp(message, L"Cannot reach", 12) == 0 ||
           wcsncmp(message, L"Device ", 7) == 0 ||
           wcsncmp(message, L"Could not open the device", 25) == 0;
}

/* Set by device.c when a device was opened; lets the "no device" box appear again. */
extern LONG g_device_seen;

/*
 * Shows an error to the user. Total Commander retries the parent folders after
 * a failure, so the same problem would be reported several times in a row:
 *  - the same text is not shown again within 30 seconds,
 *  - a "no device" text is shown once per disconnect,
 *  - a different text that follows within 1.5 seconds is dropped.
 */
void show_error(const WCHAR* message)
{
    static WCHAR last_message[MSG_MAX] = L"";
    static DWORD last_tick = 0;
    char utf8[MSG_MAX * 3];
    BOOL show = TRUE;
    BOOL same, no_device;
    DWORD now = GetTickCount();

    if (!message || !*message) return;
    wide_to_utf8(message, utf8, sizeof(utf8));
    log_line("%s", utf8);

    EnterCriticalSection(&g_message_lock);
    same = wcscmp(message, last_message) == 0;
    no_device = is_no_device_message(message);
    if (same && (now - last_tick < 30000 || (no_device && !g_device_seen))) show = FALSE;
    if (!same && now - last_tick < 1500) show = FALSE;
    copy_wide(last_message, MSG_MAX, message);
    if (show) {
        last_tick = now;
        if (no_device) InterlockedExchange(&g_device_seen, 0);
    }
    LeaveCriticalSection(&g_message_lock);

    if (!show) return;
    if (g_log_proc) g_log_proc(g_plugin_nr, MSGTYPE_IMPORTANTERROR, (WCHAR*)message);
    MessageBoxW(GetActiveWindow(), message, PLUGIN_TITLE, MB_OK | MB_ICONWARNING);
}

/* ----------------------------------------------------------- device paths */

/* "/a/b/c" -> "/a/b";  "/a" -> "/" */
void path_parent(const char* path, char* dst, int dst_len)
{
    const char* slash = strrchr(path, '/');
    int len;

    if (!slash || slash == path) { copy_text(dst, dst_len, "/"); return; }
    len = (int)(slash - path);
    if (len > dst_len - 1) len = dst_len - 1;
    memcpy(dst, path, len);
    dst[len] = 0;
}

/* Joins a folder and a name. An absolute name replaces the folder. */
void path_join(const char* dir, const char* name, char* dst, int dst_len)
{
    size_t dir_len = strlen(dir);

    if (name[0] == '/') { copy_text(dst, dst_len, name); return; }
    if (dir_len > 0 && dir[dir_len - 1] == '/')
        _snprintf(dst, dst_len - 1, "%s%s", dir, name);
    else
        _snprintf(dst, dst_len - 1, "%s/%s", dir, name);
    dst[dst_len - 1] = 0;
}
