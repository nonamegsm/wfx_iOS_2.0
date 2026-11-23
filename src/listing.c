/*
 * listing.c - directory listings: the plugin root, the application list and
 *             real folders on the device.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "wfx_ios.h"

/* -------------------------------------------------------------- find list */

void find_list_free(FindList* list)
{
    if (!list) return;
    free(list->items);
    free(list);
}

/* Appends one entry; the array grows as needed. */
void find_list_add_entry(FindList* list, const WIN32_FIND_DATAW* entry)
{
    if (list->count == list->capacity) {
        int capacity = list->capacity ? list->capacity * 2 : 64;
        WIN32_FIND_DATAW* grown = (WIN32_FIND_DATAW*)realloc(list->items, capacity * sizeof(WIN32_FIND_DATAW));
        if (!grown) return;
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = *entry;
}

/* A folder that exists only in the plugin (no date, no size). */
void find_list_add_folder(FindList* list, const WCHAR* name)
{
    WIN32_FIND_DATAW entry;
    memset(&entry, 0, sizeof(entry));
    copy_wide(entry.cFileName, MAX_PATH, name);
    entry.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    entry.ftLastWriteTime = unix_ns_to_filetime(0);
    find_list_add_entry(list, &entry);
}

/* An entry for a real file or folder on the device. */
static void add_device_entry(FindList* list, const char* name, const FileStat* st)
{
    WIN32_FIND_DATAW entry;
    memset(&entry, 0, sizeof(entry));
    utf8_to_wide(name, entry.cFileName, MAX_PATH);
    entry.dwFileAttributes = st->is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (st->is_link)    entry.dwFileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    if (name[0] == '.') entry.dwFileAttributes |= FILE_ATTRIBUTE_HIDDEN;
    if (!st->is_dir) {
        entry.nFileSizeLow = (DWORD)st->size;
        entry.nFileSizeHigh = (DWORD)(st->size >> 32);
    }
    entry.ftLastWriteTime = unix_ns_to_filetime(st->mtime);
    entry.ftCreationTime = unix_ns_to_filetime(st->birth);
    find_list_add_entry(list, &entry);
}

/* ------------------------------------------------------------ plugin root */

static afc_error_t op_stat(afc_client_t afc, void* arg);    /* defined below */

typedef struct {
    const char* path;
    FileStat*   st;
    BOOL        follow_links;
} StatArgs;

/* Adds a pseudo file ("Settings", "Device information"); Enter on it opens a window. */
static void add_pseudo_file(FindList* list, const WCHAR* name)
{
    WIN32_FIND_DATAW entry;
    memset(&entry, 0, sizeof(entry));
    copy_wide(entry.cFileName, MAX_PATH, name);
    entry.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    entry.ftLastWriteTime = unix_ns_to_filetime(0);
    find_list_add_entry(list, &entry);
}

/* The folders of one device ('udid' empty: the default device). */
static void list_device_root(FindList* list, const char* udid)
{
    RemotePath user;
    FileStat st;
    StatArgs args;

    if (device_has_afc2(udid)) find_list_add_folder(list, L"Root");
    find_list_add_folder(list, L"User");
    find_list_add_folder(list, L"Photos");
    find_list_add_folder(list, L"Processed photos");

    /* Photos that came from iCloud Photos are stored outside /DCIM; show them when present. */
    memset(&user, 0, sizeof(user));
    user.mode = M_USER;
    copy_text(user.udid, UDID_MAX, udid);
    copy_text(user.path, AFC_PATH_MAX, "/");
    args.path = ICLOUD_DIR;
    args.st = &st;
    args.follow_links = FALSE;
    if (afc_run(&user, op_stat, &args, NULL, 0) == AFC_E_SUCCESS && st.is_dir)
        find_list_add_folder(list, L"iCloud photos");
    if (photos_available(udid)) find_list_add_folder(list, MONTHS_FOLDER);

    find_list_add_folder(list, L"Applications");
    find_list_add_folder(list, HOME_FOLDER);
    add_pseudo_file(list, ARRANGE_ITEM);
    add_pseudo_file(list, INFO_ITEM);
}

/* ------------------------------------------------------------ applications */

/* Reads one text value of a dictionary; 'dst' becomes empty when it is missing. */
static void dict_get_text(plist_t dict, const char* key, char* dst, int dst_len)
{
    plist_t node = plist_dict_get_item(dict, key);
    char* value = NULL;

    dst[0] = 0;
    if (!node || plist_get_node_type(node) != PLIST_STRING) return;
    plist_get_string_val(node, &value);
    if (value) {
        copy_text(dst, dst_len, value);
        plist_mem_free(value);
    }
}

/* Makes an application's display name usable as a folder name. */
static void make_folder_name(const char* display_name, WCHAR* dst, int dst_len)
{
    int len, i;

    utf8_to_wide(display_name, dst, dst_len);
    len = (int)wcslen(dst);
    for (i = 0; i < len; i++)
        if (dst[i] < 32 || wcschr(L"\\/:*?\"<>|", dst[i])) dst[i] = L'_';
    while (len > 0 && (dst[len - 1] == L' ' || dst[len - 1] == L'.')) dst[--len] = 0;
    while (dst[0] == L' ') memmove(dst, dst + 1, wcslen(dst) * sizeof(WCHAR));
}

/* --------------------------------------------------------- device folders */

#define CONNECTED_MAX 16

/* One connected device as reported by usbmuxd. */
typedef struct {
    char  udid[UDID_MAX];
    WCHAR shown[128];               /* folder name for the plugin root */
} ConnectedDevice;

/*
 * Lists the connected devices (USB, and Wi-Fi when allowed), each once, and
 * gives every one a folder name: its device name, made unique and valid.
 */
static int enumerate_devices(ConnectedDevice* devices, int max_devices)
{
    usbmuxd_device_info_t* list = NULL;
    BOOL wifi = ini_get_bool("AllowWiFi", FALSE);
    int reported = usbmuxd_get_device_list(&list);
    int count = 0;
    int i, k;

    for (i = 0; i < reported && count < max_devices; i++) {
        BOOL known = FALSE;
        WCHAR name[128];
        char name_utf8[256];

        if (list[i].conn_type == CONNECTION_TYPE_NETWORK && !wifi) continue;
        for (k = 0; k < count; k++)
            if (strcmp(devices[k].udid, list[i].udid) == 0) known = TRUE;
        if (known) continue;                /* the same device over USB and Wi-Fi */

        copy_text(devices[count].udid, UDID_MAX, list[i].udid);
        device_read_name(list[i].udid, name, 128);
        wide_to_utf8(name, name_utf8, sizeof(name_utf8));
        make_folder_name(name_utf8, devices[count].shown, 100);
        if (devices[count].shown[0] == 0) utf8_to_wide(list[i].udid, devices[count].shown, 100);

        /* The name must not clash with our own folders or with another device. */
        known = is_reserved_name(devices[count].shown);
        for (k = 0; k < count; k++)
            if (_wcsicmp(devices[k].shown, devices[count].shown) == 0) known = TRUE;
        if (known) {
            WCHAR tail[16];
            size_t udid_len = strlen(list[i].udid);
            utf8_to_wide(list[i].udid + (udid_len > 6 ? udid_len - 6 : 0), tail, 16);
            append_wide(devices[count].shown, 128, L" (%s)", tail);
        }
        count++;
    }
    if (list) usbmuxd_device_list_free(&list);
    return count;
}

/* Reads the connected devices into the "folder name -> UDID" table. Returns how many there are. */
static int load_device_table(ConnectedDevice* devices, int max_devices)
{
    int count = enumerate_devices(devices, max_devices);
    int i;

    devices_table_clear();
    for (i = 0; i < count; i++) devices_table_add(devices[i].shown, devices[i].udid);
    return count;
}

void devices_load(void)
{
    ConnectedDevice devices[CONNECTED_MAX];
    load_device_table(devices, CONNECTED_MAX);
}

/*
 * The plugin root. Normally it shows the folders of the one device. With
 * several devices connected, and none picked in the settings, it shows one
 * folder per device instead.
 */
static void list_top(FindList* list)
{
    ConnectedDevice devices[CONNECTED_MAX];
    char preferred[UDID_MAX];
    int count, i;

    ini_get_text("PreferredDevice", preferred, sizeof(preferred));
    count = load_device_table(devices, CONNECTED_MAX);

    if (count >= 2 && preferred[0] == 0) {
        for (i = 0; i < count; i++) find_list_add_folder(list, devices[i].shown);
    } else {
        list_device_root(list, "");
    }
    add_pseudo_file(list, SETTINGS_ITEM);
}

/* Asks installation_proxy for the user-installed applications. Returns a plist array or NULL. */
static plist_t browse_applications(Device* device, WCHAR* err, int err_len)
{
    lockdownd_service_descriptor_t service = NULL;
    instproxy_client_t proxy = NULL;
    plist_t options, apps = NULL;
    instproxy_error_t e;

    if (!start_service(device, INSTPROXY_SERVICE_NAME, &service, err, err_len)) return NULL;
    e = instproxy_client_new(device->dev, service, &proxy);
    lockdownd_service_descriptor_free(service);
    if (e != INSTPROXY_E_SUCCESS || !proxy) {
        if (err) copy_wide(err, err_len, tr(S_ERR_INSTPROXY));
        return NULL;
    }

    options = instproxy_client_options_new();
    instproxy_client_options_add(options, "ApplicationType", "User", NULL);
    instproxy_client_options_set_return_attributes(options, "CFBundleIdentifier", "UIFileSharingEnabled",
                                                   "CFBundleDisplayName", "CFBundleName", NULL);
    e = instproxy_browse(proxy, options, &apps);
    instproxy_client_options_free(options);
    instproxy_client_free(proxy);

    if (e != INSTPROXY_E_SUCCESS || !apps) {
        if (err) copy_wide(err, err_len, tr(S_ERR_APP_LIST));
        return NULL;
    }
    return apps;
}

/* Sorts the entries of a list by name, ignoring case. */
static int compare_entries(const void* a, const void* b)
{
    return _wcsicmp(((const WIN32_FIND_DATAW*)a)->cFileName, ((const WIN32_FIND_DATAW*)b)->cFileName);
}

/*
 * Reads the applications from one device, rebuilds its name table and, when
 * 'list' is given, adds one folder per application to it.
 *
 * From iOS 8 on only applications with file sharing can be opened, so only
 * those are listed (ShowAllApps=1 lists all of them).
 */
static BOOL read_applications(FindList* list, const char* udid, WCHAR* err, int err_len)
{
    typedef struct { WCHAR shown[APP_ID_MAX]; char bundle[APP_ID_MAX]; BOOL duplicate; } Found;
    Device* device = NULL;
    plist_t apps = NULL;
    Found* found;
    int found_count = 0;
    BOOL list_all, use_names;
    uint32_t total, n;
    int attempt, i, k;

    /* Second attempt looks the device up again (it may have been replugged). */
    for (attempt = 0; attempt < 2 && !apps; attempt++) {
        device = device_acquire(udid, attempt == 1, err, err_len);
        if (!device) continue;
        apps = browse_applications(device, err, err_len);
        if (!apps) { device_release(device); device = NULL; }
    }
    if (!apps) return FALSE;

    list_all = device->ios_major < 8 || ini_get_bool("ShowAllApps", FALSE);
    use_names = ini_get_bool("AppNames", TRUE);
    device_release(device);

    total = plist_array_get_size(apps);
    found = (Found*)calloc(total ? total : 1, sizeof(Found));

    for (n = 0; n < total && found; n++) {
        plist_t app = plist_array_get_item(apps, n);
        plist_t sharing_node;
        char bundle[APP_ID_MAX], display[APP_ID_MAX];
        uint8_t sharing = 0;

        if (!app || plist_get_node_type(app) != PLIST_DICT) continue;
        dict_get_text(app, "CFBundleIdentifier", bundle, sizeof(bundle));
        if (!bundle[0]) continue;
        dict_get_text(app, "CFBundleDisplayName", display, sizeof(display));
        if (!display[0]) dict_get_text(app, "CFBundleName", display, sizeof(display));

        sharing_node = plist_dict_get_item(app, "UIFileSharingEnabled");
        if (sharing_node && plist_get_node_type(sharing_node) == PLIST_BOOLEAN)
            plist_get_bool_val(sharing_node, &sharing);
        log_line("app %s (%s), file sharing %d", bundle, display, (int)sharing);
        if (!list_all && !sharing) continue;

        if (use_names) make_folder_name(display, found[found_count].shown, APP_ID_MAX);
        if (!found[found_count].shown[0]) utf8_to_wide(bundle, found[found_count].shown, APP_ID_MAX);
        copy_text(found[found_count].bundle, APP_ID_MAX, bundle);
        found_count++;
    }
    plist_free(apps);
    if (!found) return FALSE;

    /* Two applications with the same name: add the bundle id to both.
       Mark first and rename afterwards, so that all comparisons use the original names. */
    for (i = 0; i < found_count; i++)
        for (k = 0; k < found_count; k++)
            if (k != i && _wcsicmp(found[i].shown, found[k].shown) == 0) found[i].duplicate = TRUE;
    for (i = 0; i < found_count; i++) {
        if (found[i].duplicate) {
            WCHAR id[APP_ID_MAX];
            utf8_to_wide(found[i].bundle, id, APP_ID_MAX);
            append_wide(found[i].shown, APP_ID_MAX, L" (%s)", id);
        }
    }

    apps_table_clear(udid);
    for (i = 0; i < found_count; i++) {
        apps_table_add(udid, found[i].shown, found[i].bundle);
        if (list) find_list_add_folder(list, found[i].shown);
    }
    apps_table_done(udid);
    free(found);

    if (list && list->count > 1)
        qsort(list->items, list->count, sizeof(WIN32_FIND_DATAW), compare_entries);
    return TRUE;
}

BOOL apps_load(const char* udid, WCHAR* err, int err_len)
{
    return read_applications(NULL, udid, err, err_len);
}

/* ---------------------------------------------------------- device folders */

static afc_error_t op_stat(afc_client_t afc, void* arg)
{
    StatArgs* args = (StatArgs*)arg;
    return args->follow_links ? afc_stat_follow(afc, args->path, args->st)
                              : afc_stat(afc, args->path, args->st);
}

typedef struct {
    const char* path;
    FindList*   list;
} ListArgs;

/* Reads one folder: the names first, then the attributes of every entry. */
static afc_error_t op_list_folder(afc_client_t afc, void* arg)
{
    ListArgs* args = (ListArgs*)arg;
    char** names = NULL;
    afc_error_t e;
    int i;

    e = afc_read_directory(afc, args->path, &names);
    if (e != AFC_E_SUCCESS) return e;

    args->list->count = 0;                  /* a repeated attempt starts from scratch */
    for (i = 0; names && names[i]; i++) {
        char full[AFC_PATH_MAX];
        FileStat st;

        if (strcmp(names[i], ".") == 0 || strcmp(names[i], "..") == 0) continue;
        path_join(args->path, names[i], full, sizeof(full));
        e = afc_stat_follow(afc, full, &st);
        if (afc_is_connection_error(e)) { afc_dictionary_free(names); return e; }
        add_device_entry(args->list, names[i], &st);    /* unreadable entries are still listed */
    }
    if (names) afc_dictionary_free(names);
    return AFC_E_SUCCESS;
}

static BOOL list_folder(const RemotePath* rp, FindList* list, WCHAR* err, int err_len)
{
    ListArgs args;
    afc_error_t e;

    args.path = rp->path;
    args.list = list;
    e = afc_run(rp, op_list_folder, &args, err, err_len);
    if (e == AFC_E_SUCCESS) return TRUE;

    /* An application opened with VendDocuments exposes only /Documents;
       its top folder cannot be listed. Show that one folder instead. */
    if (rp->mode == M_APP && strcmp(rp->path, "/") == 0 && e == AFC_E_PERM_DENIED) {
        FileStat st;
        StatArgs stat_args;
        stat_args.path = "/Documents";
        stat_args.st = &st;
        stat_args.follow_links = TRUE;
        if (afc_run(rp, op_stat, &stat_args, err, err_len) == AFC_E_SUCCESS) {
            st.is_dir = TRUE;
            list->count = 0;
            add_device_entry(list, "Documents", &st);
            return TRUE;
        }
    }

    log_line("AFC error %d for %s", (int)e, rp->path);
    if (err && err[0] == 0 && e != AFC_E_OBJECT_NOT_FOUND && e != AFC_E_PERM_DENIED && e != AFC_E_READ_ERROR)
        format_wide(err, err_len, tr(S_ERR_READ_DIR), (int)e);
    return FALSE;
}

/* --------------------------------------------------------------- dispatch */

/* Builds the listing of one Total Commander path. NULL means "cannot be read". */
FindList* find_list_create(const WCHAR* tc_path, WCHAR* err, int err_len)
{
    FindList* list = (FindList*)calloc(1, sizeof(FindList));
    RemotePath rp;
    char utf8[TC_PATH_MAX * 3];
    BOOL ok = FALSE;

    if (!list) return NULL;
    if (err && err_len > 0) err[0] = 0;
    parse_path(tc_path, &rp);

    switch (rp.mode) {
    case M_TOP:
        if (rp.udid[0]) list_device_root(list, rp.udid);    /* inside a device folder */
        else            list_top(list);
        ok = TRUE;
        break;
    case M_APPLIST: ok = read_applications(list, rp.udid, err, err_len); break;
    case M_MONTHS:
        /* rp.path is "/" for the list of months and "/2026-11" for one month. */
        if (strcmp(rp.path, "/") == 0) ok = photos_list_months(rp.udid, list, err, err_len);
        else                           ok = photos_list_month(rp.udid, rp.path + 1, list, err, err_len);
        break;
    case M_HOME:    ok = home_list(&rp, list, err, err_len); break;
    case M_INVALID:
    case M_SETTINGS:
    case M_INFO:
    case M_ARRANGE: break;
    default:        ok = list_folder(&rp, list, err, err_len); break;
    }

    wide_to_utf8(tc_path, utf8, sizeof(utf8));
    log_line("list %s -> %s (%d entries)", utf8, ok ? "ok" : "FAILED", list->count);

    if (!ok) { find_list_free(list); return NULL; }
    return list;
}
