/*
 * columns.c - extra file details for Total Commander's custom columns,
 *             tooltips and search ("content plugin" part of the interface).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * To use them: Configuration > Options > Custom columns > New, add a column
 * and pick a field of plugin "iOS".
 */
#include "wfx_ios.h"

/* The fields we offer, in the order Total Commander numbers them. */
enum {
    FIELD_CREATED,
    FIELD_TYPE,
    FIELD_LINKS,
    FIELD_BLOCKS,
    FIELD_LINK_TARGET,
    FIELD_DEVICE_PATH,
    FIELD_BUNDLE_ID,
    FIELD_COUNT
};

static const struct {
    const char* name;
    int         type;
} g_fields[FIELD_COUNT] = {
    { "Created",     ft_datetime   },
    { "Type",        ft_string     },
    { "Hard links",  ft_numeric_32 },
    { "Blocks",      ft_numeric_64 },
    { "Link target", ft_stringw    },
    { "Device path", ft_stringw    },
    { "Bundle id",   ft_string     },
};

/*
 * Total Commander asks for the fields of a file one after the other. The
 * attributes of the file asked last are remembered, so that one row costs
 * one request to the device, not one per column.
 */
static CRITICAL_SECTION g_memo_lock;
static WCHAR            g_memo_name[TC_PATH_MAX];
static FileStat         g_memo_stat;
static BOOL             g_memo_valid = FALSE;

void columns_init(void)
{
    InitializeCriticalSection(&g_memo_lock);
}

int columns_get_field(int field_index, char* field_name, char* units, int max_len)
{
    if (field_index < 0 || field_index >= FIELD_COUNT) return ft_nomorefields;
    copy_text(field_name, max_len, g_fields[field_index].name);
    if (units && max_len > 0) units[0] = 0;
    return g_fields[field_index].type;
}

typedef struct {
    const char* path;
    FileStat*   st;
} StatArgs;

static afc_error_t op_stat(afc_client_t afc, void* arg)
{
    StatArgs* args = (StatArgs*)arg;
    return afc_stat(afc, args->path, args->st);
}

/* Gets the attributes of 'file_name', from the memo or from the device. */
static BOOL get_attributes(const WCHAR* file_name, const RemotePath* rp, FileStat* st, BOOL may_ask_device, BOOL* delayed)
{
    StatArgs args;
    BOOL ok;

    *delayed = FALSE;
    EnterCriticalSection(&g_memo_lock);
    ok = g_memo_valid && wcscmp(g_memo_name, file_name) == 0;
    if (ok) *st = g_memo_stat;
    LeaveCriticalSection(&g_memo_lock);
    if (ok) return TRUE;

    /* Asking the device takes time: let Total Commander call again in the background. */
    if (!may_ask_device) { *delayed = TRUE; return FALSE; }

    args.path = rp->path;
    args.st = st;
    if (afc_run(rp, op_stat, &args, NULL, 0) != AFC_E_SUCCESS) return FALSE;

    EnterCriticalSection(&g_memo_lock);
    copy_wide(g_memo_name, TC_PATH_MAX, file_name);
    g_memo_stat = *st;
    g_memo_valid = TRUE;
    LeaveCriticalSection(&g_memo_lock);
    return TRUE;
}

/*
 * Returns the value of one field for one file. 'max_len' is the size of
 * 'value' in bytes. The return value is the field type, or an ft_ error code.
 */
int columns_get_value(const WCHAR* file_name, int field_index, void* value, int max_len, int flags)
{
    RemotePath rp;
    FileStat st;
    BOOL delayed;

    if (field_index < 0 || field_index >= FIELD_COUNT) return ft_nosuchfield;
    parse_path(file_name, &rp);

    /* Fields that need no request to the device. */
    if (field_index == FIELD_BUNDLE_ID) {
        if (rp.mode != M_APP || strcmp(rp.path, "/") != 0) return ft_fieldempty;
        copy_text((char*)value, max_len, rp.app);
        return ft_string;
    }
    if (!is_afc_mode(rp.mode)) return ft_fieldempty;       /* our own virtual folders */
    if (field_index == FIELD_DEVICE_PATH) {
        utf8_to_wide(rp.path, (WCHAR*)value, max_len / (int)sizeof(WCHAR));
        return ft_stringw;
    }

    if (!get_attributes(file_name, &rp, &st, (flags & CONTENT_DELAYIFSLOW) == 0, &delayed))
        return delayed ? ft_delayed : ft_fileerror;

    switch (field_index) {
    case FIELD_CREATED:
        if (st.birth == 0) return ft_fieldempty;
        *(FILETIME*)value = unix_ns_to_filetime(st.birth);
        return ft_datetime;
    case FIELD_TYPE:
        copy_text((char*)value, max_len,
                  st.is_link ? "Symbolic link" : st.is_dir ? "Directory" :
                  strcmp(st.ifmt, "S_IFREG") == 0 ? "File" : "Device");
        return ft_string;
    case FIELD_LINKS:
        *(int*)value = (int)st.nlink;
        return ft_numeric_32;
    case FIELD_BLOCKS:
        *(__int64*)value = (__int64)st.blocks;
        return ft_numeric_64;
    case FIELD_LINK_TARGET:
        if (!st.is_link) return ft_fieldempty;
        utf8_to_wide(st.target, (WCHAR*)value, max_len / (int)sizeof(WCHAR));
        return ft_stringw;
    }
    return ft_nosuchfield;
}
