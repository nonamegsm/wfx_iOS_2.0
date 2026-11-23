/*
 * info.c - the "Device information" window: serial number, IMEI, addresses,
 *          storage, battery and every other value the device reports.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The values come from lockdownd. It keeps them in "domains"; the unnamed
 * domain holds the general facts (name, model, serial number, IMEI, ...),
 * others hold storage, battery and regional settings. A paired ("trusted")
 * device answers everything; an unpaired one only a few basic values.
 *
 * The window shows the well-known values first, with readable names, and
 * after them everything else the device returned, under the device's own
 * key names. Nothing of this is written to the log.
 */
#include "wfx_ios.h"
#include <commctrl.h>
#include <commdlg.h>

#define NAME_LEN   96
#define VALUE_LEN  600

/* One line of the window. */
typedef struct {
    WCHAR name[NAME_LEN];
    WCHAR value[VALUE_LEN];
    BOOL  is_heading;               /* a section title, no value */
} InfoRow;

/* The list of lines; it grows as values are added. */
typedef struct {
    InfoRow* rows;
    int      count;
    int      capacity;
} InfoList;

/* Well-known values of the general domain: device key -> readable name, in display order. */
typedef struct {
    const char*  key;
    const WCHAR* label;
} KnownKey;

static const KnownKey g_main_keys[] = {
    { "DeviceName",                          L"Name" },
    { "DeviceClass",                         L"Device class" },
    { "ProductType",                         L"Product type" },
    { "ModelNumber",                         L"Model number" },
    { "RegionInfo",                          L"Region" },
    { "HardwareModel",                       L"Hardware model" },
    { "CPUArchitecture",                     L"CPU architecture" },
    { "DeviceColor",                         L"Color" },
    { "ProductName",                         L"System" },
    { "ProductVersion",                      L"System version" },
    { "BuildVersion",                        L"Build" },
    { "FirmwareVersion",                     L"Firmware (iBoot)" },
    { "BasebandVersion",                     L"Baseband version" },
    { "ActivationState",                     L"Activation state" },
    { "PasswordProtected",                   L"Passcode set" },
    { "TimeZone",                            L"Time zone" },
};

static const KnownKey g_id_keys[] = {
    { "SerialNumber",                        L"Serial number" },
    { "UniqueDeviceID",                      L"UDID" },
    { "UniqueChipID",                        L"ECID" },
    { "ChipID",                              L"Chip ID" },
    { "BoardId",                             L"Board ID" },
    { "InternationalMobileEquipmentIdentity",  L"IMEI" },
    { "InternationalMobileEquipmentIdentity2", L"IMEI 2" },
    { "MobileEquipmentIdentifier",           L"MEID" },
    { "IntegratedCircuitCardIdentity",       L"ICCID" },
    { "InternationalMobileSubscriberIdentity", L"IMSI" },
    { "PhoneNumber",                         L"Phone number" },
    { "MLBSerialNumber",                     L"Logic board serial" },
    { "WiFiAddress",                         L"Wi-Fi address" },
    { "BluetoothAddress",                    L"Bluetooth address" },
    { "EthernetAddress",                     L"Ethernet address" },
};

#define COUNT_OF(array) ((int)(sizeof(array) / sizeof((array)[0])))

/* ------------------------------------------------------------ the list */

static InfoRow* add_row(InfoList* list, const WCHAR* name, BOOL is_heading)
{
    InfoRow* row;

    if (list->count == list->capacity) {
        int capacity = list->capacity ? list->capacity * 2 : 128;
        InfoRow* grown = (InfoRow*)realloc(list->rows, capacity * sizeof(InfoRow));
        if (!grown) return NULL;
        list->rows = grown;
        list->capacity = capacity;
    }
    row = &list->rows[list->count++];
    memset(row, 0, sizeof(*row));
    copy_wide(row->name, NAME_LEN, name);
    row->is_heading = is_heading;
    return row;
}

static void add_heading(InfoList* list, StringId title)
{
    add_row(list, tr(title), TRUE);
}

/* ------------------------------------------------- plist value -> text */

/* Turns one value of the device's answer into display text. */
static void value_to_text(plist_t node, WCHAR* dst, int dst_len)
{
    dst[0] = 0;
    switch (plist_get_node_type(node)) {
    case PLIST_STRING: {
        char* text = NULL;
        plist_get_string_val(node, &text);
        if (text) { utf8_to_wide(text, dst, dst_len); plist_mem_free(text); }
        break; }
    case PLIST_UINT: {
        uint64_t number = 0;
        plist_get_uint_val(node, &number);
        format_wide(dst, dst_len, L"%I64u", number);
        break; }
    case PLIST_REAL: {
        double number = 0;
        plist_get_real_val(node, &number);
        format_wide(dst, dst_len, L"%.3f", number);
        break; }
    case PLIST_BOOLEAN: {
        uint8_t flag = 0;
        plist_get_bool_val(node, &flag);
        copy_wide(dst, dst_len, tr(flag ? S_YES : S_NO));
        break; }
    case PLIST_DATE: {
        int64_t seconds = 0;                    /* since 1970 */
        plist_get_unix_date_val(node, &seconds);
        if (seconds > 0) format_time((uint64_t)seconds * 1000000000ULL, dst, dst_len);
        break; }
    case PLIST_DATA: {
        /* Binary data: the first bytes as hex. */
        char* data = NULL;
        uint64_t length = 0, i;
        plist_get_data_val(node, &data, &length);
        for (i = 0; data && i < length && i < 24; i++)
            append_wide(dst, dst_len, L"%02X ", (unsigned char)data[i]);
        if (length > 24) append_wide(dst, dst_len, L"... (%I64u bytes)", length);
        if (data) plist_mem_free(data);
        break; }
    case PLIST_ARRAY: {
        /* A list: the simple items, separated by commas. */
        uint32_t count = plist_array_get_size(node), i;
        for (i = 0; i < count && i < 16; i++) {
            WCHAR item[128];
            plist_t child = plist_array_get_item(node, i);
            plist_type type = plist_get_node_type(child);
            if (type == PLIST_ARRAY || type == PLIST_DICT) continue;
            value_to_text(child, item, 128);
            append_wide(dst, dst_len, L"%s%s", dst[0] ? L", " : L"", item);
        }
        if (dst[0] == 0) format_wide(dst, dst_len, L"(%u)", (unsigned)count);
        break; }
    case PLIST_DICT:
        format_wide(dst, dst_len, L"{ %u }", (unsigned)plist_dict_get_size(node));
        break;
    default:
        break;
    }
}

/* Adds the values of 'keys' that the device returned, in the given order. */
static void add_known(InfoList* list, plist_t dict, const KnownKey* keys, int key_count)
{
    int i;
    for (i = 0; i < key_count; i++) {
        plist_t node = plist_dict_get_item(dict, keys[i].key);
        InfoRow* row;
        if (!node) continue;
        row = add_row(list, keys[i].label, FALSE);
        if (row) value_to_text(node, row->value, VALUE_LEN);
    }
}

static BOOL is_known(const char* key)
{
    int i;
    for (i = 0; i < COUNT_OF(g_main_keys); i++) if (strcmp(key, g_main_keys[i].key) == 0) return TRUE;
    for (i = 0; i < COUNT_OF(g_id_keys); i++)   if (strcmp(key, g_id_keys[i].key) == 0) return TRUE;
    return FALSE;
}

static int compare_rows(const void* a, const void* b)
{
    return _wcsicmp(((const InfoRow*)a)->name, ((const InfoRow*)b)->name);
}

/*
 * Adds every value of a dictionary under the device's own key names, sorted.
 *   skip_known   leave out the values already shown with readable names
 *   sizes        the numbers are byte counts: add "(12.3 GB)"
 */
static void add_all(InfoList* list, plist_t dict, BOOL skip_known, BOOL sizes)
{
    plist_dict_iter iterator = NULL;
    int first = list->count;

    plist_dict_new_iter(dict, &iterator);
    for (;;) {
        char* key = NULL;
        plist_t node = NULL;
        WCHAR name[NAME_LEN];
        InfoRow* row;

        plist_dict_next_item(dict, iterator, &key, &node);
        if (!key) break;
        if (!(skip_known && is_known(key))) {
            utf8_to_wide(key, name, NAME_LEN);
            row = add_row(list, name, FALSE);
            if (row) {
                value_to_text(node, row->value, VALUE_LEN);
                if (sizes && plist_get_node_type(node) == PLIST_UINT) {
                    uint64_t bytes = 0;
                    WCHAR rounded[64];
                    plist_get_uint_val(node, &bytes);
                    StrFormatByteSizeW((LONGLONG)bytes, rounded, 64);
                    append_wide(row->value, VALUE_LEN, L"  (%s)", rounded);
                }
            }
        }
        plist_mem_free(key);
    }
    free(iterator);
    if (list->count > first)
        qsort(&list->rows[first], list->count - first, sizeof(InfoRow), compare_rows);
}

/* Reads one whole domain and adds it as a section. Silently skipped when the device refuses. */
static void add_domain(InfoList* list, lockdownd_client_t lockdown, const char* domain, StringId title, BOOL sizes)
{
    plist_t dict = NULL;

    if (lockdownd_get_value(lockdown, domain, NULL, &dict) != LOCKDOWN_E_SUCCESS || !dict) return;
    if (plist_get_node_type(dict) == PLIST_DICT && plist_dict_get_size(dict) > 0) {
        add_heading(list, title);
        add_all(list, dict, FALSE, sizes);
    }
    plist_free(dict);
}

/* Collects everything for one device. Returns FALSE and fills 'err' when it cannot be reached. */
static BOOL collect(const char* udid, InfoList* list, WCHAR* err, int err_len)
{
    Device* device = device_acquire(udid, FALSE, err, err_len);
    lockdownd_client_t lockdown = NULL;
    plist_t general = NULL;
    BOOL trusted = TRUE;

    if (!device) return FALSE;

    /* A full session needs a trusted device; without it only basic values are readable. */
    if (lockdownd_client_new_with_handshake(device->dev, &lockdown, PLUGIN_LABEL) != LOCKDOWN_E_SUCCESS || !lockdown) {
        lockdownd_error_t e;
        trusted = FALSE;
        lockdown = NULL;
        e = lockdownd_client_new(device->dev, &lockdown, PLUGIN_LABEL);
        if (e != LOCKDOWN_E_SUCCESS || !lockdown) {
            lockdown_message(e, err, err_len);
            device_release(device);
            return FALSE;
        }
    }

    if (!trusted) add_row(list, tr(S_INFO_LIMITED), TRUE);

    if (lockdownd_get_value(lockdown, NULL, NULL, &general) == LOCKDOWN_E_SUCCESS && general &&
        plist_get_node_type(general) == PLIST_DICT) {
        add_heading(list, S_SEC_MAIN);
        add_known(list, general, g_main_keys, COUNT_OF(g_main_keys));
        add_heading(list, S_SEC_IDS);
        add_known(list, general, g_id_keys, COUNT_OF(g_id_keys));
    }

    add_domain(list, lockdown, "com.apple.disk_usage", S_SEC_STORAGE, TRUE);
    add_domain(list, lockdown, "com.apple.mobile.battery", S_SEC_BATTERY, FALSE);
    add_domain(list, lockdown, "com.apple.international", S_SEC_REGION, FALSE);

    if (general && plist_get_node_type(general) == PLIST_DICT) {
        add_heading(list, S_SEC_OTHER);
        add_all(list, general, TRUE, FALSE);
    }
    if (general) plist_free(general);
    lockdownd_client_free(lockdown);
    device_release(device);
    return TRUE;
}

/* ---------------------------------------------------- copy and save */

/* The whole list as text: "[Section]" lines and "name<TAB>value" lines. Caller frees. */
static WCHAR* list_to_text(const InfoList* list)
{
    size_t capacity = (size_t)list->count * (NAME_LEN + VALUE_LEN + 8) + 1;
    WCHAR* text = (WCHAR*)malloc(capacity * sizeof(WCHAR));
    size_t used = 0;
    int i;

    if (!text) return NULL;
    text[0] = 0;
    for (i = 0; i < list->count; i++) {
        const InfoRow* row = &list->rows[i];
        if (row->is_heading)
            format_wide(text + used, (int)(capacity - used), L"%s[%s]\r\n", i ? L"\r\n" : L"", row->name);
        else
            format_wide(text + used, (int)(capacity - used), L"%s\t%s\r\n", row->name, row->value);
        used += wcslen(text + used);
    }
    return text;
}

static void copy_to_clipboard(HWND owner, const WCHAR* text)
{
    size_t bytes = (wcslen(text) + 1) * sizeof(WCHAR);
    HGLOBAL memory;

    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        memcpy(GlobalLock(memory), text, bytes);
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    }
    CloseClipboard();
}

/* Asks for a file name and writes the list as a UTF-8 text file. */
static void save_to_file(HWND owner, const InfoList* list)
{
    WCHAR path[MAX_PATH] = L"device-info.txt";
    OPENFILENAMEW dialog;
    WCHAR* text;
    char* utf8;
    int utf8_len;
    HANDLE file;
    DWORD written;

    memset(&dialog, 0, sizeof(dialog));
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"Text (*.txt)\0*.txt\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return;

    text = list_to_text(list);
    if (!text) return;
    utf8_len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    utf8 = (char*)malloc(utf8_len > 0 ? utf8_len : 1);
    if (utf8 && utf8_len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, utf8_len, NULL, NULL);
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            WriteFile(file, "\xEF\xBB\xBF", 3, &written, NULL);         /* UTF-8 byte order mark */
            WriteFile(file, utf8, (DWORD)(utf8_len - 1), &written, NULL);
            CloseHandle(file);
        } else {
            MessageBoxW(owner, tr(S_ERR_SAVE), PLUGIN_TITLE, MB_OK | MB_ICONWARNING);
        }
    }
    free(utf8);
    free(text);
}

/* --------------------------------------------------------- the window */

/* Moves the controls when the window is resized: the list fills it, the buttons stay at the bottom. */
static void layout(HWND dialog)
{
    static const int buttons[] = { IDC_INFO_COPY, IDC_INFO_COPYALL, IDC_INFO_SAVE };
    RECT client, button;
    int margin, button_width, button_height, top, i;

    GetClientRect(dialog, &client);
    GetWindowRect(GetDlgItem(dialog, IDCANCEL), &button);
    button_width = button.right - button.left;
    button_height = button.bottom - button.top;
    margin = button_height / 2;
    top = client.bottom - margin - button_height;

    MoveWindow(GetDlgItem(dialog, IDC_INFO_LIST), margin, margin,
               client.right - 2 * margin, top - 2 * margin, TRUE);
    for (i = 0; i < 3; i++) {
        RECT r;
        HWND control = GetDlgItem(dialog, buttons[i]);
        GetWindowRect(control, &r);
        MoveWindow(control, margin + i * ((r.right - r.left) + margin / 2), top, r.right - r.left, button_height, TRUE);
    }
    MoveWindow(GetDlgItem(dialog, IDCANCEL), client.right - margin - button_width, top, button_width, button_height, TRUE);
    ListView_SetColumnWidth(GetDlgItem(dialog, IDC_INFO_LIST), 1, LVSCW_AUTOSIZE_USEHEADER);
}

/* Fills the list control from the collected lines. */
static void fill_list(HWND dialog, const InfoList* list)
{
    HWND view = GetDlgItem(dialog, IDC_INFO_LIST);
    LVCOLUMNW column;
    int i;

    ListView_SetExtendedListViewStyle(view, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    memset(&column, 0, sizeof(column));
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = 220;
    column.pszText = (WCHAR*)tr(S_COL_PROPERTY);
    ListView_InsertColumn(view, 0, &column);
    column.cx = 400;
    column.pszText = (WCHAR*)tr(S_COL_VALUE);
    ListView_InsertColumn(view, 1, &column);

    for (i = 0; i < list->count; i++) {
        const InfoRow* row = &list->rows[i];
        WCHAR heading[NAME_LEN + 8];
        LVITEMW item;

        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT;
        item.iItem = i;
        if (row->is_heading) {
            format_wide(heading, NAME_LEN + 8, L"\x2014 %s \x2014", row->name);
            item.pszText = heading;
        } else {
            item.pszText = (WCHAR*)row->name;
        }
        ListView_InsertItem(view, &item);
        ListView_SetItemText(view, i, 1, (WCHAR*)row->value);
    }
    ListView_SetColumnWidth(view, 0, LVSCW_AUTOSIZE);
}

static INT_PTR CALLBACK info_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    InfoList* list = (InfoList*)GetWindowLongPtrW(dialog, GWLP_USERDATA);

    switch (message) {
    case WM_INITDIALOG: {
        HICON icon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_PLUGIN));
        list = (InfoList*)lparam;
        SetWindowLongPtrW(dialog, GWLP_USERDATA, (LONG_PTR)list);
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        SetWindowTextW(dialog, tr(S_DLG_INFO));
        SetDlgItemTextW(dialog, IDC_INFO_COPY, tr(S_BTN_COPY_VALUE));
        SetDlgItemTextW(dialog, IDC_INFO_COPYALL, tr(S_BTN_COPY_ALL));
        SetDlgItemTextW(dialog, IDC_INFO_SAVE, tr(S_BTN_SAVE));
        SetDlgItemTextW(dialog, IDCANCEL, tr(S_BTN_CLOSE));
        fill_list(dialog, list);
        layout(dialog);
        return TRUE; }

    case WM_SIZE:
        layout(dialog);
        return TRUE;

    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lparam)->ptMinTrackSize.x = 520;
        ((MINMAXINFO*)lparam)->ptMinTrackSize.y = 300;
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_INFO_COPY: {
            /* The value of the selected line. */
            int selected = ListView_GetNextItem(GetDlgItem(dialog, IDC_INFO_LIST), -1, LVNI_SELECTED);
            if (selected >= 0 && selected < list->count && !list->rows[selected].is_heading)
                copy_to_clipboard(dialog, list->rows[selected].value);
            return TRUE; }
        case IDC_INFO_COPYALL: {
            WCHAR* text = list_to_text(list);
            if (text) { copy_to_clipboard(dialog, text); free(text); }
            return TRUE; }
        case IDC_INFO_SAVE:
            save_to_file(dialog, list);
            return TRUE;
        case IDOK:
        case IDCANCEL:
            EndDialog(dialog, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* Opens the window for one device ('udid' empty: the default device). */
void show_device_info(HWND parent, const char* udid)
{
    INITCOMMONCONTROLSEX controls;
    InfoList list;
    WCHAR err[MSG_MAX] = L"";
    HCURSOR old_cursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
    BOOL ok;

    memset(&list, 0, sizeof(list));
    ok = collect(udid, &list, err, MSG_MAX);
    SetCursor(old_cursor);

    if (!ok) {
        MessageBoxW(parent, err, PLUGIN_TITLE, MB_OK | MB_ICONWARNING);
    } else {
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_LISTVIEW_CLASSES;
        InitCommonControlsEx(&controls);                /* the list control lives in comctl32 */
        DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_INFO), parent, info_proc, (LPARAM)&list);
    }
    free(list.rows);
}
