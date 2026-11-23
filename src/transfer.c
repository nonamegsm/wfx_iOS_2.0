/*
 * transfer.c - copying files: device -> PC, PC -> device, device -> device.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Every transfer opens its own AFC connection, because Total Commander may
 * run transfers in a background thread while the user keeps browsing.
 */
#include "wfx_ios.h"

/* Reports progress to Total Commander. Returns nonzero when the user pressed Cancel. */
static int report_progress(const WCHAR* source, const WCHAR* target, uint64_t done, uint64_t total)
{
    int percent;

    if (!g_progress_proc) return 0;
    percent = total ? (int)(done * 100 / total) : 100;
    if (percent > 100) percent = 100;
    return g_progress_proc(g_plugin_nr, (WCHAR*)source, (WCHAR*)target, percent);
}

/* Writes a whole block to an open device file. */
static BOOL write_block(afc_client_t afc, uint64_t handle, const char* data, uint32_t length)
{
    uint32_t offset = 0;

    while (offset < length) {
        uint32_t written = 0;
        if (afc_file_write(afc, handle, data + offset, length - offset, &written) != AFC_E_SUCCESS) return FALSE;
        if (written == 0) return FALSE;
        offset += written;
    }
    return TRUE;
}

/*
 * "Processed photos": when a picture was edited on the device, the edited
 * rendition is stored separately. For
 *     /DCIM/100APPLE/IMG_0001.JPG
 * it is
 *     /PhotoData/Mutations/DCIM/100APPLE/IMG_0001/Adjustments/FullSizeRender.jpg
 * Returns TRUE and fills 'rendition' and 'st' when such a file exists.
 */
static BOOL find_edited_rendition(afc_client_t afc, const char* path, char* rendition, int rendition_len, FileStat* st)
{
    const char* folder_end;
    const char* dot;
    char extension[8];
    char without_extension[AFC_PATH_MAX];
    int i;

    /* Expected shape: /DCIM/<folder>/<name>.<ext>, nothing deeper. */
    if (strncmp(path, "/DCIM/", 6) != 0) return FALSE;
    folder_end = strchr(path + 6, '/');
    if (!folder_end || strchr(folder_end + 1, '/')) return FALSE;
    dot = strrchr(path, '.');
    if (!dot || dot < folder_end || strlen(dot + 1) >= sizeof(extension)) return FALSE;

    for (i = 0; dot[1 + i]; i++) extension[i] = (char)tolower((unsigned char)dot[1 + i]);
    extension[i] = 0;
    if (strcmp(extension, "jpeg") == 0) strcpy(extension, "jpg");
    if (strcmp(extension, "jpg") != 0 && strcmp(extension, "heic") != 0 && strcmp(extension, "png") != 0) return FALSE;

    copy_text(without_extension, sizeof(without_extension), path);
    without_extension[dot - path] = 0;
    _snprintf(rendition, rendition_len - 1, "/PhotoData/Mutations%s/Adjustments/FullSizeRender.%s",
              without_extension, extension);
    rendition[rendition_len - 1] = 0;

    return afc_stat(afc, rendition, st) == AFC_E_SUCCESS && !st->is_dir;
}

/* ------------------------------------------------------------ device -> PC */

int download_file(const WCHAR* remote_name, const WCHAR* local_name, int copy_flags, const RemotePath* rp)
{
    BOOL overwrite = (copy_flags & FS_COPYFLAGS_OVERWRITE) != 0;
    BOOL resume = (copy_flags & FS_COPYFLAGS_RESUME) != 0;
    AfcConn* conn;
    FileStat st;
    char source[AFC_PATH_MAX];
    uint64_t handle = 0;
    uint64_t done = 0;
    HANDLE out;
    char* buffer;
    afc_error_t e;
    int result = FS_FILE_OK;

    /* Total Commander asks first without flags; tell it the file is there. */
    if (!overwrite && !resume && GetFileAttributesW(local_name) != INVALID_FILE_ATTRIBUTES)
        return FS_FILE_EXISTSRESUMEALLOWED;

    conn = conn_open(rp, NULL, 0);
    if (!conn) return FS_FILE_READERROR;

    /* Decide which file to read: the edited rendition, the link target, or the file itself. */
    copy_text(source, sizeof(source), rp->path);
    if (rp->mode == M_PROCESSED && find_edited_rendition(conn->afc, rp->path, source, sizeof(source), &st)) {
        /* 'source' and 'st' now describe the edited picture */
    } else {
        copy_text(source, sizeof(source), rp->path);
        e = afc_stat(conn->afc, source, &st);
        if (e != AFC_E_SUCCESS) {
            conn_close(conn);
            return e == AFC_E_OBJECT_NOT_FOUND ? FS_FILE_NOTFOUND : FS_FILE_READERROR;
        }
        if (st.is_link && st.target[0]) {
            char parent[AFC_PATH_MAX], target[AFC_PATH_MAX];
            FileStat target_stat;
            path_parent(source, parent, sizeof(parent));
            path_join(parent, st.target, target, sizeof(target));
            if (afc_stat(conn->afc, target, &target_stat) == AFC_E_SUCCESS) {
                st = target_stat;
                copy_text(source, sizeof(source), target);
            }
        }
    }
    if (st.is_dir) { conn_close(conn); return FS_FILE_NOTSUPPORTED; }

    if (report_progress(remote_name, local_name, 0, st.size)) { conn_close(conn); return FS_FILE_USERABORT; }

    e = afc_file_open(conn->afc, source, AFC_FOPEN_RDONLY, &handle);
    if (e != AFC_E_SUCCESS) {
        conn_close(conn);
        return e == AFC_E_OBJECT_NOT_FOUND ? FS_FILE_NOTFOUND : FS_FILE_READERROR;
    }

    out = CreateFileW(local_name, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                      resume ? OPEN_ALWAYS : CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        afc_file_close(conn->afc, handle);
        conn_close(conn);
        return FS_FILE_WRITEERROR;
    }

    /* Resume: continue after the part that is already on the PC. */
    if (resume) {
        LARGE_INTEGER have;
        have.QuadPart = 0;
        GetFileSizeEx(out, &have);
        if ((uint64_t)have.QuadPart > st.size) have.QuadPart = 0;   /* local file is not a part of this one */
        done = (uint64_t)have.QuadPart;
        SetFilePointerEx(out, have, NULL, FILE_BEGIN);
        SetEndOfFile(out);
        if (done > 0 && afc_file_seek(conn->afc, handle, (int64_t)done, SEEK_SET) != AFC_E_SUCCESS)
            result = FS_FILE_READERROR;
    }

    buffer = (char*)malloc(IO_CHUNK);
    if (!buffer) result = FS_FILE_READERROR;

    while (result == FS_FILE_OK) {
        uint32_t got = 0;
        DWORD written = 0;

        if (afc_file_read(conn->afc, handle, buffer, IO_CHUNK, &got) != AFC_E_SUCCESS) { result = FS_FILE_READERROR; break; }
        if (got == 0) break;                                        /* end of file */
        if (!WriteFile(out, buffer, got, &written, NULL) || written != got) { result = FS_FILE_WRITEERROR; break; }
        done += got;
        if (report_progress(remote_name, local_name, done, st.size)) result = FS_FILE_USERABORT;
    }
    free(buffer);
    afc_file_close(conn->afc, handle);

    /* Give the local copy the time stamp of the original. */
    if (result == FS_FILE_OK && st.mtime) {
        FILETIME ft = unix_ns_to_filetime(st.mtime);
        SetFileTime(out, NULL, NULL, &ft);
    }
    CloseHandle(out);

    if (result == FS_FILE_OK) {
        if (copy_flags & FS_COPYFLAGS_MOVE) afc_remove_path(conn->afc, rp->path);
        report_progress(remote_name, local_name, 1, 1);
    } else if (!resume) {
        DeleteFileW(local_name);            /* partial data is kept only when resuming */
    }
    conn_close(conn);
    return result;
}

/* ------------------------------------------------------------ PC -> device */

int upload_file(const WCHAR* local_name, const WCHAR* remote_name, int copy_flags, const RemotePath* rp)
{
    BOOL overwrite = (copy_flags & FS_COPYFLAGS_OVERWRITE) != 0;
    BOOL resume = (copy_flags & FS_COPYFLAGS_RESUME) != 0;
    AfcConn* conn;
    FileStat st;
    BOOL exists, append, have_time;
    LARGE_INTEGER total;
    FILETIME local_time;
    uint64_t handle = 0;
    uint64_t done = 0;
    HANDLE in;
    char* buffer;
    int result = FS_FILE_OK;

    conn = conn_open(rp, NULL, 0);
    if (!conn) return FS_FILE_WRITEERROR;

    exists = afc_stat(conn->afc, rp->path, &st) == AFC_E_SUCCESS;
    if (exists && st.is_dir) { conn_close(conn); return FS_FILE_WRITEERROR; }
    if (exists && !overwrite && !resume) { conn_close(conn); return FS_FILE_EXISTSRESUMEALLOWED; }

    in = CreateFileW(local_name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                     OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (in == INVALID_HANDLE_VALUE) { conn_close(conn); return FS_FILE_NOTFOUND; }
    total.QuadPart = 0;
    GetFileSizeEx(in, &total);

    if (report_progress(local_name, remote_name, 0, (uint64_t)total.QuadPart)) {
        CloseHandle(in);
        conn_close(conn);
        return FS_FILE_USERABORT;
    }

    /* Resume: continue after the part that is already on the device. */
    append = resume && exists && st.size <= (uint64_t)total.QuadPart;
    if (afc_file_open(conn->afc, rp->path, append ? AFC_FOPEN_RW : AFC_FOPEN_WRONLY, &handle) != AFC_E_SUCCESS) {
        CloseHandle(in);
        conn_close(conn);
        return FS_FILE_WRITEERROR;
    }
    if (append && st.size > 0) {
        LARGE_INTEGER offset;
        offset.QuadPart = (LONGLONG)st.size;
        done = st.size;
        if (!SetFilePointerEx(in, offset, NULL, FILE_BEGIN) ||
            afc_file_seek(conn->afc, handle, (int64_t)done, SEEK_SET) != AFC_E_SUCCESS)
            result = FS_FILE_WRITEERROR;
    }

    buffer = (char*)malloc(IO_CHUNK);
    if (!buffer) result = FS_FILE_READERROR;

    while (result == FS_FILE_OK) {
        DWORD got = 0;

        if (!ReadFile(in, buffer, IO_CHUNK, &got, NULL)) { result = FS_FILE_READERROR; break; }
        if (got == 0) break;                                        /* end of file */
        if (!write_block(conn->afc, handle, buffer, got)) { result = FS_FILE_WRITEERROR; break; }
        done += got;
        if (report_progress(local_name, remote_name, done, (uint64_t)total.QuadPart)) result = FS_FILE_USERABORT;
    }
    free(buffer);
    afc_file_close(conn->afc, handle);

    have_time = GetFileTime(in, NULL, NULL, &local_time) != 0;
    CloseHandle(in);

    if (result == FS_FILE_OK) {
        if (have_time) afc_set_file_time(conn->afc, rp->path, filetime_to_unix_ns(&local_time));
        if (copy_flags & FS_COPYFLAGS_MOVE) DeleteFileW(local_name);
        report_progress(local_name, remote_name, 1, 1);
    } else if (!append) {
        afc_remove_path(conn->afc, rp->path);       /* do not leave half a file behind */
    }
    conn_close(conn);
    return result;
}

/* -------------------------------------------------------- device -> device */

/* Copies one file on the device; source and target may be in different containers. */
int copy_on_device(const WCHAR* old_name, const WCHAR* new_name, const RemotePath* src, const RemotePath* dst)
{
    char src_key[KEY_MAX], dst_key[KEY_MAX];
    AfcConn* from;
    AfcConn* to;
    FileStat st;
    uint64_t in = 0, out = 0;
    uint64_t done = 0;
    char* buffer;
    afc_error_t e;
    int result = FS_FILE_OK;

    connection_key(src, src_key, sizeof(src_key));
    connection_key(dst, dst_key, sizeof(dst_key));

    from = conn_open(src, NULL, 0);
    if (!from) return FS_FILE_READERROR;
    if (strcmp(src_key, dst_key) == 0) {
        to = from;                          /* same container: one connection is enough */
    } else {
        to = conn_open(dst, NULL, 0);
        if (!to) { conn_close(from); return FS_FILE_WRITEERROR; }
    }

    e = afc_stat(from->afc, src->path, &st);
    if (e != AFC_E_SUCCESS)  result = e == AFC_E_OBJECT_NOT_FOUND ? FS_FILE_NOTFOUND : FS_FILE_READERROR;
    else if (st.is_dir)      result = FS_FILE_NOTSUPPORTED;
    else if (afc_file_open(from->afc, src->path, AFC_FOPEN_RDONLY, &in) != AFC_E_SUCCESS) result = FS_FILE_READERROR;
    else if (afc_file_open(to->afc, dst->path, AFC_FOPEN_WRONLY, &out) != AFC_E_SUCCESS) {
        afc_file_close(from->afc, in);
        result = FS_FILE_WRITEERROR;
    }

    if (result == FS_FILE_OK) {
        buffer = (char*)malloc(IO_CHUNK);
        if (!buffer) result = FS_FILE_READERROR;

        while (result == FS_FILE_OK) {
            uint32_t got = 0;
            if (afc_file_read(from->afc, in, buffer, IO_CHUNK, &got) != AFC_E_SUCCESS) { result = FS_FILE_READERROR; break; }
            if (got == 0) break;
            if (!write_block(to->afc, out, buffer, got)) { result = FS_FILE_WRITEERROR; break; }
            done += got;
            if (report_progress(old_name, new_name, done, st.size)) result = FS_FILE_USERABORT;
        }
        free(buffer);
        afc_file_close(from->afc, in);
        afc_file_close(to->afc, out);

        if (result == FS_FILE_OK) {
            if (st.mtime) afc_set_file_time(to->afc, dst->path, st.mtime);
        } else {
            afc_remove_path(to->afc, dst->path);
        }
    }

    if (to != from) conn_close(to);
    conn_close(from);
    return result;
}
