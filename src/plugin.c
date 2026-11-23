/*
 * plugin.c - the functions Total Commander calls (the WFX interface).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Each function here is short: it turns the Total Commander path into a
 * RemotePath and hands the work to the module that does it.
 */
#include "wfx_ios.h"

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = instance;
        util_init();
        device_init();
        paths_init();
        icons_init();
        columns_init();
        photos_init();
        home_init();
    }
    return TRUE;
}

/* ------------------------------------------------------------------ set-up */

int __stdcall FsInit(int plugin_nr, tProgressProc progress, tLogProc log, tRequestProc request)
{
    (void)progress; (void)log; (void)request;       /* only the Unicode callbacks are used */
    g_plugin_nr = plugin_nr;
    return 0;
}

int __stdcall FsInitW(int plugin_nr, tProgressProcW progress, tLogProcW log, tRequestProcW request)
{
    (void)request;
    g_plugin_nr = plugin_nr;
    g_progress_proc = progress;
    g_log_proc = log;
    return 0;
}

/* Name shown in Network Neighborhood. */
void __stdcall FsGetDefRootName(char* name, int max_len)
{
    copy_text(name, max_len, "iOS");
}

/* Total Commander tells us which ini file plugins should use. */
void __stdcall FsSetDefaultParams(FsDefaultParamStruct* params)
{
    if (params && params->DefaultIniName[0]) copy_text(g_ini, sizeof(g_ini), params->DefaultIniName);
    lang_load();                                    /* the Language setting is in that ini file */
}

/* Downloads and uploads may run in a background thread. */
int __stdcall FsGetBackgroundFlags(void)
{
    return BG_DOWNLOAD | BG_UPLOAD;
}

/* ---------------------------------------------------------------- listings */

HANDLE __stdcall FsFindFirstW(WCHAR* path, WIN32_FIND_DATAW* find_data)
{
    WCHAR err[MSG_MAX] = L"";
    FindList* list;
    RemotePath rp;

    /* Deleting an application: after the user's answer there is nothing to list inside it. */
    parse_path(path, &rp);
    if (apps_delete_gate(path, &rp) != GATE_PROCEED) {
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }

    list = find_list_create(path, err, MSG_MAX);
    if (!list) {
        show_error(err);
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (list->count == 0) {                         /* an empty folder */
        find_list_free(list);
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }
    *find_data = list->items[0];
    list->next = 1;
    return (HANDLE)list;
}

BOOL __stdcall FsFindNextW(HANDLE handle, WIN32_FIND_DATAW* find_data)
{
    FindList* list = (FindList*)handle;

    if (!list || handle == INVALID_HANDLE_VALUE || list->next >= list->count) {
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    *find_data = list->items[list->next++];
    return TRUE;
}

int __stdcall FsFindClose(HANDLE handle)
{
    if (handle && handle != INVALID_HANDLE_VALUE) find_list_free((FindList*)handle);
    return 0;
}

/* The ANSI versions are required by the interface; they wrap the Unicode ones. */
static void find_data_to_ansi(const WIN32_FIND_DATAW* wide, WIN32_FIND_DATAA* ansi)
{
    memset(ansi, 0, sizeof(*ansi));
    ansi->dwFileAttributes = wide->dwFileAttributes;
    ansi->ftCreationTime = wide->ftCreationTime;
    ansi->ftLastAccessTime = wide->ftLastAccessTime;
    ansi->ftLastWriteTime = wide->ftLastWriteTime;
    ansi->nFileSizeHigh = wide->nFileSizeHigh;
    ansi->nFileSizeLow = wide->nFileSizeLow;
    WideCharToMultiByte(CP_ACP, 0, wide->cFileName, -1, ansi->cFileName, MAX_PATH - 1, NULL, NULL);
}

HANDLE __stdcall FsFindFirst(char* path, WIN32_FIND_DATAA* find_data)
{
    WCHAR wide_path[TC_PATH_MAX] = L"";
    WIN32_FIND_DATAW wide_data;
    HANDLE handle;

    MultiByteToWideChar(CP_ACP, 0, path ? path : "", -1, wide_path, TC_PATH_MAX - 1);
    handle = FsFindFirstW(wide_path, &wide_data);
    if (handle != INVALID_HANDLE_VALUE) find_data_to_ansi(&wide_data, find_data);
    return handle;
}

BOOL __stdcall FsFindNext(HANDLE handle, WIN32_FIND_DATAA* find_data)
{
    WIN32_FIND_DATAW wide_data;

    if (!FsFindNextW(handle, &wide_data)) return FALSE;
    find_data_to_ansi(&wide_data, find_data);
    return TRUE;
}

/* --------------------------------------------------------------- transfers */

int __stdcall FsGetFileW(WCHAR* remote_name, WCHAR* local_name, int copy_flags, RemoteInfoStruct* info)
{
    RemotePath rp;
    (void)info;
    parse_path(remote_name, &rp);
    if (rp.mode == M_HOME)
        return home_is_layout_file(&rp) ? home_download_layout(&rp, local_name, copy_flags) : FS_FILE_NOTSUPPORTED;
    if (!is_afc_mode(rp.mode)) return FS_FILE_NOTSUPPORTED;
    return download_file(remote_name, local_name, copy_flags, &rp);
}

int __stdcall FsPutFileW(WCHAR* local_name, WCHAR* remote_name, int copy_flags)
{
    RemotePath rp;
    parse_path(remote_name, &rp);
    if (apps_is_install_target(remote_name, &rp)) return apps_install(local_name, remote_name, &rp);
    if (rp.mode == M_HOME) {
        /* A saved layout file put back; Total Commander asks before "layout.plist" is replaced. */
        if (!home_is_layout_file(&rp)) return FS_FILE_NOTSUPPORTED;
        if (!(copy_flags & FS_COPYFLAGS_OVERWRITE) && _stricmp(rp.path, "/layout.plist") == 0) return FS_FILE_EXISTS;
        return home_upload_layout(local_name, &rp);
    }
    if (!is_afc_mode(rp.mode) || strcmp(rp.path, "/") == 0) return FS_FILE_NOTSUPPORTED;
    return upload_file(local_name, remote_name, copy_flags, &rp);
}

/* ------------------------------------------------- small device operations */

static afc_error_t op_exists(afc_client_t afc, void* arg)
{
    FileStat st;
    return afc_stat(afc, (const char*)arg, &st);
}

static afc_error_t op_remove(afc_client_t afc, void* arg)
{
    return afc_remove_path(afc, (const char*)arg);
}

static afc_error_t op_make_directory(afc_client_t afc, void* arg)
{
    return afc_make_directory(afc, (const char*)arg);
}

typedef struct { const char* from; const char* to; BOOL overwrite; } RenameArgs;

static afc_error_t op_rename(afc_client_t afc, void* arg)
{
    RenameArgs* args = (RenameArgs*)arg;
    if (args->overwrite) afc_remove_path(afc, args->to);    /* AFC does not replace an existing target */
    return afc_rename_path(afc, args->from, args->to);
}

typedef struct { const char* path; uint64_t mtime; } SetTimeArgs;

static afc_error_t op_set_time(afc_client_t afc, void* arg)
{
    SetTimeArgs* args = (SetTimeArgs*)arg;
    return afc_set_file_time(afc, args->path, args->mtime);
}

/* Rename, move or copy within the device. */
int __stdcall FsRenMovFileW(WCHAR* old_name, WCHAR* new_name, BOOL move, BOOL overwrite, RemoteInfoStruct* info)
{
    RemotePath src, dst;
    char src_key[KEY_MAX], dst_key[KEY_MAX];
    BOOL same_container;
    afc_error_t e;
    int result;

    (void)info;
    parse_path(old_name, &src);
    parse_path(new_name, &dst);
    if (src.mode == M_HOME && dst.mode == M_HOME) return move ? home_move(&src, &dst) : FS_FILE_NOTSUPPORTED;
    if (!is_afc_mode(src.mode) || !is_afc_mode(dst.mode)) return FS_FILE_NOTSUPPORTED;
    if (strcmp(src.path, "/") == 0 || strcmp(dst.path, "/") == 0) return FS_FILE_NOTSUPPORTED;

    connection_key(&src, src_key, sizeof(src_key));
    connection_key(&dst, dst_key, sizeof(dst_key));
    same_container = strcmp(src_key, dst_key) == 0;

    /* Does the target exist already? */
    e = afc_run(&dst, op_exists, dst.path, NULL, 0);
    if (e == AFC_E_SUCCESS) {
        if (!overwrite) return FS_FILE_EXISTS;
        if (same_container && strcmp(src.path, dst.path) == 0) return FS_FILE_OK;
    } else if (afc_is_connection_error(e)) {
        return FS_FILE_WRITEERROR;
    }

    /* A move inside one container is a plain rename on the device. */
    if (move && same_container) {
        RenameArgs args;
        args.from = src.path;
        args.to = dst.path;
        args.overwrite = overwrite;
        if (g_progress_proc && g_progress_proc(g_plugin_nr, old_name, new_name, 0)) return FS_FILE_USERABORT;
        e = afc_run(&src, op_rename, &args, NULL, 0);
        if (e == AFC_E_OBJECT_NOT_FOUND) return FS_FILE_NOTFOUND;
        if (e != AFC_E_SUCCESS) return FS_FILE_WRITEERROR;
        if (g_progress_proc) g_progress_proc(g_plugin_nr, old_name, new_name, 100);
        return FS_FILE_OK;
    }

    /* Everything else is a copy, followed by deleting the source for a move. */
    result = copy_on_device(old_name, new_name, &src, &dst);
    if (result == FS_FILE_OK && move) afc_run(&src, op_remove, src.path, NULL, 0);
    return result;
}

/* Deletes a file or an empty folder (AFC uses the same call for both). */
static BOOL delete_entry(const WCHAR* remote_name)
{
    RemotePath rp;

    parse_path(remote_name, &rp);
    switch (apps_delete_gate(remote_name, &rp)) {       /* uninstalling an application? see apps.c */
    case GATE_REMOVED: return TRUE;
    case GATE_REFUSED: return FALSE;
    default: break;
    }
    if (!is_afc_mode(rp.mode) || strcmp(rp.path, "/") == 0) return FALSE;
    return afc_run(&rp, op_remove, rp.path, NULL, 0) == AFC_E_SUCCESS;
}

BOOL __stdcall FsDeleteFileW(WCHAR* remote_name)
{
    return delete_entry(remote_name);
}

BOOL __stdcall FsRemoveDirW(WCHAR* remote_name)
{
    RemotePath rp;
    parse_path(remote_name, &rp);
    if (rp.mode == M_HOME) return home_remove_folder(&rp);
    return delete_entry(remote_name);
}

/* Total Commander announces the start and the end of every operation. */
void __stdcall FsStatusInfoW(WCHAR* remote_dir, int start_or_end, int operation)
{
    if (operation == FS_STATUS_OP_DELETE && remote_dir)
        apps_delete_operation(remote_dir, start_or_end == FS_STATUS_START);
}

BOOL __stdcall FsMkDirW(WCHAR* path)
{
    RemotePath rp;
    parse_path(path, &rp);
    if (rp.mode == M_HOME) return home_make_folder(&rp);
    if (!is_afc_mode(rp.mode) || strcmp(rp.path, "/") == 0) return FALSE;
    return afc_run(&rp, op_make_directory, rp.path, NULL, 0) == AFC_E_SUCCESS;
}

/* Only the modification time can be set on the device. */
BOOL __stdcall FsSetTimeW(WCHAR* remote_name, FILETIME* creation, FILETIME* last_access, FILETIME* last_write)
{
    RemotePath rp;
    SetTimeArgs args;

    (void)creation; (void)last_access;
    parse_path(remote_name, &rp);
    if (!is_afc_mode(rp.mode) || !last_write) return FALSE;
    args.path = rp.path;
    args.mtime = filetime_to_unix_ns(last_write);
    return afc_run(&rp, op_set_time, &args, NULL, 0) == AFC_E_SUCCESS;
}

/* ------------------------------------------------------- Enter / Alt+Enter */

int __stdcall FsExecuteFileW(HWND main_window, WCHAR* remote_name, WCHAR* verb)
{
    RemotePath rp;

    if (!verb) return FS_EXEC_ERROR;
    parse_path(remote_name, &rp);

    if (_wcsicmp(verb, L"open") == 0) {
        if (rp.mode == M_SETTINGS) { show_settings(main_window); return FS_EXEC_OK; }
        if (rp.mode == M_INFO)     { show_device_info(main_window, rp.udid); return FS_EXEC_OK; }
        if (rp.mode == M_HOME && !home_is_layout_file(&rp)) return FS_EXEC_OK;     /* an application: nothing to open */
        return FS_EXEC_YOURSELF;                    /* Total Commander downloads and opens the file */
    }
    if (_wcsnicmp(verb, L"properties", 10) == 0) {
        if (rp.mode == M_TOP || rp.mode == M_SETTINGS) show_settings(main_window);
        else if (rp.mode == M_INFO) show_device_info(main_window, rp.udid);
        else if (is_afc_mode(rp.mode)) show_file_properties(main_window, remote_name, &rp);
        return FS_EXEC_OK;
    }
    return FS_EXEC_ERROR;
}

int __stdcall FsExtractCustomIconW(WCHAR* remote_name, int extract_flags, HICON* icon)
{
    if (!remote_name || !icon) return FS_ICON_USEDEFAULT;
    return extract_icon(remote_name, extract_flags, icon);
}

/* ---------------------------------------------------------- custom columns */

int __stdcall FsContentGetSupportedField(int field_index, char* field_name, char* units, int max_len)
{
    return columns_get_field(field_index, field_name, units, max_len);
}

int __stdcall FsContentGetValueW(WCHAR* file_name, int field_index, int unit_index, void* value, int max_len, int flags)
{
    (void)unit_index;                               /* none of our fields has units */
    if (!file_name || !value) return ft_fileerror;
    return columns_get_value(file_name, field_index, value, max_len, flags);
}
