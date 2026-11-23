/*
 * wfx_iOS - Total Commander file-system plugin for iOS devices.
 *
 * An update of the wfx_iOS 1.3 plugin (2017) by Domo, the original author.
 * The folder layout, feature set and behaviour are his design; this version
 * re-implements them on current libimobiledevice / OpenSSL 3.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Shared declarations. The plugin is plain C, split into these modules:
 *
 *   util.c      strings, time conversion, ini file, log file, error box
 *   lang.c      all user-visible texts (English, Ukrainian, Russian, Polish)
 *   device.c    device handle, lockdown, service start, AFC connections
 *   paths.c     Total Commander path -> device path, application names
 *   listing.c   directory listings (FsFindFirst / FsFindNext)
 *   transfer.c  download, upload, copy on the device
 *   apps.c      installing and removing applications
 *   photos.c    "Photos by month" (reads the device's photo database)
 *   icons.c     folder and application icons
 *   columns.c   extra file details for custom columns
 *   dialogs.c   settings dialog, file properties
 *   plugin.c    the functions Total Commander calls (Fs...)
 *
 * Virtual folder layout shown to Total Commander:
 *
 *   \Root                  com.apple.afc2 (jailbroken devices only)
 *   \User                  com.apple.afc, the media partition
 *   \Photos                com.apple.afc, rooted at /DCIM
 *   \Processed photos      like Photos, downloads prefer the edited picture
 *   \iCloud photos         com.apple.afc, rooted at /PhotoData/CPLAssets
 *   \Photos by month       the whole library by month, see photos.c
 *   \Applications\<app>    com.apple.mobile.house_arrest (app documents)
 *   \Settings              pseudo file, opens the settings dialog
 *
 * With several devices connected (and no preferred device selected) the root
 * shows one folder per device instead, and each contains the layout above:
 *
 *   \<device name>\User\...
 */
#ifndef WFX_IOS_H
#define WFX_IOS_H

#include <winsock2.h>
#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/afc.h>
#include <libimobiledevice/house_arrest.h>
#include <libimobiledevice/installation_proxy.h>
#include <libimobiledevice/sbservices.h>
#include <plist/plist.h>
#include <usbmuxd.h>

#include "fsplugin.h"
#include "resource.h"

/* The version comes from version.txt through CMake (see CMakeLists.txt). */
#ifndef WFX_VERSION_STR
#define WFX_VERSION_STR "0.0"
#endif

#define PLUGIN_LABEL    "wfx_iOS"          /* name we give to the device services */
#define PLUGIN_TITLE    L"iOS"             /* message box caption */
#define INI_SECTION     "wfx_iOS"
#define SETTINGS_ITEM   L"Settings"        /* pseudo file in the plugin root */
#define INFO_ITEM       L"Device information"  /* pseudo file in a device's root */
#define ICLOUD_DIR      "/PhotoData/CPLAssets"
#define MONTHS_FOLDER   L"Photos by month"
#define HOME_FOLDER     L"Home screen"     /* the icons of the device as folders */

#define AFC_PATH_MAX    2048               /* device path, UTF-8 */
#define TC_PATH_MAX     1024               /* Total Commander path, UTF-16 */
#define APP_ID_MAX      200                /* bundle identifier */
#define UDID_MAX        64                 /* device identifier */
#define KEY_MAX         (UDID_MAX + APP_ID_MAX + 8)   /* connection key, see connection_key() */
#define MSG_MAX         700                /* error message text */
#define IO_CHUNK        (1024 * 1024)      /* file transfer block size */

/* ------------------------------------------------------------------ util.c */

extern HINSTANCE      g_inst;              /* this DLL */
extern int            g_plugin_nr;         /* our number, given by FsInit */
extern tProgressProcW g_progress_proc;
extern tLogProcW      g_log_proc;
extern char           g_ini[MAX_PATH];     /* ini file with our settings */
extern WCHAR          g_log_path[MAX_PATH];/* empty while the log is closed */

void     util_init(void);                                   /* called once from DllMain */

void     utf8_to_wide(const char* src, WCHAR* dst, int dst_len);
void     wide_to_utf8(const WCHAR* src, char* dst, int dst_len);
void     copy_text(char* dst, int dst_len, const char* src);
void     copy_wide(WCHAR* dst, int dst_len, const WCHAR* src);
void     format_wide(WCHAR* dst, int dst_len, const WCHAR* fmt, ...);
void     append_wide(WCHAR* dst, int dst_len, const WCHAR* fmt, ...);

FILETIME unix_ns_to_filetime(uint64_t ns);
uint64_t filetime_to_unix_ns(const FILETIME* ft);
void     format_time(uint64_t ns, WCHAR* dst, int dst_len);

BOOL     ini_get_bool(const char* key, BOOL default_value);
void     ini_get_text(const char* key, char* dst, int dst_len);
void     ini_set_text(const char* key, const char* value);  /* NULL removes the key */

void     log_open(void);                                    /* honours Log / Debug / LogFile */
void     log_line(const char* fmt, ...);
void     show_error(const WCHAR* message);                  /* one box per problem, not per retry */

void     path_parent(const char* path, char* dst, int dst_len);
void     path_join(const char* dir, const char* name, char* dst, int dst_len);

/* ----------------------------------------------------------------- lang.c */

/* Identifiers of the texts shown to the user; the texts themselves are in lang.c. */
typedef enum {
    S_ERR_NO_SERVICE,
    S_ERR_NO_DEVICE,
    S_ERR_DEVICE_NOT_CONNECTED,
    S_ERR_OPEN_DEVICE,
    S_ERR_DEVICES_REPORTED,
    S_ERR_PASSCODE,
    S_ERR_TRUST_PENDING,
    S_ERR_TRUST_DENIED,
    S_ERR_NOT_PAIRED,
    S_ERR_LOCKDOWN,
    S_ERR_START_SERVICE,
    S_ERR_HOUSE_ARREST,
    S_ERR_APP_NOT_SHARED,
    S_ERR_AFC,
    S_ERR_INSTPROXY,
    S_ERR_APP_LIST,
    S_ERR_READ_DIR,
    S_ERR_FILE_INFO,
    S_UNINSTALL_QUESTION,
    S_UNINSTALL_TITLE,
    S_ERR_UNINSTALL,
    S_ERR_INSTALL,
    S_ERR_PHOTO_DB_MISSING,
    S_ERR_PHOTO_DB_READ,
    S_ERR_PHOTO_DB_LAYOUT,
    S_DLG_SETTINGS,
    S_GRP_DEVICE,
    S_LBL_USE,
    S_BTN_REFRESH,
    S_GRP_OPTIONS,
    S_CHK_APPNAMES,
    S_CHK_ALLAPPS,
    S_CHK_WIFI,
    S_CHK_LOG,
    S_CHK_DEBUG,
    S_LBL_LANGUAGE,
    S_LANG_AUTO,
    S_BTN_OPENLOG,
    S_BTN_OK,
    S_BTN_CANCEL,
    S_ALL_DEVICES,
    S_NOT_CONNECTED,
    S_NOT_CONNECTED_SUFFIX,
    S_INFO_FORMAT,
    S_FREE_FORMAT,
    S_ABOUT,
    S_RESTART_NEEDED,
    S_PROP_TITLE,
    S_PROP_NAME,
    S_PROP_PATH,
    S_PROP_TYPE,
    S_PROP_SIZE,
    S_PROP_CREATED,
    S_PROP_MODIFIED,
    S_PROP_LINKS,
    S_PROP_BLOCKS,
    S_PROP_TARGET,
    S_ENTRIES,
    S_SIZE_FORMAT,
    S_TYPE_FILE,
    S_TYPE_DIR,
    S_TYPE_LINK,
    S_TYPE_BLOCK,
    S_TYPE_DEVICE,
    S_BTN_DEVINFO,
    S_DLG_INFO,
    S_COL_PROPERTY,
    S_COL_VALUE,
    S_BTN_COPY_VALUE,
    S_BTN_COPY_ALL,
    S_BTN_SAVE,
    S_BTN_CLOSE,
    S_SEC_MAIN,
    S_SEC_IDS,
    S_SEC_STORAGE,
    S_SEC_BATTERY,
    S_SEC_REGION,
    S_SEC_OTHER,
    S_INFO_LIMITED,
    S_YES,
    S_NO,
    S_ERR_SAVE,
    S_ERR_HOME_READ,
    S_ERR_HOME_WRITE,
    S_ERR_HOME_FILE,
    S_COUNT
} StringId;

void         lang_load(void);                               /* picks the language (setting or Windows) */
const WCHAR* tr(StringId id);                               /* the text in the current language */

/* ---------------------------------------------------------------- paths.c */

typedef enum {
    M_INVALID,      /* not one of our folders */
    M_TOP,          /* the plugin root */
    M_ROOT,         /* \Root */
    M_USER,         /* \User */
    M_PHOTOS,       /* \Photos */
    M_PROCESSED,    /* \Processed photos */
    M_ICLOUD,       /* \iCloud photos */
    M_MONTHS,       /* \Photos by month and its month folders (files inside become M_USER) */
    M_APPLIST,      /* \Applications */
    M_APP,          /* inside one application */
    M_SETTINGS,     /* \Settings */
    M_INFO,         /* \Device information */
    M_HOME          /* \Home screen\...: path is "/Page 01/Folder/Item" */
} PathMode;

typedef struct {
    PathMode mode;
    char     udid[UDID_MAX];        /* device; empty = the default device */
    char     app[APP_ID_MAX];       /* bundle id, only for M_APP */
    char     path[AFC_PATH_MAX];    /* path on the device, always starts with '/' */
} RemotePath;

void paths_init(void);                                      /* called once from DllMain */
void parse_path(const WCHAR* tc_path, RemotePath* out);
BOOL is_afc_mode(PathMode mode);                            /* TRUE when files can be read there */
void connection_key(const RemotePath* rp, char* key, int key_len);

/* Table "name shown in \Applications" -> bundle id, kept per device. */
void apps_table_clear(const char* udid);                    /* NULL clears all devices */
void apps_table_add(const char* udid, const WCHAR* shown_name, const char* bundle_id);
void apps_table_done(const char* udid);                     /* marks the device's table as loaded */
void apps_resolve(const char* udid, const WCHAR* shown_name, char* bundle_id, int bundle_len);

/* Table "device folder shown in the plugin root" -> UDID (several devices connected). */
void devices_table_clear(void);
void devices_table_add(const WCHAR* shown_name, const char* udid);
BOOL devices_resolve(const WCHAR* shown_name, char* udid, int udid_len);
BOOL is_reserved_name(const WCHAR* name);                   /* TRUE for "User", "Photos", ... */

/* --------------------------------------------------------------- device.c */

typedef struct {
    idevice_t dev;
    char      udid[UDID_MAX];
    BOOL      is_default;           /* opened as "the default device" (preferred or first found) */
    DWORD     last_check;           /* tick of the last "still connected?" check */
    int       ios_major;            /* 0 until the first lockdown session */
    int       has_afc2;             /* -1 unknown, 0 no, 1 yes */
    BOOL      network;              /* reached over Wi-Fi instead of USB */
    LONG      refs;                 /* reference count, see device_release() */
} Device;

/* One open AFC connection (with its house_arrest session for applications). */
typedef struct {
    Device*               device;
    afc_client_t          afc;
    house_arrest_client_t house_arrest;
    char                  key[KEY_MAX];
} AfcConn;

/* What we read from the device about one file. Times are ns since 1970. */
typedef struct {
    BOOL     is_dir;
    BOOL     is_link;
    uint64_t size, mtime, birth, nlink, blocks;
    char     ifmt[16];              /* "S_IFREG", "S_IFDIR", ... */
    char     target[AFC_PATH_MAX];  /* link target */
} FileStat;

void    device_init(void);                                  /* called once from DllMain */
/* 'udid' selects the device; an empty string means the default device. */
Device* device_acquire(const char* udid, BOOL fresh, WCHAR* err, int err_len);  /* NULL: none; else release it */
void    device_release(Device* device);
void    device_forget(void);                                /* e.g. another device was selected */
BOOL    device_has_afc2(const char* udid);
void    device_read_name(const char* udid, WCHAR* name, int name_len);          /* empty when unknown */

void    lockdown_message(lockdownd_error_t e, WCHAR* dst, int dst_len);
BOOL    start_service(Device* device, const char* name,
                      lockdownd_service_descriptor_t* service, WCHAR* err, int err_len);

/* A private connection, used for one file transfer. */
AfcConn* conn_open(const RemotePath* rp, WCHAR* err, int err_len);
void     conn_close(AfcConn* conn);

/*
 * Short operations share cached connections. afc_run() calls 'op' with the
 * cached connection for 'rp' and repeats it once on a fresh connection when
 * the link turned out to be dead.
 */
typedef afc_error_t (*AfcOp)(afc_client_t afc, void* arg);
afc_error_t afc_run(const RemotePath* rp, AfcOp op, void* arg, WCHAR* err, int err_len);
void        afc_cache_clear(void);
BOOL        afc_is_connection_error(afc_error_t e);

afc_error_t afc_stat(afc_client_t afc, const char* path, FileStat* st);          /* the entry itself */
afc_error_t afc_stat_follow(afc_client_t afc, const char* path, FileStat* st);   /* link to dir counts as dir */

/* -------------------------------------------------------------- listing.c */

/* Result of one directory listing; Total Commander reads it entry by entry. */
typedef struct {
    WIN32_FIND_DATAW* items;
    int               count;
    int               capacity;
    int               next;         /* next entry FsFindNext will return */
} FindList;

FindList* find_list_create(const WCHAR* tc_path, WCHAR* err, int err_len);  /* NULL on failure */
void      find_list_add_entry(FindList* list, const WIN32_FIND_DATAW* entry);
void      find_list_add_folder(FindList* list, const WCHAR* name);   /* a folder without date */
void      find_list_free(FindList* list);
BOOL      apps_load(const char* udid, WCHAR* err, int err_len);   /* fills the application table */
void      devices_load(void);                               /* fills the device folder table */

/* ------------------------------------------------------------- transfer.c */

int download_file(const WCHAR* remote_name, const WCHAR* local_name, int copy_flags, const RemotePath* rp);
int upload_file(const WCHAR* local_name, const WCHAR* remote_name, int copy_flags, const RemotePath* rp);
int copy_on_device(const WCHAR* old_name, const WCHAR* new_name, const RemotePath* src, const RemotePath* dst);

/* ---------------------------------------------------------------- icons.c */

void icons_init(void);                                      /* called once from DllMain */
int  extract_icon(WCHAR* remote_name, int extract_flags, HICON* icon);

/* --------------------------------------------------------------- photos.c */

void photos_init(void);                                     /* called once from DllMain */
BOOL photos_available(const char* udid);                    /* the device has a photo database */
BOOL photos_list_months(const char* udid, FindList* list, WCHAR* err, int err_len);
BOOL photos_list_month(const char* udid, const char* month_text, FindList* list, WCHAR* err, int err_len);
BOOL photos_resolve(const char* udid, const char* month_text, const char* name, char* real, int real_len);

/* ----------------------------------------------------------------- apps.c */

typedef enum { GATE_PROCEED, GATE_REMOVED, GATE_REFUSED } AppGate;

void    apps_delete_operation(const WCHAR* remote_dir, BOOL starting);
AppGate apps_delete_gate(const WCHAR* tc_path, const RemotePath* rp);
BOOL    apps_is_install_target(const WCHAR* tc_path, const RemotePath* rp);
int     apps_install(const WCHAR* local_name, const WCHAR* remote_name, const RemotePath* target);

/* -------------------------------------------------------------- columns.c */

void columns_init(void);                                    /* called once from DllMain */
int  columns_get_field(int field_index, char* field_name, char* units, int max_len);
int  columns_get_value(const WCHAR* file_name, int field_index, void* value, int max_len, int flags);

/* -------------------------------------------------------------- dialogs.c */

void show_settings(HWND parent);
void show_device_info(HWND parent, const char* udid);     /* info.c */

/* home.c - the home screen layout as folders */
void home_init(void);                                       /* called once from DllMain */
BOOL home_list(const RemotePath* rp, FindList* list, WCHAR* err, int err_len);
int  home_move(const RemotePath* src, const RemotePath* dst);                   /* FS_FILE_* */
BOOL home_make_folder(const RemotePath* rp);
BOOL home_remove_folder(const RemotePath* rp);
BOOL home_is_layout_file(const RemotePath* rp);             /* "\Home screen\<name>.plist" */
int  home_download_layout(const RemotePath* rp, const WCHAR* local_name, int copy_flags);
int  home_upload_layout(const WCHAR* local_name, const RemotePath* rp);
BOOL home_item_bundle(const RemotePath* rp, char* bundle, int bundle_len);
void show_file_properties(HWND parent, const WCHAR* remote_name, const RemotePath* rp);

#endif /* WFX_IOS_H */
