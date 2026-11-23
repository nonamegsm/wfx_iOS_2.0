/*
 * apps.c - installing and removing applications from \Applications.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *   Install    copy an .ipa file into \Applications
 *   Uninstall  delete an application folder in \Applications
 *
 * Uninstalling deletes the application and all its data, so it is always
 * confirmed. Total Commander deletes a folder by first deleting everything
 * inside it; without care it would wipe an application's documents before we
 * get to ask. Therefore the plugin watches for "a delete operation started in
 * \Applications" and asks at the first request that touches an application:
 *
 *   Yes  the application is removed at once; the rest of the delete operation
 *        finds nothing left to do
 *   No   every delete request for that application is refused
 */
#include "wfx_ios.h"

#define STAGING_DIR   "/PublicStaging"      /* where the device expects packages to install */
#define DECISIONS_MAX 64

typedef enum { ANSWER_YES, ANSWER_NO } Answer;

/* What the user answered for one application during the current delete operation. */
typedef struct {
    char   udid[UDID_MAX];
    char   bundle[APP_ID_MAX];
    Answer answer;
} Decision;

static BOOL     g_deleting_apps = FALSE;    /* a delete operation started in \Applications */
static DWORD    g_delete_started = 0;       /* tick when it started */

/* Should Total Commander never report the end of the operation, stop asking after this time. */
#define DELETE_OPERATION_TIMEOUT_MS (10 * 60 * 1000)
static Decision g_decisions[DECISIONS_MAX];
static int      g_decisions_count = 0;

/* ------------------------------------------------- installation_proxy calls */

/* Connects to installation_proxy on the device of 'udid'. */
static instproxy_client_t proxy_connect(const char* udid, WCHAR* err, int err_len)
{
    Device* device = device_acquire(udid, FALSE, err, err_len);
    lockdownd_service_descriptor_t service = NULL;
    instproxy_client_t proxy = NULL;

    if (!device) return NULL;
    if (start_service(device, INSTPROXY_SERVICE_NAME, &service, err, err_len)) {
        if (instproxy_client_new(device->dev, service, &proxy) != INSTPROXY_E_SUCCESS) {
            proxy = NULL;
            if (err) copy_wide(err, err_len, L"Could not connect to installation_proxy.");
        }
        lockdownd_service_descriptor_free(service);
    }
    device_release(device);
    return proxy;
}

/* Removes an application with all its data. Waits until the device is done. */
static BOOL uninstall_application(const char* udid, const char* bundle, WCHAR* err, int err_len)
{
    instproxy_client_t proxy = proxy_connect(udid, err, err_len);
    instproxy_error_t e;

    if (!proxy) return FALSE;
    e = instproxy_uninstall(proxy, bundle, NULL, NULL, NULL);       /* no callback: runs to the end */
    instproxy_client_free(proxy);
    log_line("uninstall %s: %d", bundle, (int)e);

    if (e != INSTPROXY_E_SUCCESS) {
        if (err) format_wide(err, err_len, L"The device refused to remove the application (error %d).", (int)e);
        return FALSE;
    }
    afc_cache_clear();              /* a connection into the removed application is useless now */
    apps_table_clear(udid);
    return TRUE;
}

/* ------------------------------------------------------------- uninstall */

/* Called by FsStatusInfoW when a delete operation starts or ends. */
void apps_delete_operation(const WCHAR* remote_dir, BOOL starting)
{
    RemotePath rp;

    g_decisions_count = 0;
    g_deleting_apps = FALSE;
    if (!starting) return;
    parse_path(remote_dir, &rp);
    g_deleting_apps = rp.mode == M_APPLIST;
    g_delete_started = GetTickCount();
}

/* The application's name as the user sees it: the path component after "Applications". */
static void shown_app_name(const WCHAR* tc_path, WCHAR* name, int name_len)
{
    const WCHAR* p = wcsstr(tc_path, L"\\Applications\\");
    int len = 0;

    name[0] = 0;
    if (!p) return;
    p += 14;                        /* length of "\Applications\" */
    while (*p && *p != L'\\' && len < name_len - 1) name[len++] = *p++;
    name[len] = 0;
}

/*
 * To be called for every request that would delete something of an
 * application. Tells the caller what to do with it:
 *
 *   GATE_PROCEED    not our business: carry on as usual
 *   GATE_REMOVED    the application was uninstalled: report success, there is nothing left
 *   GATE_REFUSED    the user said no (or uninstalling failed): refuse the request
 */
AppGate apps_delete_gate(const WCHAR* tc_path, const RemotePath* rp)
{
    WCHAR name[APP_ID_MAX], id[APP_ID_MAX], question[MSG_MAX], err[MSG_MAX] = L"";
    Decision* decision;
    int i;

    if (!g_deleting_apps || rp->mode != M_APP) return GATE_PROCEED;
    if (GetTickCount() - g_delete_started > DELETE_OPERATION_TIMEOUT_MS) {
        g_deleting_apps = FALSE;
        return GATE_PROCEED;
    }

    /* Already asked about this application in this operation? */
    for (i = 0; i < g_decisions_count; i++) {
        if (strcmp(g_decisions[i].udid, rp->udid) == 0 && strcmp(g_decisions[i].bundle, rp->app) == 0)
            return g_decisions[i].answer == ANSWER_YES ? GATE_REMOVED : GATE_REFUSED;
    }
    if (g_decisions_count == DECISIONS_MAX) return GATE_REFUSED;    /* be careful rather than forgetful */

    shown_app_name(tc_path, name, APP_ID_MAX);
    utf8_to_wide(rp->app, id, APP_ID_MAX);
    format_wide(question, MSG_MAX,
                L"Uninstall \"%s\" from the device?\n\n"
                L"%s\n\n"
                L"This removes the application and ALL its documents and data. It cannot be undone.",
                name, id);

    decision = &g_decisions[g_decisions_count++];
    copy_text(decision->udid, UDID_MAX, rp->udid);
    copy_text(decision->bundle, APP_ID_MAX, rp->app);
    decision->answer = ANSWER_NO;

    if (MessageBoxW(GetActiveWindow(), question, L"Uninstall application",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return GATE_REFUSED;

    if (!uninstall_application(rp->udid, rp->app, err, MSG_MAX)) {
        show_error(err);
        return GATE_REFUSED;
    }
    decision->answer = ANSWER_YES;
    return GATE_REMOVED;
}

/* --------------------------------------------------------------- install */

/* TRUE when 'tc_path' is "<something>.ipa" directly inside \Applications. */
BOOL apps_is_install_target(const WCHAR* tc_path, const RemotePath* rp)
{
    size_t len = wcslen(tc_path);

    /* A name directly under \Applications is parsed as an application with path "/". */
    if (rp->mode != M_APP || strcmp(rp->path, "/") != 0) return FALSE;
    return len > 4 && _wcsicmp(tc_path + len - 4, L".ipa") == 0;
}

static afc_error_t op_make_directory(afc_client_t afc, void* arg)
{
    return afc_make_directory(afc, (const char*)arg);
}

static afc_error_t op_remove(afc_client_t afc, void* arg)
{
    return afc_remove_path(afc, (const char*)arg);
}

/*
 * Installs an .ipa package: upload it to the staging folder of the device,
 * ask installation_proxy to install it, remove the uploaded file.
 * Returns an FS_FILE_ code for FsPutFileW.
 */
int apps_install(const WCHAR* local_name, const WCHAR* remote_name, const RemotePath* target)
{
    RemotePath staging;
    WCHAR err[MSG_MAX] = L"";
    char file_name[MAX_PATH * 3];
    const WCHAR* last_backslash = wcsrchr(local_name, L'\\');
    instproxy_client_t proxy;
    instproxy_error_t e;
    int result;

    /* The package goes to /PublicStaging/<file name> on the media partition. */
    memset(&staging, 0, sizeof(staging));
    staging.mode = M_USER;
    copy_text(staging.udid, UDID_MAX, target->udid);
    copy_text(staging.path, AFC_PATH_MAX, "/");
    afc_run(&staging, op_make_directory, STAGING_DIR, NULL, 0);     /* fails harmlessly when it exists */

    wide_to_utf8(last_backslash ? last_backslash + 1 : local_name, file_name, sizeof(file_name));
    _snprintf(staging.path, AFC_PATH_MAX - 1, "%s/%s", STAGING_DIR, file_name);
    staging.path[AFC_PATH_MAX - 1] = 0;

    result = upload_file(local_name, remote_name, FS_COPYFLAGS_OVERWRITE, &staging);
    if (result != FS_FILE_OK) return result;

    proxy = proxy_connect(target->udid, err, MSG_MAX);
    if (!proxy) {
        afc_run(&staging, op_remove, staging.path, NULL, 0);
        show_error(err);
        return FS_FILE_WRITEERROR;
    }
    /* The path is given relative to the media partition; no callback: runs to the end. */
    e = instproxy_install(proxy, staging.path + 1, NULL, NULL, NULL);
    instproxy_client_free(proxy);
    log_line("install %s: %d", staging.path, (int)e);
    afc_run(&staging, op_remove, staging.path, NULL, 0);

    if (e != INSTPROXY_E_SUCCESS) {
        format_wide(err, MSG_MAX,
                    L"The device did not install the package (error %d).\n"
                    L"The package must be signed for this device.", (int)e);
        show_error(err);
        return FS_FILE_WRITEERROR;
    }
    apps_table_clear(target->udid);     /* the list of applications changed */
    return FS_FILE_OK;
}
