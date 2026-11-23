/*
 * photos.c - "Photos by month": the whole photo library grouped by the month
 *            a picture was taken, with the original file names.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * On the device, pictures are spread over technical folders:
 *     /DCIM/100APPLE/IMG_0001.HEIC                       taken on this device
 *     /PhotoData/CPLAssets/group123/<UUID>.JPG           synced from iCloud Photos
 * The Photos application keeps a database, /PhotoData/Photos.sqlite, that
 * knows for every picture where the file is, when it was taken and how it was
 * originally named. We copy that database to a temporary folder, read those
 * three facts from it, delete the copy and keep the result in memory.
 *
 *     \Photos by month              one folder per month, "2026-11"
 *     \Photos by month\2026-11      the pictures of that month
 *
 * Only pictures whose file is really on the device are listed. With
 * "Optimise Storage" turned on, many originals exist only in iCloud.
 */
#include "wfx_ios.h"
#include <sqlite3.h>

#define PHOTO_DB        "/PhotoData/Photos.sqlite"
#define NAME_MAX_LEN    100
#define REAL_MAX_LEN    176
#define UNKNOWN_MONTH   0               /* pictures without a date */

/* Seconds between 1970-01-01 (Unix) and 2001-01-01 (Apple's reference date). */
#define APPLE_EPOCH_OFFSET 978307200.0

/* One picture or video of the library. */
typedef struct {
    int      month;                     /* year * 100 + month, e.g. 202611 */
    uint64_t taken_ns;                  /* when it was taken, ns since 1970 (0 = unknown) */
    char     name[NAME_MAX_LEN];        /* name shown to the user, UTF-8 */
    char     real[REAL_MAX_LEN];        /* path of the file on the device */
} PhotoItem;

static CRITICAL_SECTION g_photos_lock;          /* protects everything below */
static PhotoItem*       g_items = NULL;         /* sorted by month, then name */
static int              g_items_count = 0;
static int              g_items_capacity = 0;
static char             g_items_udid[UDID_MAX]; /* device the index belongs to */
static BOOL             g_items_loaded = FALSE;
static uint64_t         g_db_stamp = 0;         /* size + time of the database when it was read */
static DWORD            g_loaded_tick = 0;      /* when the index was read */

/* The Photos application touches its database all the time; do not re-read it more often than this. */
#define RELOAD_INTERVAL_MS (5 * 60 * 1000)

void photos_init(void)
{
    InitializeCriticalSection(&g_photos_lock);
}

/* ------------------------------------------------------- device helpers */

/* A RemotePath for the media partition of one device. */
static void user_partition(const char* udid, RemotePath* rp)
{
    memset(rp, 0, sizeof(*rp));
    rp->mode = M_USER;
    copy_text(rp->udid, UDID_MAX, udid);
    copy_text(rp->path, AFC_PATH_MAX, "/");
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

static BOOL stat_on_device(const char* udid, const char* path, FileStat* st)
{
    RemotePath rp;
    StatArgs args;

    user_partition(udid, &rp);
    args.path = path;
    args.st = st;
    return afc_run(&rp, op_stat, &args, NULL, 0) == AFC_E_SUCCESS;
}

/* TRUE when the device has a photo database we can try to read. */
BOOL photos_available(const char* udid)
{
    FileStat st;
    return stat_on_device(udid, PHOTO_DB, &st) && !st.is_dir && st.size > 0;
}

typedef struct {
    const char*  device_path;
    const WCHAR* local_path;
} FetchArgs;

/* AFC operation: copies one file from the device to the PC, without progress display. */
static afc_error_t op_fetch(afc_client_t afc, void* arg)
{
    FetchArgs* args = (FetchArgs*)arg;
    uint64_t handle = 0;
    afc_error_t e;
    HANDLE out;
    char* buffer;

    e = afc_file_open(afc, args->device_path, AFC_FOPEN_RDONLY, &handle);
    if (e != AFC_E_SUCCESS) return e;

    out = CreateFileW(args->local_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    buffer = (char*)malloc(IO_CHUNK);
    if (out == INVALID_HANDLE_VALUE || !buffer) {
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        free(buffer);
        afc_file_close(afc, handle);
        return AFC_E_IO_ERROR;
    }
    for (;;) {
        uint32_t got = 0;
        DWORD written = 0;
        e = afc_file_read(afc, handle, buffer, IO_CHUNK, &got);
        if (e != AFC_E_SUCCESS || got == 0) break;
        if (!WriteFile(out, buffer, got, &written, NULL) || written != got) { e = AFC_E_IO_ERROR; break; }
    }
    free(buffer);
    CloseHandle(out);
    afc_file_close(afc, handle);
    return e;
}

static BOOL fetch_file(const char* udid, const char* device_path, const WCHAR* local_path)
{
    RemotePath rp;
    FetchArgs args;

    user_partition(udid, &rp);
    args.device_path = device_path;
    args.local_path = local_path;
    return afc_run(&rp, op_fetch, &args, NULL, 0) == AFC_E_SUCCESS;
}

/* ---------------------------------------------------- reading the database */

/* Adds one picture to the index. Caller holds g_photos_lock. */
static void add_item(double apple_seconds, BOOL has_date, const char* directory, const char* file, const char* original)
{
    PhotoItem* item;
    char* p;

    if (g_items_count == g_items_capacity) {
        int capacity = g_items_capacity ? g_items_capacity * 2 : 4096;
        PhotoItem* grown = (PhotoItem*)realloc(g_items, capacity * sizeof(PhotoItem));
        if (!grown) return;
        g_items = grown;
        g_items_capacity = capacity;
    }
    item = &g_items[g_items_count];
    memset(item, 0, sizeof(*item));

    /* File on the device: "/<directory>/<file>". Too long paths cannot be ours. */
    if (strlen(directory) + strlen(file) + 3 > REAL_MAX_LEN) return;
    _snprintf(item->real, REAL_MAX_LEN - 1, "/%s/%s", directory, file);

    /* Name for the user: the original name when the database has one. */
    copy_text(item->name, NAME_MAX_LEN, (original && original[0]) ? original : file);
    for (p = item->name; *p; p++)
        if ((unsigned char)*p < 32 || strchr("\\/:*?\"<>|", *p)) *p = '_';

    /* Month in local time. */
    item->month = UNKNOWN_MONTH;
    if (has_date) {
        double unix_seconds = apple_seconds + APPLE_EPOCH_OFFSET;
        if (unix_seconds > 0) {
            FILETIME ft;
            SYSTEMTIME utc, local;
            item->taken_ns = (uint64_t)(unix_seconds * 1000000000.0);
            ft = unix_ns_to_filetime(item->taken_ns);
            if (FileTimeToSystemTime(&ft, &utc)) {
                if (!SystemTimeToTzSpecificLocalTime(NULL, &utc, &local)) local = utc;
                item->month = local.wYear * 100 + local.wMonth;
            }
        }
    }
    g_items_count++;
}

/*
 * Runs one query that returns: date taken, directory, file name, original name.
 * Returns the number of rows, or -1 when the query does not fit this database.
 */
static int run_query(sqlite3* db, const char* sql)
{
    sqlite3_stmt* statement = NULL;
    int rows = 0;

    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) {
        log_line("photo database: query not possible: %s", sqlite3_errmsg(db));
        return -1;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char* directory = (const char*)sqlite3_column_text(statement, 1);
        const char* file = (const char*)sqlite3_column_text(statement, 2);
        const char* original = (const char*)sqlite3_column_text(statement, 3);
        BOOL has_date = sqlite3_column_type(statement, 0) != SQLITE_NULL;

        if (!directory || !file || !directory[0] || !file[0]) continue;
        add_item(sqlite3_column_double(statement, 0), has_date, directory, file, original);
        rows++;
    }
    sqlite3_finalize(statement);
    return rows;
}

/*
 * The table and column names changed between iOS versions. Try the known
 * layouts from the newest to the oldest; the first that works is used.
 */
static int read_database(sqlite3* db)
{
    static const char* queries[] = {
        /* iOS 14 and later: assets with their original file name */
        "SELECT a.ZDATECREATED, a.ZDIRECTORY, a.ZFILENAME, x.ZORIGINALFILENAME "
        "FROM ZASSET a LEFT JOIN ZADDITIONALASSETATTRIBUTES x ON x.ZASSET = a.Z_PK "
        "WHERE a.ZTRASHEDSTATE = 0",
        /* the same without the original name */
        "SELECT ZDATECREATED, ZDIRECTORY, ZFILENAME, NULL FROM ZASSET WHERE ZTRASHEDSTATE = 0",
        /* iOS 13 and earlier */
        "SELECT ZDATECREATED, ZDIRECTORY, ZFILENAME, NULL FROM ZGENERICASSET WHERE ZTRASHEDSTATE = 0",
    };
    int i, rows;

    for (i = 0; i < (int)(sizeof(queries) / sizeof(queries[0])); i++) {
        g_items_count = 0;
        rows = run_query(db, queries[i]);
        if (rows >= 0) {
            log_line("photo database: layout %d, %d pictures", i, rows);
            return rows;
        }
    }
    return -1;
}

/* Sort order of the index: month, then name (ignoring case), then device path. */
static int compare_items(const void* a, const void* b)
{
    const PhotoItem* x = (const PhotoItem*)a;
    const PhotoItem* y = (const PhotoItem*)b;
    int c;

    if (x->month != y->month) return x->month < y->month ? -1 : 1;
    c = _stricmp(x->name, y->name);
    if (c != 0) return c;
    return strcmp(x->real, y->real);
}

/*
 * Two pictures of one month can have the same name (two cameras both made an
 * IMG_0001.JPG). The index is sorted, so equal names are neighbours: the
 * second gets " (2)" before the extension, the third " (3)", and so on.
 */
static void make_names_unique(void)
{
    char base[NAME_MAX_LEN] = "";
    int base_month = -1;
    int repeat = 1;
    int i;

    for (i = 0; i < g_items_count; i++) {
        PhotoItem* item = &g_items[i];

        if (item->month == base_month && _stricmp(item->name, base) == 0) {
            char renamed[NAME_MAX_LEN + 16];
            char* dot = strrchr(item->name, '.');
            repeat++;
            if (dot) {
                *dot = 0;
                _snprintf(renamed, sizeof(renamed) - 1, "%s (%d).%s", item->name, repeat, dot + 1);
            } else {
                _snprintf(renamed, sizeof(renamed) - 1, "%s (%d)", item->name, repeat);
            }
            renamed[sizeof(renamed) - 1] = 0;
            copy_text(item->name, NAME_MAX_LEN, renamed);
        } else {
            copy_text(base, NAME_MAX_LEN, item->name);
            base_month = item->month;
            repeat = 1;
        }
    }
}

/*
 * Makes sure the index for 'udid' is in memory. With 'check_for_changes' the
 * database is read again when it changed on the device (done when the list of
 * months is opened, at most every five minutes). Caller holds g_photos_lock.
 */
static BOOL load_index(const char* udid, BOOL check_for_changes, WCHAR* err, int err_len)
{
    static const char* suffixes[3] = { "", "-wal", "-shm" };    /* the database and its journal files */
    WCHAR folder[MAX_PATH], local[3][MAX_PATH];
    char device_path[128], local_utf8[MAX_PATH * 3];
    FileStat st;
    uint64_t stamp = 0;
    sqlite3* db = NULL;
    int rows = -1;
    int i;

    /* Asking the device costs time; most callers are happy with what we have. */
    if (g_items_loaded && strcmp(g_items_udid, udid) == 0) {
        if (!check_for_changes) return TRUE;
        if (GetTickCount() - g_loaded_tick < RELOAD_INTERVAL_MS) return TRUE;
    }

    /* Size and time of the database files tell us whether anything changed. */
    for (i = 0; i < 2; i++) {
        _snprintf(device_path, sizeof(device_path) - 1, "%s%s", PHOTO_DB, suffixes[i]);
        device_path[sizeof(device_path) - 1] = 0;
        if (stat_on_device(udid, device_path, &st)) stamp += st.size * 31 + st.mtime;
        else if (i == 0) {
            if (err) copy_wide(err, err_len, tr(S_ERR_PHOTO_DB_MISSING));
            return FALSE;
        }
    }
    if (g_items_loaded && strcmp(g_items_udid, udid) == 0 && stamp == g_db_stamp) return TRUE;

    /* Copy the database to a private temporary folder. */
    GetTempPathW(MAX_PATH, folder);
    append_wide(folder, MAX_PATH, L"wfx_iOS_photos_%u", (unsigned)GetCurrentProcessId());
    CreateDirectoryW(folder, NULL);
    for (i = 0; i < 3; i++) {
        format_wide(local[i], MAX_PATH, L"%s\\Photos.sqlite%hs", folder, suffixes[i]);
        _snprintf(device_path, sizeof(device_path) - 1, "%s%s", PHOTO_DB, suffixes[i]);
        device_path[sizeof(device_path) - 1] = 0;
        if (!fetch_file(udid, device_path, local[i])) {
            DeleteFileW(local[i]);              /* the journal files are optional */
            if (i == 0) {
                if (err) copy_wide(err, err_len, tr(S_ERR_PHOTO_DB_READ));
                RemoveDirectoryW(folder);
                return FALSE;
            }
        }
    }

    /* Read it. Write access is needed so that SQLite can apply the journal to our copy. */
    wide_to_utf8(local[0], local_utf8, sizeof(local_utf8));
    if (sqlite3_open_v2(local_utf8, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK) {
        rows = read_database(db);
    } else {
        log_line("photo database: cannot open the copy: %s", db ? sqlite3_errmsg(db) : "out of memory");
    }
    if (db) sqlite3_close(db);

    /* The copy holds private data: remove it right away. */
    for (i = 0; i < 3; i++) DeleteFileW(local[i]);
    RemoveDirectoryW(folder);

    if (rows < 0) {
        g_items_count = 0;
        g_items_loaded = FALSE;
        if (err) copy_wide(err, err_len, tr(S_ERR_PHOTO_DB_LAYOUT));
        return FALSE;
    }

    qsort(g_items, g_items_count, sizeof(PhotoItem), compare_items);
    make_names_unique();
    copy_text(g_items_udid, UDID_MAX, udid);
    g_db_stamp = stamp;
    g_loaded_tick = GetTickCount();
    g_items_loaded = TRUE;
    return TRUE;
}

/* ------------------------------------------------------------- listings */

/* "2026-11" for 202611; "Unknown date" for pictures without a date. */
static void month_to_text(int month, WCHAR* dst, int dst_len)
{
    if (month == UNKNOWN_MONTH) copy_wide(dst, dst_len, L"Unknown date");
    else format_wide(dst, dst_len, L"%04d-%02d", month / 100, month % 100);
}

static int text_to_month(const char* text)
{
    int year = 0, month = 0;
    if (sscanf(text, "%4d-%2d", &year, &month) == 2 && month >= 1 && month <= 12) return year * 100 + month;
    return UNKNOWN_MONTH;
}

/* \Photos by month: one folder per month that has pictures. */
BOOL photos_list_months(const char* udid, FindList* list, WCHAR* err, int err_len)
{
    HCURSOR old_cursor = SetCursor(LoadCursor(NULL, IDC_WAIT));     /* reading the database can take a while */
    BOOL ok;
    int i;

    EnterCriticalSection(&g_photos_lock);
    ok = load_index(udid, TRUE, err, err_len);
    if (ok) {
        int previous = -1;
        for (i = 0; i < g_items_count; i++) {
            if (g_items[i].month != previous) {
                WCHAR name[32];
                previous = g_items[i].month;
                month_to_text(previous, name, 32);
                find_list_add_folder(list, name);
            }
        }
    }
    LeaveCriticalSection(&g_photos_lock);
    SetCursor(old_cursor);
    return ok;
}

/* \Photos by month\2026-11: the pictures of one month whose file is on the device. */
BOOL photos_list_month(const char* udid, const char* month_text, FindList* list, WCHAR* err, int err_len)
{
    HCURSOR old_cursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int month = text_to_month(month_text);
    PhotoItem* copy = NULL;
    int copy_count = 0;
    BOOL ok;
    int i;

    /* Take a copy of the month's items, so the lock is not held while we talk to the device. */
    EnterCriticalSection(&g_photos_lock);
    ok = load_index(udid, FALSE, err, err_len);
    if (ok) {
        for (i = 0; i < g_items_count; i++) if (g_items[i].month == month) copy_count++;
        copy = (PhotoItem*)malloc((copy_count ? copy_count : 1) * sizeof(PhotoItem));
        copy_count = 0;
        for (i = 0; copy && i < g_items_count; i++)
            if (g_items[i].month == month) copy[copy_count++] = g_items[i];
    }
    LeaveCriticalSection(&g_photos_lock);

    for (i = 0; ok && copy && i < copy_count; i++) {
        WIN32_FIND_DATAW entry;
        FileStat st;

        if (!stat_on_device(udid, copy[i].real, &st) || st.is_dir) continue;    /* not downloaded to the device */
        memset(&entry, 0, sizeof(entry));
        utf8_to_wide(copy[i].name, entry.cFileName, MAX_PATH);
        entry.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        entry.nFileSizeLow = (DWORD)st.size;
        entry.nFileSizeHigh = (DWORD)(st.size >> 32);
        entry.ftLastWriteTime = unix_ns_to_filetime(copy[i].taken_ns ? copy[i].taken_ns : st.mtime);
        entry.ftCreationTime = unix_ns_to_filetime(st.birth);
        find_list_add_entry(list, &entry);
    }
    free(copy);
    SetCursor(old_cursor);
    return ok;
}

/*
 * Name shown in a month folder -> path of the file on the device.
 * Returns FALSE when there is no such picture.
 */
BOOL photos_resolve(const char* udid, const char* month_text, const char* name, char* real, int real_len)
{
    int month = text_to_month(month_text);
    BOOL found = FALSE;
    int i;

    EnterCriticalSection(&g_photos_lock);
    if (load_index(udid, FALSE, NULL, 0)) {
        for (i = 0; i < g_items_count && !found; i++) {
            if (g_items[i].month == month && _stricmp(g_items[i].name, name) == 0) {
                copy_text(real, real_len, g_items[i].real);
                found = TRUE;
            }
        }
    }
    LeaveCriticalSection(&g_photos_lock);
    return found;
}
