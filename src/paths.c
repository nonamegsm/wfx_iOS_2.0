/*
 * paths.c - turns a Total Commander path into "which service, which path on
 *           the device", and keeps the table of application names.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "wfx_ios.h"

/*
 * Two small tables translate the names the user sees into identifiers:
 *
 *   applications   "Keynote"        -> "com.apple.Keynote"   (per device)
 *   devices        "Anna's iPhone"  -> UDID                  (several devices connected)
 *
 * Both are filled by listing.c when the corresponding folder is read.
 */

/* One application as shown in \Applications. */
typedef struct {
    char  udid[UDID_MAX];           /* device the entry belongs to ("" = default device) */
    WCHAR shown[APP_ID_MAX];        /* folder name the user sees */
    char  bundle[APP_ID_MAX];       /* e.g. "com.example.app" */
} AppEntry;

/* One device folder as shown in the plugin root. */
typedef struct {
    WCHAR shown[128];
    char  udid[UDID_MAX];
} DeviceEntry;

#define LOADED_MAX   16             /* devices whose application list was read */
#define DEVICE_NAMES 16             /* device folders */

static CRITICAL_SECTION g_tables_lock;          /* protects everything below */

static AppEntry*   g_apps = NULL;
static int         g_apps_count = 0;
static int         g_apps_capacity = 0;
static char        g_apps_loaded[LOADED_MAX][UDID_MAX];
static int         g_apps_loaded_count = 0;

static DeviceEntry g_device_names[DEVICE_NAMES];
static int         g_device_names_count = 0;
static BOOL        g_device_names_loaded = FALSE;

void paths_init(void)
{
    InitializeCriticalSection(&g_tables_lock);
}

/* -------------------------------------------------- application name table */

/* Removes the entries of one device (udid == NULL: of all devices). */
void apps_table_clear(const char* udid)
{
    int i, kept = 0;

    EnterCriticalSection(&g_tables_lock);
    for (i = 0; i < g_apps_count; i++)
        if (udid && strcmp(g_apps[i].udid, udid) != 0) g_apps[kept++] = g_apps[i];
    g_apps_count = kept;

    kept = 0;
    for (i = 0; i < g_apps_loaded_count; i++)
        if (udid && strcmp(g_apps_loaded[i], udid) != 0) {
            if (kept != i) memcpy(g_apps_loaded[kept], g_apps_loaded[i], UDID_MAX);
            kept++;
        }
    g_apps_loaded_count = kept;
    LeaveCriticalSection(&g_tables_lock);
}

void apps_table_add(const char* udid, const WCHAR* shown_name, const char* bundle_id)
{
    EnterCriticalSection(&g_tables_lock);
    if (g_apps_count == g_apps_capacity) {
        int capacity = g_apps_capacity ? g_apps_capacity * 2 : 64;
        AppEntry* grown = (AppEntry*)realloc(g_apps, capacity * sizeof(AppEntry));
        if (!grown) { LeaveCriticalSection(&g_tables_lock); return; }
        g_apps = grown;
        g_apps_capacity = capacity;
    }
    copy_text(g_apps[g_apps_count].udid, UDID_MAX, udid);
    copy_wide(g_apps[g_apps_count].shown, APP_ID_MAX, shown_name);
    copy_text(g_apps[g_apps_count].bundle, APP_ID_MAX, bundle_id);
    g_apps_count++;
    LeaveCriticalSection(&g_tables_lock);
}

void apps_table_done(const char* udid)
{
    EnterCriticalSection(&g_tables_lock);
    if (g_apps_loaded_count < LOADED_MAX)
        copy_text(g_apps_loaded[g_apps_loaded_count++], UDID_MAX, udid);
    LeaveCriticalSection(&g_tables_lock);
}

/* Looks a folder name up in the table. Returns FALSE when it is not there. */
static BOOL apps_lookup(const char* udid, const WCHAR* shown_name, char* bundle_id, int bundle_len, BOOL* loaded)
{
    WCHAR as_wide[APP_ID_MAX];
    BOOL found = FALSE;
    int i;

    EnterCriticalSection(&g_tables_lock);
    for (i = 0; i < g_apps_count && !found; i++) {
        if (strcmp(g_apps[i].udid, udid) != 0) continue;
        /* The bundle id itself is accepted too, so old paths keep working. */
        utf8_to_wide(g_apps[i].bundle, as_wide, APP_ID_MAX);
        if (_wcsicmp(g_apps[i].shown, shown_name) == 0 || _wcsicmp(as_wide, shown_name) == 0) {
            copy_text(bundle_id, bundle_len, g_apps[i].bundle);
            found = TRUE;
        }
    }
    *loaded = FALSE;
    for (i = 0; i < g_apps_loaded_count; i++)
        if (strcmp(g_apps_loaded[i], udid) == 0) *loaded = TRUE;
    LeaveCriticalSection(&g_tables_lock);
    return found;
}

/*
 * Folder name under \Applications -> bundle id. When Total Commander starts
 * inside an application folder, the table is still empty: load it once.
 * An unknown name is used as a bundle id as it is.
 */
void apps_resolve(const char* udid, const WCHAR* shown_name, char* bundle_id, int bundle_len)
{
    BOOL loaded;

    if (apps_lookup(udid, shown_name, bundle_id, bundle_len, &loaded)) return;
    if (!loaded && apps_load(udid, NULL, 0) && apps_lookup(udid, shown_name, bundle_id, bundle_len, &loaded)) return;
    wide_to_utf8(shown_name, bundle_id, bundle_len);
}

/* ------------------------------------------------------ device name table */

void devices_table_clear(void)
{
    EnterCriticalSection(&g_tables_lock);
    g_device_names_count = 0;
    g_device_names_loaded = TRUE;       /* an empty table is a valid result */
    LeaveCriticalSection(&g_tables_lock);
}

void devices_table_add(const WCHAR* shown_name, const char* udid)
{
    EnterCriticalSection(&g_tables_lock);
    if (g_device_names_count < DEVICE_NAMES) {
        copy_wide(g_device_names[g_device_names_count].shown, 128, shown_name);
        copy_text(g_device_names[g_device_names_count].udid, UDID_MAX, udid);
        g_device_names_count++;
    }
    LeaveCriticalSection(&g_tables_lock);
}

static BOOL devices_lookup(const WCHAR* shown_name, char* udid, int udid_len, BOOL* loaded)
{
    BOOL found = FALSE;
    int i;

    EnterCriticalSection(&g_tables_lock);
    for (i = 0; i < g_device_names_count && !found; i++) {
        if (_wcsicmp(g_device_names[i].shown, shown_name) == 0) {
            copy_text(udid, udid_len, g_device_names[i].udid);
            found = TRUE;
        }
    }
    *loaded = g_device_names_loaded;
    LeaveCriticalSection(&g_tables_lock);
    return found;
}

/* Device folder name -> UDID. Reads the device list once if it was never read. */
BOOL devices_resolve(const WCHAR* shown_name, char* udid, int udid_len)
{
    BOOL loaded;

    if (devices_lookup(shown_name, udid, udid_len, &loaded)) return TRUE;
    if (loaded) return FALSE;
    devices_load();
    return devices_lookup(shown_name, udid, udid_len, &loaded);
}

/* ------------------------------------------------------------ path parsing */

BOOL is_afc_mode(PathMode mode)
{
    return mode == M_ROOT || mode == M_USER || mode == M_PHOTOS || mode == M_PROCESSED ||
           mode == M_ICLOUD || mode == M_APP;
}

/* Paths with the same key can share one AFC connection: "<udid>|<service>[:<app>]". */
void connection_key(const RemotePath* rp, char* key, int key_len)
{
    if (rp->mode == M_ROOT)      _snprintf(key, key_len - 1, "%s|afc2", rp->udid);
    else if (rp->mode == M_APP)  _snprintf(key, key_len - 1, "%s|app:%s", rp->udid, rp->app);
    else                         _snprintf(key, key_len - 1, "%s|afc", rp->udid);
    key[key_len - 1] = 0;
}

/* The fixed folder names of the plugin; a device folder must not use one of them. */
BOOL is_reserved_name(const WCHAR* name)
{
    static const WCHAR* reserved[] = {
        L"Root", L"User", L"Photos", L"Processed photos", L"iCloud photos", MONTHS_FOLDER,
        L"Applications", SETTINGS_ITEM, INFO_ITEM
    };
    int i;
    for (i = 0; i < (int)(sizeof(reserved) / sizeof(reserved[0])); i++)
        if (_wcsicmp(name, reserved[i]) == 0) return TRUE;
    return FALSE;
}

/* Copies the next path component (up to '\' or the end) and returns what follows it. */
static const WCHAR* next_component(const WCHAR* path, WCHAR* component, int component_len)
{
    int len = 0;
    while (*path && *path != L'\\') {
        if (len < component_len - 1) component[len++] = *path;
        path++;
    }
    component[len] = 0;
    return path;                    /* points at '\' or at the terminator */
}

/*
 * Paths under "\Photos by month". 'rest' is what follows that folder:
 *     ""                      the list of months        M_MONTHS, path "/"
 *     "\2026-11"              one month                 M_MONTHS, path "/2026-11"
 *     "\2026-11\IMG_1.JPG"    a picture: becomes M_USER with the real path on
 *                             the device, so that it behaves like any other file
 */
static void parse_months_path(const WCHAR* rest, RemotePath* out)
{
    WCHAR month[32], name[MAX_PATH];
    char month_utf8[64], name_utf8[MAX_PATH * 3];

    out->mode = M_MONTHS;
    if (*rest == 0) return;                             /* path is already "/" */

    rest = next_component(rest + 1, month, 32);
    wide_to_utf8(month, month_utf8, sizeof(month_utf8));
    if (*rest == 0) {
        _snprintf(out->path, AFC_PATH_MAX - 1, "/%s", month_utf8);
        out->path[AFC_PATH_MAX - 1] = 0;
        return;
    }

    rest = next_component(rest + 1, name, MAX_PATH);
    wide_to_utf8(name, name_utf8, sizeof(name_utf8));
    if (*rest == 0 && photos_resolve(out->udid, month_utf8, name_utf8, out->path, AFC_PATH_MAX))
        out->mode = M_USER;
    else
        out->mode = M_INVALID;                          /* nothing lives deeper than a picture */
}

/*
 * Splits a Total Commander path such as
 *     \Applications\Keynote\Documents\a.key
 * into the mode (M_APP), the application ("com.apple.Keynote") and the path
 * on the device ("/Documents/a.key").
 *
 * With several devices connected the path starts with a device folder:
 *     \Anna's iPhone\Applications\Keynote\Documents\a.key
 * which additionally sets the device (udid).
 */
void parse_path(const WCHAR* tc_path, RemotePath* out)
{
    WCHAR path[TC_PATH_MAX];
    WCHAR first[APP_ID_MAX];
    const WCHAR* rest;
    const char* prefix = "";
    int len, i;

    memset(out, 0, sizeof(*out));
    out->mode = M_INVALID;
    copy_text(out->path, AFC_PATH_MAX, "/");

    /* Normalise: backslashes only, no trailing backslash. */
    copy_wide(path, TC_PATH_MAX, tc_path);
    len = (int)wcslen(path);
    for (i = 0; i < len; i++) if (path[i] == L'/') path[i] = L'\\';
    while (len > 1 && path[len - 1] == L'\\') path[--len] = 0;

    if (len == 0 || wcscmp(path, L"\\") == 0) { out->mode = M_TOP; return; }

    rest = next_component(path[0] == L'\\' ? path + 1 : path, first, APP_ID_MAX);

    /* A first component that is not one of our folders must be a device folder. */
    if (!is_reserved_name(first)) {
        if (!devices_resolve(first, out->udid, UDID_MAX)) return;      /* M_INVALID */
        if (*rest == 0) { out->mode = M_TOP; return; }                 /* the device's own root */
        rest = next_component(rest + 1, first, APP_ID_MAX);
    }

    if      (wcscmp(first, L"Root") == 0)             out->mode = M_ROOT;
    else if (wcscmp(first, L"User") == 0)             out->mode = M_USER;
    else if (wcscmp(first, L"Photos") == 0)           { out->mode = M_PHOTOS;    prefix = "/DCIM"; }
    else if (wcscmp(first, L"Processed photos") == 0) { out->mode = M_PROCESSED; prefix = "/DCIM"; }
    else if (wcscmp(first, L"iCloud photos") == 0)    { out->mode = M_ICLOUD;    prefix = ICLOUD_DIR; }
    else if (wcscmp(first, MONTHS_FOLDER) == 0)       { parse_months_path(rest, out); return; }
    else if (wcscmp(first, SETTINGS_ITEM) == 0 && *rest == 0) { out->mode = M_SETTINGS; return; }
    else if (wcscmp(first, INFO_ITEM) == 0 && *rest == 0)     { out->mode = M_INFO; return; }
    else if (wcscmp(first, L"Applications") == 0) {
        WCHAR app_name[APP_ID_MAX];
        if (*rest == 0) { out->mode = M_APPLIST; return; }
        rest = next_component(rest + 1, app_name, APP_ID_MAX);
        out->mode = M_APP;
        apps_resolve(out->udid, app_name, out->app, APP_ID_MAX);
    }
    else return;                    /* M_INVALID */

    /* Device path = prefix + the remaining components with forward slashes. */
    {
        char rest_utf8[AFC_PATH_MAX];
        char* p;
        wide_to_utf8(rest, rest_utf8, sizeof(rest_utf8));
        for (p = rest_utf8; *p; p++) if (*p == '\\') *p = '/';
        _snprintf(out->path, AFC_PATH_MAX - 1, "%s%s", prefix, rest_utf8);
        out->path[AFC_PATH_MAX - 1] = 0;
        if (out->path[0] == 0) copy_text(out->path, AFC_PATH_MAX, "/");
    }
}
