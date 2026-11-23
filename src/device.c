/*
 * device.c - everything that talks to the device:
 *            finding it, the lockdown handshake, starting services,
 *            and the AFC (file access) connections.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Connection model
 * ----------------
 * usbmuxd (Apple Mobile Device Service) lists the devices. For each service
 * we need, we open a lockdown session, ask it to start the service, and then
 * connect to the port it returns:
 *
 *   com.apple.afc                 files of the media partition
 *   com.apple.afc2                whole file system (jailbreak only)
 *   com.apple.mobile.house_arrest files of one application, then speaks AFC
 *
 * Short operations (list, stat, delete, ...) share one cached connection per
 * target; file transfers open their own so they can run in the background.
 */
#include "wfx_ios.h"

#define CACHE_MAX   8                       /* cached AFC connections */
#define DEVICES_MAX 8                       /* devices open at the same time */

static CRITICAL_SECTION g_device_lock;      /* protects g_devices */
static Device*          g_devices[DEVICES_MAX];     /* open devices (each holds one reference) */
static int              g_devices_count = 0;

static CRITICAL_SECTION g_cache_lock;       /* protects g_cache and serialises short operations */
static AfcConn*         g_cache[CACHE_MAX];
static int              g_cache_count = 0;

static LONG g_drop_cache = 0;               /* device vanished: cached connections are dead */
LONG        g_device_seen = 0;              /* see show_error() in util.c */

void device_init(void)
{
    InitializeCriticalSection(&g_device_lock);
    InitializeCriticalSection(&g_cache_lock);
}

/* ------------------------------------------------------------ the device */

void device_release(Device* device)
{
    if (device && InterlockedDecrement(&device->refs) == 0) {
        idevice_free(device->dev);
        free(device);
    }
}

/* Asks usbmuxd whether the device we use is still connected the same way. */
static BOOL device_is_present(const Device* device)
{
    usbmuxd_device_info_t* list = NULL;
    int count = usbmuxd_get_device_list(&list);
    BOOL present = FALSE;
    int i;

    for (i = 0; i < count; i++) {
        BOOL network = list[i].conn_type == CONNECTION_TYPE_NETWORK;
        if (strcmp(device->udid, list[i].udid) == 0 && network == device->network) present = TRUE;
    }
    if (list) usbmuxd_device_list_free(&list);
    return present;
}

/* Explains why no device could be opened. */
static void describe_missing_device(const char* wanted, idevice_error_t error, WCHAR* err, int err_len)
{
    usbmuxd_device_info_t* list = NULL;
    int count = usbmuxd_get_device_list(&list);
    int i;

    if (count < 0) {
        format_wide(err, err_len,
            L"Cannot reach the Apple Mobile Device Service (usbmuxd, 127.0.0.1:27015), error %d.\n"
            L"Install Apple Mobile Device Support and make sure the \"Apple Mobile Device Service\" is running.",
            count);
    } else if (count == 0) {
        format_wide(err, err_len,
            L"No iOS device is connected.\n"
            L"Connect and unlock the device. (The Apple Mobile Device Service is running but reports no device.)");
    } else {
        if (wanted[0])
            format_wide(err, err_len, L"Device %hs is not connected (idevice error %d).", wanted, (int)error);
        else
            format_wide(err, err_len, L"Could not open the device (idevice error %d).", (int)error);
        append_wide(err, err_len, L"\nDevices reported by the service:");
        for (i = 0; i < count; i++)
            append_wide(err, err_len, L"\n  %hs (%s)", list[i].udid,
                        list[i].conn_type == CONNECTION_TYPE_NETWORK ? L"network" : L"USB");
    }
    if (list) usbmuxd_device_list_free(&list);
}

/* Index of the open device for 'udid' ("" = the default device), or -1. Caller holds g_device_lock. */
static int find_open_device(const char* udid)
{
    int i;
    for (i = 0; i < g_devices_count; i++) {
        if (udid[0] == 0 ? g_devices[i]->is_default
                         : (!g_devices[i]->is_default && strcmp(g_devices[i]->udid, udid) == 0))
            return i;
    }
    return -1;
}

/* Closes one open device. Caller holds g_device_lock. */
static void close_open_device(int index)
{
    device_release(g_devices[index]);
    memmove(&g_devices[index], &g_devices[index + 1], (g_devices_count - index - 1) * sizeof(Device*));
    g_devices_count--;
}

/*
 * Returns the device to use, with one reference added for the caller
 * (give it back with device_release). Returns NULL and fills 'err' when the
 * device is not available.
 *   udid   which device; "" means the default one (the preferred device from
 *          the settings, else the first one found)
 *   fresh  look the device up again even if we have it open
 */
Device* device_acquire(const char* udid, BOOL fresh, WCHAR* err, int err_len)
{
    static BOOL first_use = TRUE;
    char wanted[UDID_MAX];
    idevice_t dev = NULL;
    idevice_error_t error;
    BOOL network = FALSE;
    Device* device;
    char* real_udid = NULL;
    int index;

    if (!udid) udid = "";
    if (err && err_len > 0) err[0] = 0;
    EnterCriticalSection(&g_device_lock);

    if (first_use) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        log_open();
        first_use = FALSE;
    }

    index = find_open_device(udid);
    if (index >= 0 && !fresh) {
        /* A cheap check with the local service, so a pulled cable fails at
           once instead of waiting on a dead connection. At most every 0.7 s. */
        DWORD now = GetTickCount();
        device = g_devices[index];
        if (now - device->last_check < 700 || device_is_present(device)) {
            if (now - device->last_check >= 700) device->last_check = now;
            InterlockedIncrement(&device->refs);
            LeaveCriticalSection(&g_device_lock);
            return device;
        }
        log_line("device %s is gone", device->udid);
        InterlockedExchange(&g_drop_cache, 1);
    }
    if (index >= 0) close_open_device(index);

    /* Which device to open: the one asked for, else the preferred one, else any. */
    copy_text(wanted, sizeof(wanted), udid);
    if (wanted[0] == 0) ini_get_text("PreferredDevice", wanted, sizeof(wanted));

    /* USB first. Wi-Fi only on request, because connecting can block for long. */
    error = idevice_new_with_options(&dev, wanted[0] ? wanted : NULL, IDEVICE_LOOKUP_USBMUX);
    if ((error != IDEVICE_E_SUCCESS || !dev) && ini_get_bool("AllowWiFi", FALSE)) {
        dev = NULL;
        network = TRUE;
        error = idevice_new_with_options(&dev, wanted[0] ? wanted : NULL, IDEVICE_LOOKUP_NETWORK);
    }
    if (error != IDEVICE_E_SUCCESS || !dev) {
        if (err) describe_missing_device(wanted, error, err, err_len);
        LeaveCriticalSection(&g_device_lock);
        return NULL;
    }

    device = (Device*)calloc(1, sizeof(Device));
    device->dev = dev;
    device->is_default = udid[0] == 0;
    device->network = network;
    device->has_afc2 = -1;
    device->last_check = GetTickCount();
    device->refs = 2;                       /* one for g_devices, one for the caller */
    if (idevice_get_udid(dev, &real_udid) == IDEVICE_E_SUCCESS && real_udid) {
        copy_text(device->udid, sizeof(device->udid), real_udid);
        free(real_udid);
    }
    if (g_devices_count == DEVICES_MAX) close_open_device(0);   /* full: close the oldest */
    g_devices[g_devices_count++] = device;
    InterlockedExchange(&g_device_seen, 1);
    log_line("device %s opened over %s", device->udid, network ? "Wi-Fi" : "USB");

    LeaveCriticalSection(&g_device_lock);
    return device;
}

/* Closes all devices and their connections (e.g. the settings changed). */
void device_forget(void)
{
    afc_cache_clear();
    EnterCriticalSection(&g_device_lock);
    while (g_devices_count > 0) close_open_device(g_devices_count - 1);
    LeaveCriticalSection(&g_device_lock);
}

/* TRUE when the device offers com.apple.afc2 (it is jailbroken). Asked once per device. */
BOOL device_has_afc2(const char* udid)
{
    Device* device = device_acquire(udid, FALSE, NULL, 0);
    BOOL result;

    if (!device) return FALSE;
    if (device->has_afc2 < 0) {
        lockdownd_service_descriptor_t service = NULL;
        BOOL ok = start_service(device, "com.apple.afc2", &service, NULL, 0);
        if (service) lockdownd_service_descriptor_free(service);
        device->has_afc2 = ok ? 1 : 0;
    }
    result = device->has_afc2 == 1;
    device_release(device);
    return result;
}

/* -------------------------------------------------------------- lockdown */

/* Text for a failed lockdown handshake; the pairing cases get plain advice. */
void lockdown_message(lockdownd_error_t e, WCHAR* dst, int dst_len)
{
    switch (e) {
    case LOCKDOWN_E_PASSWORD_PROTECTED:
        copy_wide(dst, dst_len, L"The device is locked with a passcode.\nUnlock it and try again.");
        break;
    case LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING:
        copy_wide(dst, dst_len, L"Tap \"Trust\" on the device, then try again.");
        break;
    case LOCKDOWN_E_USER_DENIED_PAIRING:
        copy_wide(dst, dst_len, L"Pairing was declined on the device (\"Don't Trust\").\n"
                                L"Reconnect the device and tap \"Trust\".");
        break;
    case LOCKDOWN_E_INVALID_HOST_ID:
        copy_wide(dst, dst_len, L"The device is not paired with this computer.\nReconnect it and tap \"Trust\".");
        break;
    default:
        format_wide(dst, dst_len, L"Could not connect to lockdownd: %hs (%d)", lockdownd_strerror(e), (int)e);
        break;
    }
}

/* Reads one text value from lockdown. 'dst' becomes empty when it is not available. */
static void lockdown_get_text(lockdownd_client_t lockdown, const char* key, char* dst, int dst_len)
{
    plist_t node = NULL;
    char* value = NULL;

    dst[0] = 0;
    if (lockdownd_get_value(lockdown, NULL, key, &node) != LOCKDOWN_E_SUCCESS || !node) return;
    plist_get_string_val(node, &value);
    if (value) {
        copy_text(dst, dst_len, value);
        plist_mem_free(value);
    }
    plist_free(node);
}

/* The user-visible name of a connected device ("Anna's iPhone"). Works without pairing. */
void device_read_name(const char* udid, WCHAR* name, int name_len)
{
    idevice_t dev = NULL;
    lockdownd_client_t lockdown = NULL;
    char value[256] = "";

    name[0] = 0;
    if (idevice_new_with_options(&dev, udid, IDEVICE_LOOKUP_USBMUX) != IDEVICE_E_SUCCESS || !dev) {
        dev = NULL;
        if (!ini_get_bool("AllowWiFi", FALSE) ||
            idevice_new_with_options(&dev, udid, IDEVICE_LOOKUP_NETWORK) != IDEVICE_E_SUCCESS || !dev) return;
    }
    if (lockdownd_client_new(dev, &lockdown, PLUGIN_LABEL) == LOCKDOWN_E_SUCCESS && lockdown) {
        lockdown_get_text(lockdown, "DeviceName", value, sizeof(value));
        lockdownd_client_free(lockdown);
    }
    idevice_free(dev);
    utf8_to_wide(value, name, name_len);
}

/* First session with a device: remember the iOS version and note the device in the log. */
static void read_device_facts(Device* device, lockdownd_client_t lockdown)
{
    static const char* keys[] = { "DeviceName", "DeviceClass", "ProductType", "ProductVersion", "BuildVersion" };
    char value[256];
    int i;

    for (i = 0; i < 5; i++) {
        lockdown_get_text(lockdown, keys[i], value, sizeof(value));
        log_line("%s: %s", keys[i], value);
        if (strcmp(keys[i], "ProductVersion") == 0) device->ios_major = atoi(value);
    }
    if (device->ios_major == 0) device->ios_major = 1;      /* unknown, but do not ask again */
}

/*
 * Opens a lockdown session and starts one service. On success the caller
 * connects to *service and frees it with lockdownd_service_descriptor_free().
 */
BOOL start_service(Device* device, const char* name,
                   lockdownd_service_descriptor_t* service, WCHAR* err, int err_len)
{
    lockdownd_client_t lockdown = NULL;
    lockdownd_error_t e;

    *service = NULL;
    e = lockdownd_client_new_with_handshake(device->dev, &lockdown, PLUGIN_LABEL);
    if (e != LOCKDOWN_E_SUCCESS || !lockdown) {
        log_line("lockdown handshake failed: %d (%s)", (int)e, lockdownd_strerror(e));
        if (err) lockdown_message(e, err, err_len);
        return FALSE;
    }
    if (device->ios_major == 0) read_device_facts(device, lockdown);

    e = lockdownd_start_service(lockdown, name, service);
    lockdownd_client_free(lockdown);
    log_line("start %s: %d (%s), port %d, ssl %d", name, (int)e, lockdownd_strerror(e),
             *service ? (int)(*service)->port : 0, *service ? (int)(*service)->ssl_enabled : -1);

    if (e != LOCKDOWN_E_SUCCESS || !*service || (*service)->port == 0) {
        if (*service) { lockdownd_service_descriptor_free(*service); *service = NULL; }
        if (err) format_wide(err, err_len, L"Could not start service %hs: %hs", name, lockdownd_strerror(e));
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------- AFC connections */

void conn_close(AfcConn* conn)
{
    if (!conn) return;
    if (conn->afc) afc_client_free(conn->afc);
    if (conn->house_arrest) house_arrest_client_free(conn->house_arrest);
    device_release(conn->device);
    free(conn);
}

/* Connects to the house_arrest service; the result is ready for one command. */
static BOOL house_arrest_connect(AfcConn* conn, WCHAR* err, int err_len)
{
    lockdownd_service_descriptor_t service = NULL;
    house_arrest_error_t e;

    if (conn->house_arrest) { house_arrest_client_free(conn->house_arrest); conn->house_arrest = NULL; }
    if (!start_service(conn->device, HOUSE_ARREST_SERVICE_NAME, &service, err, err_len)) return FALSE;
    e = house_arrest_client_new(conn->device->dev, service, &conn->house_arrest);
    lockdownd_service_descriptor_free(service);
    if (e != HOUSE_ARREST_E_SUCCESS || !conn->house_arrest) {
        conn->house_arrest = NULL;
        if (err) copy_wide(err, err_len, L"Could not connect to house_arrest.");
        return FALSE;
    }
    return TRUE;
}

/*
 * Opens the files of one application.
 *   VendDocuments  the app's Documents folder (apps with file sharing)
 *   VendContainer  the whole container (before iOS 8, or developer-signed apps)
 * A failed command leaves the session unusable, so each try reconnects.
 */
static BOOL open_application(AfcConn* conn, const char* bundle_id, WCHAR* err, int err_len)
{
    const char* commands[2];
    int command_count = 0;
    char last_error[128] = "unknown error";
    int i;

    if (conn->device->ios_major == 0) {         /* need the iOS version: any session reads it */
        lockdownd_service_descriptor_t service = NULL;
        if (start_service(conn->device, HOUSE_ARREST_SERVICE_NAME, &service, NULL, 0) && service)
            lockdownd_service_descriptor_free(service);
    }
    if (conn->device->ios_major < 8) {
        commands[command_count++] = "VendContainer";
    } else {
        if (ini_get_bool("ShowAllApps", FALSE)) commands[command_count++] = "VendContainer";
        commands[command_count++] = "VendDocuments";
    }

    for (i = 0; i < command_count; i++) {
        plist_t result = NULL;
        plist_t error_node;
        BOOL accepted;

        if (!house_arrest_connect(conn, err, err_len)) return FALSE;
        if (house_arrest_send_command(conn->house_arrest, commands[i], bundle_id) != HOUSE_ARREST_E_SUCCESS) continue;
        if (house_arrest_get_result(conn->house_arrest, &result) != HOUSE_ARREST_E_SUCCESS || !result) continue;

        error_node = plist_dict_get_item(result, "Error");
        accepted = error_node == NULL;
        if (error_node) {
            char* text = NULL;
            plist_get_string_val(error_node, &text);
            if (text) { copy_text(last_error, sizeof(last_error), text); plist_mem_free(text); }
        }
        plist_free(result);
        if (!accepted) continue;

        if (afc_client_new_from_house_arrest_client(conn->house_arrest, &conn->afc) == AFC_E_SUCCESS && conn->afc)
            return TRUE;
        conn->afc = NULL;
    }

    log_line("house_arrest %s: %s", bundle_id, last_error);
    if (err) {
        WCHAR wide_id[APP_ID_MAX];
        utf8_to_wide(bundle_id, wide_id, APP_ID_MAX);
        format_wide(err, err_len, L"The application %s does not share its files (%hs).", wide_id, last_error);
    }
    return FALSE;
}

/* Opens the connection for 'rp' on one device. */
static AfcConn* conn_open_on(Device* device, const RemotePath* rp, WCHAR* err, int err_len)
{
    AfcConn* conn = (AfcConn*)calloc(1, sizeof(AfcConn));
    BOOL ok;

    conn->device = device;
    InterlockedIncrement(&device->refs);
    connection_key(rp, conn->key, sizeof(conn->key));

    if (rp->mode == M_APP) {
        ok = open_application(conn, rp->app, err, err_len);
    } else {
        const char* name = rp->mode == M_ROOT ? "com.apple.afc2" : AFC_SERVICE_NAME;
        lockdownd_service_descriptor_t service = NULL;
        ok = start_service(device, name, &service, err, err_len);
        if (ok) {
            ok = afc_client_new(device->dev, service, &conn->afc) == AFC_E_SUCCESS && conn->afc != NULL;
            lockdownd_service_descriptor_free(service);
            if (!ok && err) copy_wide(err, err_len, L"Could not connect to the AFC service.");
        }
    }
    if (!ok) { conn->afc = NULL; conn_close(conn); return NULL; }
    return conn;
}

/*
 * Opens a new connection for 'rp'. Tries twice: the second time the device is
 * looked up again, which covers "unplugged and plugged back in".
 */
AfcConn* conn_open(const RemotePath* rp, WCHAR* err, int err_len)
{
    int attempt;

    for (attempt = 0; attempt < 2; attempt++) {
        Device* device = device_acquire(rp->udid, attempt == 1, err, err_len);
        AfcConn* conn;
        if (!device) continue;
        conn = conn_open_on(device, rp, err, err_len);
        device_release(device);
        if (conn) return conn;
        if (attempt == 0) afc_cache_clear();    /* the old connections are suspect too */
    }
    return NULL;
}

/* ------------------------------------------------------ connection cache */

void afc_cache_clear(void)
{
    int i;
    EnterCriticalSection(&g_cache_lock);
    for (i = 0; i < g_cache_count; i++) conn_close(g_cache[i]);
    g_cache_count = 0;
    LeaveCriticalSection(&g_cache_lock);
}

/* Index of the cached connection with this key, or -1. Caller holds g_cache_lock. */
static int cache_find(const char* key)
{
    int i;
    for (i = 0; i < g_cache_count; i++)
        if (strcmp(g_cache[i]->key, key) == 0) return i;
    return -1;
}

/* Closes and removes one cached connection. Caller holds g_cache_lock. */
static void cache_remove(int index)
{
    conn_close(g_cache[index]);
    memmove(&g_cache[index], &g_cache[index + 1], (g_cache_count - index - 1) * sizeof(AfcConn*));
    g_cache_count--;
}

/* Errors that mean the connection itself is broken (as opposed to "file not found"). */
BOOL afc_is_connection_error(afc_error_t e)
{
    return e == AFC_E_MUX_ERROR || e == AFC_E_NOT_ENOUGH_DATA || e == AFC_E_INTERNAL_ERROR ||
           e == AFC_E_OP_TIMEOUT || e == AFC_E_UNKNOWN_ERROR || e == AFC_E_OP_HEADER_INVALID;
}

/* See the declaration in wfx_ios.h. Returns AFC_E_MUX_ERROR when no connection can be made. */
afc_error_t afc_run(const RemotePath* rp, AfcOp op, void* arg, WCHAR* err, int err_len)
{
    afc_error_t result = AFC_E_MUX_ERROR;
    char key[KEY_MAX];
    int attempt;

    connection_key(rp, key, sizeof(key));
    EnterCriticalSection(&g_cache_lock);

    for (attempt = 0; attempt < 2; attempt++) {
        Device* device;
        int index;

        /* No device: nothing cached can work, and there is no point in trying. */
        device = device_acquire(rp->udid, FALSE, err, err_len);
        if (!device) {
            afc_cache_clear();
            InterlockedExchange(&g_drop_cache, 0);
            break;
        }
        device_release(device);
        if (InterlockedExchange(&g_drop_cache, 0)) afc_cache_clear();

        index = cache_find(key);
        if (index < 0) {
            AfcConn* conn = conn_open(rp, err, err_len);
            if (!conn) break;
            if (g_cache_count == CACHE_MAX) cache_remove(0);    /* full: drop the oldest */
            index = g_cache_count++;
            g_cache[index] = conn;
        }

        result = op(g_cache[index]->afc, arg);
        if (!afc_is_connection_error(result)) break;

        index = cache_find(key);                /* dead link: forget it and try once more */
        if (index >= 0) cache_remove(index);
    }

    LeaveCriticalSection(&g_cache_lock);
    return result;
}

/* ------------------------------------------------------ file information */

/* Reads the attributes of one entry. A symbolic link is reported as a link. */
afc_error_t afc_stat(afc_client_t afc, const char* path, FileStat* st)
{
    char** info = NULL;
    afc_error_t e;
    int i;

    memset(st, 0, sizeof(*st));
    e = afc_get_file_info(afc, path, &info);
    if (e != AFC_E_SUCCESS) return e;
    if (!info) return AFC_E_UNKNOWN_ERROR;

    /* The device answers with a list of key, value, key, value, ... */
    for (i = 0; info[i] && info[i + 1]; i += 2) {
        const char* key = info[i];
        const char* value = info[i + 1];
        if      (strcmp(key, "st_size") == 0)      st->size   = _strtoui64(value, NULL, 10);
        else if (strcmp(key, "st_mtime") == 0)     st->mtime  = _strtoui64(value, NULL, 10);
        else if (strcmp(key, "st_birthtime") == 0) st->birth  = _strtoui64(value, NULL, 10);
        else if (strcmp(key, "st_nlink") == 0)     st->nlink  = _strtoui64(value, NULL, 10);
        else if (strcmp(key, "st_blocks") == 0)    st->blocks = _strtoui64(value, NULL, 10);
        else if (strcmp(key, "st_ifmt") == 0)      copy_text(st->ifmt, sizeof(st->ifmt), value);
        else if (strcmp(key, "LinkTarget") == 0)   copy_text(st->target, sizeof(st->target), value);
    }
    afc_dictionary_free(info);

    st->is_dir = strcmp(st->ifmt, "S_IFDIR") == 0;
    st->is_link = strcmp(st->ifmt, "S_IFLNK") == 0;
    return AFC_E_SUCCESS;
}

/* Like afc_stat(), but a link that points to a folder is reported as a folder. */
afc_error_t afc_stat_follow(afc_client_t afc, const char* path, FileStat* st)
{
    afc_error_t e = afc_stat(afc, path, st);

    if (e == AFC_E_SUCCESS && st->is_link && st->target[0]) {
        char parent[AFC_PATH_MAX], target[AFC_PATH_MAX];
        FileStat* target_stat = (FileStat*)malloc(sizeof(FileStat));
        path_parent(path, parent, sizeof(parent));
        path_join(parent, st->target, target, sizeof(target));
        if (target_stat && afc_stat(afc, target, target_stat) == AFC_E_SUCCESS && target_stat->is_dir)
            st->is_dir = TRUE;
        free(target_stat);
    }
    return e;
}
