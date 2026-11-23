/*
 * paths.c - turns a Total Commander path into "which service, which path on
 *           the device", and keeps the table of application names.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "wfx_ios.h"

/* -------------------------------------------------- application name table */

/* One application as shown in \Applications. */
typedef struct {
    WCHAR shown[APP_ID_MAX];        /* folder name the user sees */
    char  bundle[APP_ID_MAX];       /* e.g. "com.example.app" */
} AppEntry;

static CRITICAL_SECTION g_apps_lock;
static AppEntry*        g_apps = NULL;
static int              g_apps_count = 0;
static int              g_apps_capacity = 0;
static BOOL             g_apps_loaded = FALSE;      /* FALSE until \Applications was read once */

void paths_init(void)
{
    InitializeCriticalSection(&g_apps_lock);
}

static void apps_lock(void)
{
    EnterCriticalSection(&g_apps_lock);
}

void apps_table_clear(void)
{
    apps_lock();
    g_apps_count = 0;
    g_apps_loaded = FALSE;
    LeaveCriticalSection(&g_apps_lock);
}

void apps_table_add(const WCHAR* shown_name, const char* bundle_id)
{
    apps_lock();
    if (g_apps_count == g_apps_capacity) {
        int capacity = g_apps_capacity ? g_apps_capacity * 2 : 64;
        AppEntry* grown = (AppEntry*)realloc(g_apps, capacity * sizeof(AppEntry));
        if (!grown) { LeaveCriticalSection(&g_apps_lock); return; }
        g_apps = grown;
        g_apps_capacity = capacity;
    }
    copy_wide(g_apps[g_apps_count].shown, APP_ID_MAX, shown_name);
    copy_text(g_apps[g_apps_count].bundle, APP_ID_MAX, bundle_id);
    g_apps_count++;
    LeaveCriticalSection(&g_apps_lock);
}

void apps_table_done(void)
{
    apps_lock();
    g_apps_loaded = TRUE;
    LeaveCriticalSection(&g_apps_lock);
}

/* Looks a folder name up in the table. Returns FALSE when it is not there. */
static BOOL apps_lookup(const WCHAR* shown_name, char* bundle_id, int bundle_len, BOOL* loaded)
{
    WCHAR as_wide[APP_ID_MAX];
    BOOL found = FALSE;
    int i;

    apps_lock();
    for (i = 0; i < g_apps_count && !found; i++) {
        /* The bundle id itself is accepted too, so old paths keep working. */
        utf8_to_wide(g_apps[i].bundle, as_wide, APP_ID_MAX);
        if (_wcsicmp(g_apps[i].shown, shown_name) == 0 || _wcsicmp(as_wide, shown_name) == 0) {
            copy_text(bundle_id, bundle_len, g_apps[i].bundle);
            found = TRUE;
        }
    }
    *loaded = g_apps_loaded;
    LeaveCriticalSection(&g_apps_lock);
    return found;
}

/*
 * Folder name under \Applications -> bundle id. When Total Commander starts
 * inside an application folder, the table is still empty: load it once.
 * An unknown name is used as a bundle id as it is.
 */
void apps_resolve(const WCHAR* shown_name, char* bundle_id, int bundle_len)
{
    BOOL loaded;

    if (apps_lookup(shown_name, bundle_id, bundle_len, &loaded)) return;
    if (!loaded && apps_load(NULL, 0) && apps_lookup(shown_name, bundle_id, bundle_len, &loaded)) return;
    wide_to_utf8(shown_name, bundle_id, bundle_len);
}

/* ------------------------------------------------------------ path parsing */

BOOL is_afc_mode(PathMode mode)
{
    return mode == M_ROOT || mode == M_USER || mode == M_PHOTOS || mode == M_PROCESSED ||
           mode == M_ICLOUD || mode == M_APP;
}

/* Paths with the same key can share one AFC connection. */
void connection_key(const RemotePath* rp, char* key, int key_len)
{
    if (rp->mode == M_ROOT)      copy_text(key, key_len, "afc2");
    else if (rp->mode == M_APP)  { _snprintf(key, key_len - 1, "app:%s", rp->app); key[key_len - 1] = 0; }
    else                         copy_text(key, key_len, "afc");
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
 * Splits a Total Commander path such as
 *     \Applications\Keynote\Documents\a.key
 * into the mode (M_APP), the application ("com.apple.Keynote") and the path
 * on the device ("/Documents/a.key").
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

    if      (wcscmp(first, L"Root") == 0)             out->mode = M_ROOT;
    else if (wcscmp(first, L"User") == 0)             out->mode = M_USER;
    else if (wcscmp(first, L"Photos") == 0)           { out->mode = M_PHOTOS;    prefix = "/DCIM"; }
    else if (wcscmp(first, L"Processed photos") == 0) { out->mode = M_PROCESSED; prefix = "/DCIM"; }
    else if (wcscmp(first, L"iCloud photos") == 0)    { out->mode = M_ICLOUD;    prefix = ICLOUD_DIR; }
    else if (wcscmp(first, SETTINGS_ITEM) == 0 && *rest == 0) { out->mode = M_SETTINGS; return; }
    else if (wcscmp(first, L"Applications") == 0) {
        WCHAR app_name[APP_ID_MAX];
        if (*rest == 0) { out->mode = M_APPLIST; return; }
        rest = next_component(rest + 1, app_name, APP_ID_MAX);
        out->mode = M_APP;
        apps_resolve(app_name, out->app, APP_ID_MAX);
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
