/*
 * dialogs.c - the settings dialog and the file properties dialog.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "wfx_ios.h"

#define MAX_DEVICES 16              /* entries in the device list of the dialog */
#define INFO_MAX    600             /* device information text */

/* -------------------------------------------------- device information text */

/* Reads one text value from lockdown and converts it for display. */
static void lockdown_value(lockdownd_client_t lockdown, const char* key, WCHAR* dst, int dst_len)
{
    plist_t node = NULL;
    char* value = NULL;

    dst[0] = 0;
    if (lockdownd_get_value(lockdown, NULL, key, &node) != LOCKDOWN_E_SUCCESS || !node) return;
    plist_get_string_val(node, &value);
    if (value) {
        utf8_to_wide(value, dst, dst_len);
        plist_mem_free(value);
    }
    plist_free(node);
}

/*
 * Name, model and iOS version of one connected device. These values can be
 * read without pairing. 'name' (may be NULL) receives the device name alone.
 */
static void describe_device(const char* udid, WCHAR* info, int info_len, WCHAR* name, int name_len)
{
    idevice_t dev = NULL;
    lockdownd_client_t lockdown = NULL;
    lockdownd_error_t e;
    WCHAR device_name[128], model[64], version[64], build[64];

    if (name) name[0] = 0;
    if (idevice_new_with_options(&dev, udid, IDEVICE_LOOKUP_USBMUX) != IDEVICE_E_SUCCESS || !dev) {
        dev = NULL;
        if (!ini_get_bool("AllowWiFi", FALSE) ||
            idevice_new_with_options(&dev, udid, IDEVICE_LOOKUP_NETWORK) != IDEVICE_E_SUCCESS || !dev) {
            copy_wide(info, info_len, L"Not connected.");
            return;
        }
    }
    e = lockdownd_client_new(dev, &lockdown, PLUGIN_LABEL);
    if (e != LOCKDOWN_E_SUCCESS || !lockdown) {
        lockdown_message(e, info, info_len);
        idevice_free(dev);
        return;
    }
    lockdown_value(lockdown, "DeviceName", device_name, 128);
    lockdown_value(lockdown, "ProductType", model, 64);
    lockdown_value(lockdown, "ProductVersion", version, 64);
    lockdown_value(lockdown, "BuildVersion", build, 64);
    lockdownd_client_free(lockdown);
    idevice_free(dev);

    if (name) copy_wide(name, name_len, device_name);
    format_wide(info, info_len, L"Name:\t%s\r\nModel:\t%s\r\niOS:\t%s (%s)", device_name, model, version, build);
}

/* AFC operation: reads total and free space of the media partition. */
static afc_error_t op_free_space(afc_client_t afc, void* arg)
{
    WCHAR* line = (WCHAR*)arg;      /* buffer of 128 characters */
    char* total = NULL;
    char* free_bytes = NULL;
    afc_error_t e;

    e = afc_get_device_info_key(afc, "FSTotalBytes", &total);
    if (e == AFC_E_SUCCESS) e = afc_get_device_info_key(afc, "FSFreeBytes", &free_bytes);
    if (e == AFC_E_SUCCESS && total && free_bytes) {
        format_wide(line, 128, L"\r\nFree:\t%.1f GB of %.1f GB",
                    _strtoui64(free_bytes, NULL, 10) / 1073741824.0,
                    _strtoui64(total, NULL, 10) / 1073741824.0);
    }
    free(total);
    free(free_bytes);
    return e;
}

/* --------------------------------------------------------- settings dialog */

/* What the dialog remembers while it is open. */
typedef struct {
    char udids[MAX_DEVICES][64];    /* combo box index -> UDID; index 0 is "any device" */
    int  count;
} SettingsState;

static BOOL is_checked(HWND dialog, int control_id)
{
    return IsDlgButtonChecked(dialog, control_id) == BST_CHECKED;
}

static void set_checked(HWND dialog, int control_id, BOOL checked)
{
    CheckDlgButton(dialog, control_id, checked ? BST_CHECKED : BST_UNCHECKED);
}

/* Shows the information for the device selected in the combo box. */
static void settings_show_info(HWND dialog, SettingsState* state)
{
    int selected = (int)SendDlgItemMessageW(dialog, IDC_DEVICE, CB_GETCURSEL, 0, 0);
    WCHAR info[INFO_MAX] = L"";
    WCHAR udid[64];
    HCURSOR old_cursor = SetCursor(LoadCursor(NULL, IDC_WAIT));

    if (selected > 0 && selected < state->count) {
        describe_device(state->udids[selected], info, INFO_MAX, NULL, 0);
        utf8_to_wide(state->udids[selected], udid, 64);
        append_wide(info, INFO_MAX, L"\r\nUDID:\t%s", udid);
    } else {
        /* "Any connected device": describe the one the plugin would use now. */
        Device* device = device_acquire("", FALSE, info, INFO_MAX);
        if (device) {
            RemotePath user;
            WCHAR space[128] = L"";
            describe_device(device->udid, info, INFO_MAX, NULL, 0);
            memset(&user, 0, sizeof(user));          /* the default device's media partition */
            user.mode = M_USER;
            copy_text(user.path, AFC_PATH_MAX, "/");
            afc_run(&user, op_free_space, space, NULL, 0);
            utf8_to_wide(device->udid, udid, 64);
            append_wide(info, INFO_MAX, L"%s\r\nUDID:\t%s", space, udid);
            device_release(device);
        }
    }
    SetCursor(old_cursor);
    SetDlgItemTextW(dialog, IDC_INFO, info);
}

/* Fills the combo box with "any device" plus the devices connected right now. */
static void settings_fill_devices(HWND dialog, SettingsState* state)
{
    HWND combo = GetDlgItem(dialog, IDC_DEVICE);
    usbmuxd_device_info_t* list = NULL;
    char preferred[64];
    BOOL wifi = is_checked(dialog, IDC_WIFI);
    int count, i, k;
    int selected = 0;

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    state->count = 1;
    state->udids[0][0] = 0;
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"<All connected devices>");

    count = usbmuxd_get_device_list(&list);
    for (i = 0; i < count && state->count < MAX_DEVICES - 1; i++) {
        BOOL network = list[i].conn_type == CONNECTION_TYPE_NETWORK;
        BOOL known = FALSE;
        WCHAR info[INFO_MAX], name[128], udid[64], label[256];

        if (network && !wifi) continue;
        for (k = 1; k < state->count; k++)
            if (strcmp(state->udids[k], list[i].udid) == 0) known = TRUE;
        if (known) continue;            /* the same device over USB and Wi-Fi */

        describe_device(list[i].udid, info, INFO_MAX, name, 128);
        utf8_to_wide(list[i].udid, udid, 64);
        if (name[0]) format_wide(label, 256, L"%s  (%s)%s", name, udid, network ? L"  [Wi-Fi]" : L"");
        else         format_wide(label, 256, L"%s%s", udid, network ? L"  [Wi-Fi]" : L"");
        copy_text(state->udids[state->count++], 64, list[i].udid);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)label);
    }
    if (list) usbmuxd_device_list_free(&list);

    /* Select the preferred device; list it even when it is not connected. */
    ini_get_text("PreferredDevice", preferred, sizeof(preferred));
    if (preferred[0]) {
        for (k = 1; k < state->count; k++)
            if (strcmp(state->udids[k], preferred) == 0) selected = k;
        if (selected == 0) {
            WCHAR udid[64], label[128];
            utf8_to_wide(preferred, udid, 64);
            format_wide(label, 128, L"%s  (not connected)", udid);
            selected = state->count;
            copy_text(state->udids[state->count++], 64, preferred);
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)label);
        }
    }
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
    settings_show_info(dialog, state);
}

/* Reads the options from the ini file into the dialog. */
static void settings_load(HWND dialog)
{
    WCHAR about[160];
    HICON icon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_PLUGIN));

    SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    SendMessageW(dialog, WM_SETICON, ICON_BIG, (LPARAM)icon);

    set_checked(dialog, IDC_APPNAMES, ini_get_bool("AppNames", TRUE));
    set_checked(dialog, IDC_ALLAPPS, ini_get_bool("ShowAllApps", FALSE));
    set_checked(dialog, IDC_WIFI, ini_get_bool("AllowWiFi", FALSE));
    set_checked(dialog, IDC_LOG, ini_get_bool("Log", TRUE));
    set_checked(dialog, IDC_DEBUG, ini_get_bool("Debug", FALSE));
    EnableWindow(GetDlgItem(dialog, IDC_DEBUG), is_checked(dialog, IDC_LOG));
    EnableWindow(GetDlgItem(dialog, IDC_OPENLOG), g_log_path[0] != 0);

    format_wide(about, 160, L"wfx_iOS %hs \x2013 update of wfx_iOS 1.3 by Domo, the original author", WFX_VERSION_STR);
    SetDlgItemTextW(dialog, IDC_ABOUT, about);
}

/* Writes the options from the dialog to the ini file and applies them. */
static void settings_save(HWND dialog, SettingsState* state)
{
    int selected = (int)SendDlgItemMessageW(dialog, IDC_DEVICE, CB_GETCURSEL, 0, 0);
    const char* udid = (selected > 0 && selected < state->count) ? state->udids[selected] : "";
    char old_udid[64];
    BOOL log_before = ini_get_bool("Log", TRUE);
    BOOL log = is_checked(dialog, IDC_LOG);
    BOOL debug = log && is_checked(dialog, IDC_DEBUG);

    ini_get_text("PreferredDevice", old_udid, sizeof(old_udid));
    ini_set_text("PreferredDevice", udid[0] ? udid : NULL);
    ini_set_text("AppNames", is_checked(dialog, IDC_APPNAMES) ? "1" : "0");
    ini_set_text("ShowAllApps", is_checked(dialog, IDC_ALLAPPS) ? "1" : "0");
    ini_set_text("AllowWiFi", is_checked(dialog, IDC_WIFI) ? "1" : "0");
    ini_set_text("Log", log ? "1" : "0");
    ini_set_text("Debug", debug ? "1" : "0");

    /* The trace level can change while running; opening or closing the log cannot. */
    if (g_log_path[0]) {
        idevice_set_debug_level(debug ? 1 : 0);
        libusbmuxd_set_debug_level(debug ? 1 : 0);
    }
    if (strcmp(udid, old_udid) != 0) device_forget();
    apps_table_clear(NULL);             /* names / "all applications" may have changed */

    if (log != log_before)
        MessageBoxW(dialog, L"The log file setting takes effect after Total Commander is restarted.",
                    PLUGIN_TITLE, MB_OK | MB_ICONINFORMATION);
}

static INT_PTR CALLBACK settings_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    SettingsState* state = (SettingsState*)GetWindowLongPtrW(dialog, GWLP_USERDATA);

    switch (message) {
    case WM_INITDIALOG:
        state = (SettingsState*)lparam;
        SetWindowLongPtrW(dialog, GWLP_USERDATA, (LONG_PTR)state);
        settings_load(dialog);
        settings_fill_devices(dialog, state);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_DEVICE:
            if (HIWORD(wparam) == CBN_SELCHANGE) settings_show_info(dialog, state);
            return TRUE;
        case IDC_REFRESH:
        case IDC_WIFI:                  /* the Wi-Fi option changes which devices are listed */
            settings_fill_devices(dialog, state);
            return TRUE;
        case IDC_LOG:
            EnableWindow(GetDlgItem(dialog, IDC_DEBUG), is_checked(dialog, IDC_LOG));
            return TRUE;
        case IDC_OPENLOG:
            if (g_log_path[0]) {
                WCHAR quoted[MAX_PATH + 4];
                format_wide(quoted, MAX_PATH + 4, L"\"%s\"", g_log_path);
                ShellExecuteW(dialog, L"open", L"notepad.exe", quoted, NULL, SW_SHOWNORMAL);
            }
            return TRUE;
        case IDOK:
            settings_save(dialog, state);
            EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* Opened by Enter on "Settings" and by Alt+Enter on the plugin root. */
void show_settings(HWND parent)
{
    SettingsState state;
    Device* device = device_acquire("", FALSE, NULL, 0);    /* also opens sockets and the log on first use */

    if (device) device_release(device);
    memset(&state, 0, sizeof(state));
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), parent, settings_proc, (LPARAM)&state);
}

/* --------------------------------------------------------- file properties */

/* Arguments and results of op_properties(). */
typedef struct {
    const char* path;
    FileStat    st;
    BOOL        counted;            /* TRUE when 'entries' is valid (folders only) */
    int         entries;
} PropertiesArgs;

/* AFC operation: attributes of one entry and, for a folder, how many entries it has. */
static afc_error_t op_properties(afc_client_t afc, void* arg)
{
    PropertiesArgs* args = (PropertiesArgs*)arg;
    afc_error_t e = afc_stat(afc, args->path, &args->st);
    char** names = NULL;
    int i;

    args->counted = FALSE;
    args->entries = 0;
    if (e != AFC_E_SUCCESS || !args->st.is_dir) return e;
    if (afc_read_directory(afc, args->path, &names) == AFC_E_SUCCESS && names) {
        for (i = 0; names[i]; i++)
            if (strcmp(names[i], ".") != 0 && strcmp(names[i], "..") != 0) args->entries++;
        afc_dictionary_free(names);
        args->counted = TRUE;
    }
    return e;
}

/* "File", "Directory", "Symbolic link", ... */
static const WCHAR* type_name(const FileStat* st)
{
    if (st->is_link) return L"Symbolic link";
    if (st->is_dir) return L"Directory";
    if (strcmp(st->ifmt, "S_IFBLK") == 0) return L"Block device";
    if (strcmp(st->ifmt, "S_IFCHR") == 0 || strcmp(st->ifmt, "S_IFIFO") == 0 ||
        strcmp(st->ifmt, "S_IFSOCK") == 0) return L"Device";
    return L"File";
}

/* Everything the properties dialog shows, prepared as text. */
typedef struct {
    WCHAR name[TC_PATH_MAX];
    WCHAR path[TC_PATH_MAX];
    WCHAR type[64];
    WCHAR size[128];
    WCHAR created[80];
    WCHAR modified[80];
    WCHAR links[32];
    WCHAR blocks[32];
    WCHAR target[TC_PATH_MAX];
} PropertiesText;

static INT_PTR CALLBACK properties_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_INITDIALOG) {
        const PropertiesText* text = (const PropertiesText*)lparam;
        HICON icon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_PLUGIN));
        WCHAR title[TC_PATH_MAX + 32];

        SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        format_wide(title, TC_PATH_MAX + 32, L"Properties of %s", text->name);
        SetWindowTextW(dialog, title);
        SetDlgItemTextW(dialog, IDC_P_NAME, text->name);
        SetDlgItemTextW(dialog, IDC_P_PATH, text->path);
        SetDlgItemTextW(dialog, IDC_P_TYPE, text->type);
        SetDlgItemTextW(dialog, IDC_P_SIZE, text->size);
        SetDlgItemTextW(dialog, IDC_P_CREATED, text->created);
        SetDlgItemTextW(dialog, IDC_P_MODIFIED, text->modified);
        SetDlgItemTextW(dialog, IDC_P_LINKS, text->links);
        SetDlgItemTextW(dialog, IDC_P_BLOCKS, text->blocks);
        SetDlgItemTextW(dialog, IDC_P_TARGET, text->target);
        SetFocus(GetDlgItem(dialog, IDOK));
        return FALSE;                   /* we set the focus ourselves */
    }
    if (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
        EndDialog(dialog, LOWORD(wparam));
        return TRUE;
    }
    return FALSE;
}

/* Alt+Enter on a file or folder. */
void show_file_properties(HWND parent, const WCHAR* remote_name, const RemotePath* rp)
{
    PropertiesArgs* args = (PropertiesArgs*)calloc(1, sizeof(PropertiesArgs));
    PropertiesText* text = (PropertiesText*)calloc(1, sizeof(PropertiesText));
    WCHAR err[MSG_MAX] = L"";
    const WCHAR* last_backslash;
    const FileStat* st;

    if (!args || !text) { free(args); free(text); return; }
    args->path = rp->path;
    if (afc_run(rp, op_properties, args, err, MSG_MAX) != AFC_E_SUCCESS) {
        show_error(err[0] ? err : L"Could not read file information.");
        free(args);
        free(text);
        return;
    }
    st = &args->st;

    /* Name = last component of the Total Commander path. */
    last_backslash = wcsrchr(remote_name, L'\\');
    copy_wide(text->name, TC_PATH_MAX, last_backslash ? last_backslash + 1 : remote_name);
    utf8_to_wide(rp->path, text->path, TC_PATH_MAX);
    copy_wide(text->type, 64, type_name(st));

    if (st->is_dir) {
        if (args->counted) format_wide(text->size, 128, L"%d entries", args->entries);
        else               copy_wide(text->size, 128, L"-");
    } else {
        WCHAR rounded[64];
        StrFormatByteSizeW((LONGLONG)st->size, rounded, 64);
        format_wide(text->size, 128, L"%s (%I64u bytes)", rounded, st->size);
    }
    format_time(st->birth, text->created, 80);
    format_time(st->mtime, text->modified, 80);
    format_wide(text->links, 32, L"%I64u", st->nlink);
    format_wide(text->blocks, 32, L"%I64u", st->blocks);
    if (st->is_link) utf8_to_wide(st->target, text->target, TC_PATH_MAX);
    else             copy_wide(text->target, TC_PATH_MAX, L"-");

    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_PROPERTIES), parent, properties_proc, (LPARAM)text);
    free(args);
    free(text);
}
