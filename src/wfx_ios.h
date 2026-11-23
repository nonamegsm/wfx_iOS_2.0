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
 *   device.c    device handle, lockdown, service start, AFC connections
 *   paths.c     Total Commander path -> device path, application names
 *   listing.c   directory listings (FsFindFirst / FsFindNext)
 *   transfer.c  download, upload, copy on the device
 *   icons.c     folder and application icons
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
 *   \Applications\<app>    com.apple.mobile.house_arrest (app documents)
 *   \Settings              pseudo file, opens the settings dialog
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
#define ICLOUD_DIR      "/PhotoData/CPLAssets"

#define AFC_PATH_MAX    2048               /* device path, UTF-8 */
#define TC_PATH_MAX     1024               /* Total Commander path, UTF-16 */
#define APP_ID_MAX      200                /* bundle identifier */
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

/* ---------------------------------------------------------------- paths.c */

typedef enum {
    M_INVALID,      /* not one of our folders */
    M_TOP,          /* the plugin root */
    M_ROOT,         /* \Root */
    M_USER,         /* \User */
    M_PHOTOS,       /* \Photos */
    M_PROCESSED,    /* \Processed photos */
    M_ICLOUD,       /* \iCloud photos */
    M_APPLIST,      /* \Applications */
    M_APP,          /* inside one application */
    M_SETTINGS      /* \Settings */
} PathMode;

typedef struct {
    PathMode mode;
    char     app[APP_ID_MAX];       /* bundle id, only for M_APP */
    char     path[AFC_PATH_MAX];    /* path on the device, always starts with '/' */
} RemotePath;

void paths_init(void);                                      /* called once from DllMain */
void parse_path(const WCHAR* tc_path, RemotePath* out);
BOOL is_afc_mode(PathMode mode);                            /* TRUE when files can be read there */
void connection_key(const RemotePath* rp, char* key, int key_len);

/* Table "name shown in \Applications" -> bundle id. */
void apps_table_clear(void);
void apps_table_add(const WCHAR* shown_name, const char* bundle_id);
void apps_table_done(void);                                 /* marks the table as loaded */
void apps_resolve(const WCHAR* shown_name, char* bundle_id, int bundle_len);

/* --------------------------------------------------------------- device.c */

typedef struct {
    idevice_t dev;
    char      udid[64];
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
    char                  key[APP_ID_MAX + 8];
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
Device* device_acquire(BOOL fresh, WCHAR* err, int err_len);/* NULL when no device; else release it */
void    device_release(Device* device);
void    device_forget(void);                                /* e.g. another device was selected */
BOOL    device_has_afc2(void);

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
void      find_list_free(FindList* list);
BOOL      apps_load(WCHAR* err, int err_len);               /* fills the application table */

/* ------------------------------------------------------------- transfer.c */

int download_file(const WCHAR* remote_name, const WCHAR* local_name, int copy_flags, const RemotePath* rp);
int upload_file(const WCHAR* local_name, const WCHAR* remote_name, int copy_flags, const RemotePath* rp);
int copy_on_device(const WCHAR* old_name, const WCHAR* new_name, const RemotePath* src, const RemotePath* dst);

/* ---------------------------------------------------------------- icons.c */

void icons_init(void);                                      /* called once from DllMain */
int  extract_icon(WCHAR* remote_name, int extract_flags, HICON* icon);

/* -------------------------------------------------------------- dialogs.c */

void show_settings(HWND parent);
void show_file_properties(HWND parent, const WCHAR* remote_name, const RemotePath* rp);

#endif /* WFX_IOS_H */
