/*
 * home.c - "Home screen": arrange the icons of the device from Total Commander.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * SpringBoard (the home screen application of iOS) hands out its layout as a
 * property list and accepts a changed one back:
 *
 *     layout     = [ dock, page 1, page 2, ... ]
 *     dock, page = [ item, item, ... ]
 *     item       = application, web clip or widget (a dictionary), or
 *                  a folder: { displayName, listType = "folder",
 *                              iconLists = [ [ item, ... ], ... ] }
 *
 * We show that as folders and files:
 *
 *     \Home screen\Dock\Safari
 *     \Home screen\Page 01\Mail
 *     \Home screen\Page 02\Games\Chess          ("Games" is a folder on the device)
 *     \Home screen\layout.plist                 (the whole layout as a file)
 *
 * Moving an item to another page or folder (F6) moves its dictionary to the
 * other list and sends the layout back. The items themselves are never
 * edited, so everything the device stores in them is returned unchanged.
 *
 * iOS drops empty folders and pages. A folder made with F7 therefore exists
 * only in this plugin ("pending") until something is moved into it.
 *
 * Before every change the layout last read from the device is saved to a
 * file; copying such a file into "Home screen" puts that layout back.
 */
#include "wfx_ios.h"

#define ITEMS_MAX           1024    /* items of one page or folder we can handle */
#define NAME_MAX_UTF8       400
#define FOLDER_PAGE_SIZE    9       /* icons on one page inside a folder */
#define PENDING_MAX         16
#define LAYOUT_FILE         "layout.plist"
#define LAYOUT_SIZE_MAX     (16 * 1024 * 1024)

typedef HomeItem ItemRef;            /* one item together with the list that holds it */

/* A folder made with F7 that has no items yet. */
typedef struct {
    int  page;                      /* index in the layout: 0 = dock, 1 = first page */
    char name[NAME_MAX_UTF8];
} PendingFolder;

static CRITICAL_SECTION g_home_lock;            /* protects everything below */
static plist_t       g_layout = NULL;           /* working copy of the layout */
static char          g_layout_udid[UDID_MAX];   /* device it belongs to */
static char*         g_device_xml = NULL;       /* the layout as last read from the device */
static uint32_t      g_device_xml_len = 0;
static PendingFolder g_pending[PENDING_MAX];
static int           g_pending_count = 0;

void home_init(void)
{
    InitializeCriticalSection(&g_home_lock);
}

/* ------------------------------------------------------------- SpringBoard */

/* Opens a connection to SpringBoard. Close it with springboard_close(). */
static BOOL springboard_open(const char* udid, Device** device, sbservices_client_t* client,
                             WCHAR* err, int err_len)
{
    lockdownd_service_descriptor_t service = NULL;

    *client = NULL;
    *device = device_acquire(udid, FALSE, err, err_len);
    if (!*device) return FALSE;
    if (start_service(*device, SBSERVICES_SERVICE_NAME, &service, err, err_len)) {
        if (sbservices_client_new((*device)->dev, service, client) != SBSERVICES_E_SUCCESS) *client = NULL;
        lockdownd_service_descriptor_free(service);
    }
    if (!*client) {
        if (err && err_len > 0 && err[0] == 0) format_wide(err, err_len, tr(S_ERR_HOME_READ), -1);
        device_release(*device);
        *device = NULL;
        return FALSE;
    }
    return TRUE;
}

static void springboard_close(Device* device, sbservices_client_t client)
{
    sbservices_client_free(client);
    device_release(device);
}

/* ------------------------------------------------------------------ items */

BOOL home_is_folder(plist_t item)
{
    plist_t lists;
    if (plist_get_node_type(item) != PLIST_DICT) return FALSE;
    lists = plist_dict_get_item(item, "iconLists");
    return lists && plist_get_node_type(lists) == PLIST_ARRAY;
}

/* Reads a text value of an item; FALSE when it is missing or empty. */
BOOL home_item_text(plist_t item, const char* key, char* dst, int dst_len)
{
    plist_t node;
    char* value = NULL;

    dst[0] = 0;
    if (plist_get_node_type(item) != PLIST_DICT) return FALSE;
    node = plist_dict_get_item(item, key);
    if (!node || plist_get_node_type(node) != PLIST_STRING) return FALSE;
    plist_get_string_val(node, &value);
    if (value) { copy_text(dst, dst_len, value); plist_mem_free(value); }
    return dst[0] != 0;
}

/* The name of an item as shown on the device, made usable as a file name. */
void home_item_name(plist_t item, char* dst, int dst_len)
{
    WCHAR wide[NAME_MAX_UTF8];
    char text[NAME_MAX_UTF8];
    int len, i;

    if (!home_item_text(item, "displayName", text, sizeof(text)) &&
        !home_item_text(item, "displayIdentifier", text, sizeof(text)) &&
        !home_item_text(item, "bundleIdentifier", text, sizeof(text)))
        copy_text(text, sizeof(text), "Item");

    utf8_to_wide(text, wide, NAME_MAX_UTF8);
    len = (int)wcslen(wide);
    for (i = 0; i < len; i++)
        if (wide[i] < 32 || wcschr(L"\\/:*?\"<>|", wide[i])) wide[i] = L'_';
    while (len > 0 && (wide[len - 1] == L' ' || wide[len - 1] == L'.')) wide[--len] = 0;
    if (len == 0) copy_wide(wide, NAME_MAX_UTF8, L"Item");
    wide_to_utf8(wide, dst, dst_len);
}

/*
 * Lists the items of a container. A page or the dock is one array; a folder
 * has several arrays (its pages), which we show as one list.
 */
int home_collect(plist_t container, HomeItem* items, int items_max)
{
    int count = 0;
    uint32_t i, k;

    if (plist_get_node_type(container) == PLIST_ARRAY) {
        for (i = 0; i < plist_array_get_size(container) && count < items_max; i++) {
            items[count].node = plist_array_get_item(container, i);
            items[count].list = container;
            items[count].index = i;
            count++;
        }
    } else if (home_is_folder(container)) {
        plist_t lists = plist_dict_get_item(container, "iconLists");
        for (k = 0; k < plist_array_get_size(lists); k++) {
            plist_t list = plist_array_get_item(lists, k);
            if (plist_get_node_type(list) != PLIST_ARRAY) continue;
            for (i = 0; i < plist_array_get_size(list) && count < items_max; i++) {
                items[count].node = plist_array_get_item(list, i);
                items[count].list = list;
                items[count].index = i;
                count++;
            }
        }
    }
    return count;
}

/*
 * The file name of item 'index'. Two items of one container can have the
 * same name on the device; the later ones get " (2)", " (3)", ...
 */
static void item_name(const ItemRef* items, int index, char* dst, int dst_len)
{
    char other[NAME_MAX_UTF8];
    int same = 0, i;

    home_item_name(items[index].node, dst, dst_len);
    for (i = 0; i < index; i++) {
        home_item_name(items[i].node, other, sizeof(other));
        if (_stricmp(other, dst) == 0) same++;
    }
    if (same > 0) {
        char numbered[NAME_MAX_UTF8];
        _snprintf(numbered, sizeof(numbered) - 1, "%s (%d)", dst, same + 1);
        numbered[sizeof(numbered) - 1] = 0;
        copy_text(dst, dst_len, numbered);
    }
}

/* ------------------------------------------------------------------ paths */

/* "Dock" for 0, "Page 01" for 1, ... */
static void top_name(int index, char* dst, int dst_len)
{
    if (index == 0) copy_text(dst, dst_len, "Dock");
    else { _snprintf(dst, dst_len - 1, "Page %02d", index); dst[dst_len - 1] = 0; }
}

/* Splits "/Page 01/Games" into "/Page 01" and "Games"; the parent of "/Dock" is "/". */
static void split_path(const char* path, char* parent, int parent_len, char* name, int name_len)
{
    const char* slash = strrchr(path, '/');

    copy_text(parent, parent_len, path);
    if (!slash) { name[0] = 0; return; }
    copy_text(name, name_len, slash + 1);
    if (slash == path) copy_text(parent, parent_len, "/");
    else if ((int)(slash - path) < parent_len) parent[slash - path] = 0;
}

/*
 * Finds the dock or a page by its folder name. With 'create', the page right
 * after the last one is added (that is how a new page is made).
 */
static plist_t find_top(const char* name, BOOL create)
{
    uint32_t count = plist_array_get_size(g_layout);
    char expected[32];
    uint32_t i;

    for (i = 0; i <= count; i++) {
        top_name((int)i, expected, sizeof(expected));
        if (strcmp(name, expected) != 0) continue;
        if (i == count) {
            if (!create) return NULL;
            plist_array_append_item(g_layout, plist_new_array());
        }
        return plist_array_get_item(g_layout, i);
    }
    return NULL;
}

/* Finds a container: "/Page 01" (a page) or "/Page 01/Games" (a folder on it). */
static plist_t find_container(const char* path, BOOL create_page)
{
    static ItemRef items[ITEMS_MAX];        /* caller holds g_home_lock */
    char parent[AFC_PATH_MAX], name[NAME_MAX_UTF8], item[NAME_MAX_UTF8];
    plist_t top;
    int count, i;

    split_path(path, parent, sizeof(parent), name, sizeof(name));
    if (strcmp(parent, "/") == 0) return find_top(name, create_page);

    /* A folder: its parent must be a page or the dock, folders do not nest. */
    if (strchr(parent + 1, '/')) return NULL;
    top = find_top(parent + 1, FALSE);
    if (!top) return NULL;
    count = home_collect(top, items, ITEMS_MAX);
    for (i = 0; i < count; i++) {
        item_name(items, i, item, sizeof(item));
        if (strcmp(item, name) == 0) return home_is_folder(items[i].node) ? items[i].node : NULL;
    }
    return NULL;
}

/* Finds one item by its path, e.g. "/Page 02/Games/Chess". */
static BOOL find_item(const char* path, ItemRef* found)
{
    static ItemRef items[ITEMS_MAX];        /* caller holds g_home_lock */
    char parent[AFC_PATH_MAX], name[NAME_MAX_UTF8], item[NAME_MAX_UTF8];
    plist_t container;
    int count, i;

    split_path(path, parent, sizeof(parent), name, sizeof(name));
    if (strcmp(parent, "/") == 0) return FALSE;         /* the dock and pages are not items */
    container = find_container(parent, FALSE);
    if (!container) return FALSE;
    count = home_collect(container, items, ITEMS_MAX);
    for (i = 0; i < count; i++) {
        item_name(items, i, item, sizeof(item));
        if (strcmp(item, name) == 0) { *found = items[i]; return TRUE; }
    }
    return FALSE;
}

/* --------------------------------------------------------- pending folders */

plist_t home_new_folder(const char* name)
{
    plist_t folder = plist_new_dict();
    plist_t lists = plist_new_array();

    plist_array_append_item(lists, plist_new_array());
    plist_dict_set_item(folder, "displayName", plist_new_string(name));
    plist_dict_set_item(folder, "listType", plist_new_string("folder"));
    plist_dict_set_item(folder, "iconLists", lists);
    return folder;
}

static int pending_find(int page, const char* name)
{
    int i;
    for (i = 0; i < g_pending_count; i++)
        if (g_pending[i].page == page && strcmp(g_pending[i].name, name) == 0) return i;
    return -1;
}

static void pending_remove(int index)
{
    if (index < 0) return;
    g_pending[index] = g_pending[--g_pending_count];
}

/* Index of a page (or the dock) in the layout; -1 when 'list' is not one. */
static int top_index(plist_t list)
{
    uint32_t i;
    for (i = 0; i < plist_array_get_size(g_layout); i++)
        if (plist_array_get_item(g_layout, i) == list) return (int)i;
    return -1;
}

/* After reading the layout: puts the still empty F7 folders back into it. */
static void pending_restore(void)
{
    static ItemRef items[ITEMS_MAX];
    char name[NAME_MAX_UTF8];
    int p = 0;

    while (p < g_pending_count) {
        uint32_t pages = plist_array_get_size(g_layout);
        plist_t page;
        BOOL exists = FALSE;
        int count, i;

        if (g_pending[p].page == (int)pages) plist_array_append_item(g_layout, plist_new_array());
        if (g_pending[p].page > (int)pages) { pending_remove(p); continue; }
        page = plist_array_get_item(g_layout, (uint32_t)g_pending[p].page);

        /* Something was moved into it: the device has the folder now. */
        count = home_collect(page, items, ITEMS_MAX);
        for (i = 0; i < count && !exists; i++) {
            home_item_name(items[i].node, name, sizeof(name));
            exists = home_is_folder(items[i].node) && strcmp(name, g_pending[p].name) == 0;
        }
        if (exists) { pending_remove(p); continue; }
        plist_array_append_item(page, home_new_folder(g_pending[p].name));
        p++;
    }
}

/* ------------------------------------------------------ reading the layout */

/* Reads the layout from the device into g_layout. Caller holds g_home_lock. */
static BOOL layout_load(const char* udid, WCHAR* err, int err_len)
{
    Device* device;
    sbservices_client_t client;
    plist_t layout = NULL;
    sbservices_error_t e;

    if (!springboard_open(udid, &device, &client, err, err_len)) return FALSE;
    e = sbservices_get_icon_state(client, &layout, "2");
    springboard_close(device, client);

    if (e != SBSERVICES_E_SUCCESS || !layout ||
        plist_get_node_type(layout) != PLIST_ARRAY || plist_array_get_size(layout) == 0) {
        if (layout) plist_free(layout);
        log_line("home: reading the layout failed (%d)", (int)e);
        if (err && err_len > 0) format_wide(err, err_len, tr(S_ERR_HOME_READ), (int)e);
        return FALSE;
    }

    if (g_layout) plist_free(g_layout);
    if (g_device_xml) { plist_mem_free(g_device_xml); g_device_xml = NULL; g_device_xml_len = 0; }
    if (strcmp(g_layout_udid, udid) != 0) g_pending_count = 0;     /* another device */
    g_layout = layout;
    copy_text(g_layout_udid, UDID_MAX, udid);
    plist_to_xml(g_layout, &g_device_xml, &g_device_xml_len);
    pending_restore();
    return TRUE;
}

/* Makes sure g_layout holds the layout of this device. */
static BOOL layout_ensure(const char* udid, WCHAR* err, int err_len)
{
    if (g_layout && strcmp(g_layout_udid, udid) == 0) return TRUE;
    return layout_load(udid, err, err_len);
}

/* ------------------------------------------------------ writing the layout */

/* Saves the layout last read from the device to a file; 'path' receives its name. */
static void save_backup(WCHAR* path, int path_len)
{
    WCHAR folder[MAX_PATH];
    SYSTEMTIME now;
    HANDLE file;
    DWORD written;

    path[0] = 0;
    if (!g_device_xml) return;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", folder, MAX_PATH - 16) == 0)
        GetTempPathW(MAX_PATH - 16, folder);
    append_wide(folder, MAX_PATH, L"\\wfx_iOS");
    CreateDirectoryW(folder, NULL);

    GetLocalTime(&now);
    format_wide(path, path_len, L"%s\\home-screen-%04d%02d%02d-%02d%02d%02d.plist", folder,
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { path[0] = 0; return; }
    WriteFile(file, g_device_xml, g_device_xml_len, &written, NULL);
    CloseHandle(file);
}

/* Removes what iOS does not accept: empty folders, and empty pages after the first. */
static void remove_empty(plist_t layout)
{
    static ItemRef items[ITEMS_MAX];
    uint32_t page_index = plist_array_get_size(layout);

    while (page_index-- > 0) {
        plist_t page = plist_array_get_item(layout, page_index);
        uint32_t i = plist_array_get_size(page);

        while (i-- > 0) {
            plist_t item = plist_array_get_item(page, i);
            if (!home_is_folder(item)) continue;
            if (home_collect(item, items, ITEMS_MAX) == 0) {
                plist_array_remove_item(page, i);
            } else {
                /* Also empty pages inside the folder. */
                plist_t lists = plist_dict_get_item(item, "iconLists");
                uint32_t k = plist_array_get_size(lists);
                while (k-- > 0)
                    if (plist_array_get_size(plist_array_get_item(lists, k)) == 0)
                        plist_array_remove_item(lists, k);
            }
        }
        if (page_index >= 2 && plist_array_get_size(page) == 0)
            plist_array_remove_item(layout, page_index);
    }
}

/*
 * Sends g_layout to the device and reads back what the device made of it.
 * On failure the change is dropped and the user is told.
 */
static BOOL layout_write(void)
{
    WCHAR backup[MAX_PATH], message[MSG_MAX], err[MSG_MAX] = L"";
    char udid[UDID_MAX];
    Device* device;
    sbservices_client_t client;
    plist_t to_send;
    sbservices_error_t e = SBSERVICES_E_UNKNOWN_ERROR;

    copy_text(udid, UDID_MAX, g_layout_udid);
    save_backup(backup, MAX_PATH);

    to_send = plist_copy(g_layout);
    remove_empty(to_send);
    if (springboard_open(udid, &device, &client, err, MSG_MAX)) {
        e = sbservices_set_icon_state(client, to_send);
        springboard_close(device, client);
    }
    plist_free(to_send);
    log_line("home: writing the layout -> %d", (int)e);

    if (e == SBSERVICES_E_SUCCESS) Sleep(300);          /* SpringBoard rearranges its icons */
    if (!layout_load(udid, NULL, 0)) {
        plist_free(g_layout);                           /* unknown state: read again on next use */
        g_layout = NULL;
    }
    if (e != SBSERVICES_E_SUCCESS) {
        format_wide(message, MSG_MAX, tr(S_ERR_HOME_WRITE), (int)e, backup);
        show_error(message);
        return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------- listing */

/* Position on the screen as a date, so that sorting by date gives the order on the device. */
static FILETIME order_time(int index)
{
    return unix_ns_to_filetime((uint64_t)(978307200 + index * 60) * 1000000000ULL);
}

/* Lists "\Home screen" or one of the folders below it. */
BOOL home_list(const RemotePath* rp, FindList* list, WCHAR* err, int err_len)
{
    static ItemRef items[ITEMS_MAX];
    WIN32_FIND_DATAW entry;
    BOOL ok = FALSE;
    char name[NAME_MAX_UTF8];
    int count, i;

    EnterCriticalSection(&g_home_lock);
    if (strcmp(rp->path, "/") == 0) {
        /* The top: always read again, the icons may have been moved on the device. */
        if (layout_load(rp->udid, err, err_len)) {
            /* One page more than the device has: moving something there makes a new page. */
            count = (int)plist_array_get_size(g_layout);
            for (i = 0; i <= count; i++) {
                memset(&entry, 0, sizeof(entry));
                top_name(i, name, sizeof(name));
                utf8_to_wide(name, entry.cFileName, MAX_PATH);
                entry.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                entry.ftLastWriteTime = order_time(i);
                find_list_add_entry(list, &entry);
            }
            memset(&entry, 0, sizeof(entry));
            utf8_to_wide(LAYOUT_FILE, entry.cFileName, MAX_PATH);
            entry.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            entry.nFileSizeLow = g_device_xml_len;
            entry.ftLastWriteTime = order_time(count + 1);
            find_list_add_entry(list, &entry);
            ok = TRUE;
        }
    } else if (layout_ensure(rp->udid, err, err_len)) {
        plist_t container = find_container(rp->path, TRUE);
        if (container) {
            count = home_collect(container, items, ITEMS_MAX);
            for (i = 0; i < count; i++) {
                memset(&entry, 0, sizeof(entry));
                item_name(items, i, name, sizeof(name));
                utf8_to_wide(name, entry.cFileName, MAX_PATH);
                entry.dwFileAttributes = home_is_folder(items[i].node) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                entry.ftLastWriteTime = order_time(i);
                find_list_add_entry(list, &entry);
            }
            ok = TRUE;
        } else if (err && err_len > 0) {
            copy_wide(err, err_len, tr(S_ERR_READ_DIR));
        }
    }
    LeaveCriticalSection(&g_home_lock);
    return ok;
}

/* The bundle id of an application item, for its icon. Uses only what is in memory. */
BOOL home_item_bundle(const RemotePath* rp, char* bundle, int bundle_len)
{
    ItemRef item;
    BOOL found = FALSE;

    bundle[0] = 0;
    EnterCriticalSection(&g_home_lock);
    if (g_layout && strcmp(g_layout_udid, rp->udid) == 0 && find_item(rp->path, &item) && !home_is_folder(item.node))
        found = home_item_text(item.node, "bundleIdentifier", bundle, bundle_len) ||
                home_item_text(item.node, "displayIdentifier", bundle, bundle_len);
    LeaveCriticalSection(&g_home_lock);
    return found;
}

/* ----------------------------------------------------------------- moving */

/* Appends an item to a page, the dock or a folder. */
static void container_append(plist_t container, plist_t item)
{
    plist_t lists, last = NULL;
    uint32_t count;

    if (plist_get_node_type(container) == PLIST_ARRAY) {
        plist_array_append_item(container, item);
        return;
    }
    /* A folder: the last of its pages, or a new one when that is full. */
    lists = plist_dict_get_item(container, "iconLists");
    count = plist_array_get_size(lists);
    if (count > 0) last = plist_array_get_item(lists, count - 1);
    if (!last || plist_get_node_type(last) != PLIST_ARRAY || plist_array_get_size(last) >= FOLDER_PAGE_SIZE) {
        last = plist_new_array();
        plist_array_append_item(lists, last);
    }
    plist_array_append_item(last, item);
}

/* Gives a folder another name. An empty (pending) folder is renamed only here. */
static int rename_folder(ItemRef* folder, const char* parent, const char* new_name)
{
    static ItemRef items[ITEMS_MAX];
    char old_name[NAME_MAX_UTF8], path[AFC_PATH_MAX];
    ItemRef other;
    int pending;

    _snprintf(path, sizeof(path) - 1, "%s/%s", parent, new_name);
    path[sizeof(path) - 1] = 0;
    if (find_item(path, &other)) return FS_FILE_EXISTS;

    home_item_name(folder->node, old_name, sizeof(old_name));
    pending = pending_find(top_index(folder->list), old_name);
    if (pending >= 0) copy_text(g_pending[pending].name, NAME_MAX_UTF8, new_name);
    plist_dict_set_item(folder->node, "displayName", plist_new_string(new_name));

    if (home_collect(folder->node, items, ITEMS_MAX) == 0) return FS_FILE_OK;
    return layout_write() ? FS_FILE_OK : FS_FILE_WRITEERROR;
}

/*
 * F6 inside "Home screen": moves an item to another page or folder, or
 * renames a folder. Applications cannot be renamed.
 */
int home_move(const RemotePath* src, const RemotePath* dst)
{
    char src_parent[AFC_PATH_MAX], src_name[NAME_MAX_UTF8];
    char dst_parent[AFC_PATH_MAX], dst_name[NAME_MAX_UTF8];
    ItemRef item;
    plist_t target, moved;
    int result = FS_FILE_NOTSUPPORTED;

    if (strcmp(src->udid, dst->udid) != 0) return FS_FILE_NOTSUPPORTED;
    split_path(src->path, src_parent, sizeof(src_parent), src_name, sizeof(src_name));
    split_path(dst->path, dst_parent, sizeof(dst_parent), dst_name, sizeof(dst_name));

    EnterCriticalSection(&g_home_lock);
    if (!layout_ensure(src->udid, NULL, 0)) {
        result = FS_FILE_READERROR;
    } else if (!find_item(src->path, &item)) {
        result = FS_FILE_NOTFOUND;
    } else if (strcmp(src_parent, dst_parent) == 0) {
        /* Same place: a new name. */
        if (strcmp(src_name, dst_name) == 0) result = FS_FILE_OK;
        else if (home_is_folder(item.node))       result = rename_folder(&item, src_parent, dst_name);
    } else {
        target = find_container(dst_parent, TRUE);
        if (!target) {
            result = FS_FILE_NOTFOUND;
        } else if (home_is_folder(item.node) && plist_get_node_type(target) != PLIST_ARRAY) {
            result = FS_FILE_NOTSUPPORTED;              /* no folders inside folders */
        } else {
            /* An empty folder that moves stays pending, on its new page. */
            if (home_is_folder(item.node)) {
                int pending = pending_find(top_index(item.list), src_name);
                if (pending >= 0) g_pending[pending].page = top_index(target);
            }
            moved = plist_copy(item.node);
            plist_array_remove_item(item.list, item.index);
            container_append(target, moved);
            result = layout_write() ? FS_FILE_OK : FS_FILE_WRITEERROR;
        }
    }
    LeaveCriticalSection(&g_home_lock);
    return result;
}

/* ---------------------------------------------------------------- folders */

/* F7 on a page: a new folder. It reaches the device with the first item moved into it. */
BOOL home_make_folder(const RemotePath* rp)
{
    char parent[AFC_PATH_MAX], name[NAME_MAX_UTF8];
    ItemRef existing;
    plist_t page;
    BOOL made = FALSE;

    split_path(rp->path, parent, sizeof(parent), name, sizeof(name));
    if (strcmp(parent, "/") == 0 || strchr(parent + 1, '/') || name[0] == 0) return FALSE;

    EnterCriticalSection(&g_home_lock);
    if (layout_ensure(rp->udid, NULL, 0) && !find_item(rp->path, &existing) && g_pending_count < PENDING_MAX) {
        page = find_top(parent + 1, TRUE);
        if (page) {
            plist_array_append_item(page, home_new_folder(name));
            g_pending[g_pending_count].page = top_index(page);
            copy_text(g_pending[g_pending_count].name, NAME_MAX_UTF8, name);
            g_pending_count++;
            made = TRUE;
        }
    }
    LeaveCriticalSection(&g_home_lock);
    return made;
}

/* F8 on a folder: only an empty one can go; move its items out first. */
BOOL home_remove_folder(const RemotePath* rp)
{
    static ItemRef items[ITEMS_MAX];
    char name[NAME_MAX_UTF8];
    ItemRef item;
    BOOL removed = FALSE;

    EnterCriticalSection(&g_home_lock);
    if (g_layout && strcmp(g_layout_udid, rp->udid) == 0 && find_item(rp->path, &item) &&
        home_is_folder(item.node) && home_collect(item.node, items, ITEMS_MAX) == 0) {
        home_item_name(item.node, name, sizeof(name));
        pending_remove(pending_find(top_index(item.list), name));
        plist_array_remove_item(item.list, item.index);
        removed = TRUE;
    }
    LeaveCriticalSection(&g_home_lock);
    return removed;
}

/* ------------------------------------------------------- layout as a file */

BOOL home_is_layout_file(const RemotePath* rp)
{
    const char* dot = strrchr(rp->path, '.');
    return rp->mode == M_HOME && strchr(rp->path + 1, '/') == NULL && dot && _stricmp(dot, ".plist") == 0;
}

/* F5 on "layout.plist": writes the current layout of the device to a local file. */
int home_download_layout(const RemotePath* rp, const WCHAR* local_name, int copy_flags)
{
    int result = FS_FILE_READERROR;
    HANDLE file;
    DWORD written;

    if (!(copy_flags & FS_COPYFLAGS_OVERWRITE) && GetFileAttributesW(local_name) != INVALID_FILE_ATTRIBUTES)
        return FS_FILE_EXISTS;

    EnterCriticalSection(&g_home_lock);
    if (layout_load(rp->udid, NULL, 0) && g_device_xml) {
        file = CreateFileW(local_name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            result = FS_FILE_WRITEERROR;
        } else {
            result = WriteFile(file, g_device_xml, g_device_xml_len, &written, NULL) ? FS_FILE_OK : FS_FILE_WRITEERROR;
            CloseHandle(file);
        }
    }
    LeaveCriticalSection(&g_home_lock);
    return result;
}

/* A layout must be a list of lists; anything else is not sent to the device. */
BOOL home_layout_is_valid(plist_t layout)
{
    uint32_t i;
    if (!layout || plist_get_node_type(layout) != PLIST_ARRAY || plist_array_get_size(layout) < 2) return FALSE;
    for (i = 0; i < plist_array_get_size(layout); i++)
        if (plist_get_node_type(plist_array_get_item(layout, i)) != PLIST_ARRAY) return FALSE;
    return TRUE;
}

/* Reads a layout from a file (text or binary property list). NULL when it is not one. */
plist_t home_load_layout_file(const WCHAR* local_name)
{
    HANDLE file = CreateFileW(local_name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    char* data;
    plist_t layout = NULL;

    if (file == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(file, NULL);
    data = (size > 0 && size < LAYOUT_SIZE_MAX) ? (char*)malloc(size) : NULL;
    if (data && ReadFile(file, data, size, &got, NULL) && got == size) {
        if (size > 8 && memcmp(data, "bplist00", 8) == 0) plist_from_bin(data, size, &layout);
        else                                               plist_from_xml(data, size, &layout);
    }
    CloseHandle(file);
    free(data);
    if (layout && !home_layout_is_valid(layout)) { plist_free(layout); layout = NULL; }
    return layout;
}

/* F5 of a saved layout file into "Home screen": puts that layout back on the device. */
int home_upload_layout(const WCHAR* local_name, const RemotePath* rp)
{
    plist_t layout = home_load_layout_file(local_name);
    int result;

    if (!layout) {
        show_error(tr(S_ERR_HOME_FILE));
        return FS_FILE_NOTSUPPORTED;
    }
    EnterCriticalSection(&g_home_lock);
    if (!layout_load(rp->udid, NULL, 0)) {              /* also: the backup is the state of right now */
        plist_free(layout);
        result = FS_FILE_READERROR;
    } else {
        plist_free(g_layout);
        g_layout = layout;
        g_pending_count = 0;
        result = layout_write() ? FS_FILE_OK : FS_FILE_WRITEERROR;
    }
    LeaveCriticalSection(&g_home_lock);
    return result;
}

/* ------------------------------------------- for the arrangement window */

/* The layout of the device, freshly read. The caller frees it with plist_free(). */
plist_t home_read_layout(const char* udid, WCHAR* err, int err_len)
{
    plist_t layout = NULL;

    EnterCriticalSection(&g_home_lock);
    if (layout_load(udid, err, err_len) && g_device_xml)
        plist_from_xml(g_device_xml, g_device_xml_len, &layout);
    LeaveCriticalSection(&g_home_lock);
    return layout;
}

/* Sends a layout to the device, after saving the present one. 'layout' stays the caller's. */
BOOL home_apply_layout(const char* udid, plist_t layout)
{
    BOOL applied = FALSE;

    EnterCriticalSection(&g_home_lock);
    if (layout_load(udid, NULL, 0)) {                   /* the backup is the state of right now */
        plist_free(g_layout);
        g_layout = plist_copy(layout);
        g_pending_count = 0;
        applied = layout_write();
    }
    LeaveCriticalSection(&g_home_lock);
    return applied;
}
