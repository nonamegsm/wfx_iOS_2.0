/*
 * arrange.c - the "Arrange home screen" window.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * On the left a tree of the places icons can be: the dock, the pages and the
 * folders on them. On the right the icons of the place selected in the tree,
 * in their order on the screen. Icons are dragged onto a place in the tree to
 * move them there, or inside the list to change their order.
 *
 * The window works on its own copy of the layout (see home.c for its
 * structure). The device is changed only by "Apply to device", in one step,
 * after the present layout was saved to a file.
 *
 * A "container" below is a place that holds icons: a page or the dock (a
 * property list array) or a folder (a dictionary with several arrays). The
 * icons of a container are numbered 0, 1, 2, ... through all its arrays.
 */
#include "wfx_ios.h"
#include <commctrl.h>
#include <commdlg.h>

#define ITEMS_MAX           1024    /* icons in one container */
#define ICON_SLOTS_MAX      2048    /* different applications with a loaded icon */
#define FOLDER_PAGE_SIZE    9       /* icons on one page inside a folder */
#define TITLE_LEN           128
#define STOCK_APPLICATION   2       /* Windows stock icons for items without their own */
#define STOCK_FOLDER        3
#define IMAGE_APPLICATION   0       /* positions of those two in the image list */
#define IMAGE_FOLDER        1
#define WM_REFRESH          (WM_APP + 1)

/* Which image of the image list belongs to an application. */
typedef struct {
    char bundle[APP_ID_MAX];
    int  image;
} IconSlot;

/* How a control follows the window when it is resized. */
typedef struct {
    int  id;
    BOOL move_x, move_y;            /* keep the distance to the right / bottom edge */
    BOOL size_x, size_y;            /* grow with the window */
    RECT start;                     /* position in the dialog template */
} Anchor;

/* Everything the window knows. */
typedef struct {
    char       udid[UDID_MAX];
    plist_t    layout;              /* our copy: [ dock, page 1, page 2, ... ] */
    plist_t    current;             /* container shown in the list */
    BOOL       changed;             /* differs from the device */
    BOOL       filling;             /* we are rebuilding the tree: ignore its messages */
    BOOL       dragging;
    HIMAGELIST images;
    int        icon_size;
    IconSlot*  slots;
    int        slot_count;
    SIZE       start_size;          /* client size of the dialog template */
    Anchor     anchors[10];
} Arrange;

static HomeItem g_items[ITEMS_MAX];         /* scratch list; the window is modal, one at a time */

/* ----------------------------------------------------------- containers */

/* Rebuilds the pages of a folder so that each holds at most FOLDER_PAGE_SIZE icons. */
static void folder_normalize(plist_t folder)
{
    int count = home_collect(folder, g_items, ITEMS_MAX);
    plist_t lists = plist_new_array();
    plist_t page = NULL;
    int i;

    for (i = 0; i < count; i++) {
        if (i % FOLDER_PAGE_SIZE == 0) {
            page = plist_new_array();
            plist_array_append_item(lists, page);
        }
        plist_array_append_item(page, plist_copy(g_items[i].node));
    }
    if (count == 0) plist_array_append_item(lists, plist_new_array());
    plist_dict_set_item(folder, "iconLists", lists);        /* frees the old lists */
}

/* Takes icon number 'position' out of a container. The caller owns the result. */
static plist_t take_item(plist_t container, int position)
{
    int count = home_collect(container, g_items, ITEMS_MAX);
    plist_t taken;

    if (position < 0 || position >= count) return NULL;
    taken = plist_copy(g_items[position].node);
    plist_array_remove_item(g_items[position].list, g_items[position].index);
    return taken;
}

/* Puts an icon into a container before icon number 'position'; -1 puts it at the end. */
static void put_item(plist_t container, int position, plist_t item)
{
    int count = home_collect(container, g_items, ITEMS_MAX);

    if (plist_get_node_type(container) == PLIST_ARRAY) {
        if (position < 0 || position >= count) plist_array_append_item(container, item);
        else                                   plist_array_insert_item(container, item, (uint32_t)position);
        return;
    }
    /* A folder: into the array that holds the icon at that position, then even the pages out. */
    if (position < 0 || position >= count) {
        plist_t lists = plist_dict_get_item(container, "iconLists");
        uint32_t pages = plist_array_get_size(lists);
        if (pages == 0) { plist_array_append_item(lists, plist_new_array()); pages = 1; }
        plist_array_append_item(plist_array_get_item(lists, pages - 1), item);
    } else {
        plist_array_insert_item(g_items[position].list, item, g_items[position].index);
    }
    folder_normalize(container);
}

/* Finds the page a folder is on, and the folder's position there. NULL when it is gone. */
static plist_t page_of_folder(Arrange* a, plist_t folder, int* position)
{
    uint32_t p;
    int count, i;

    for (p = 0; p < plist_array_get_size(a->layout); p++) {
        plist_t page = plist_array_get_item(a->layout, p);
        count = home_collect(page, g_items, ITEMS_MAX);
        for (i = 0; i < count; i++)
            if (g_items[i].node == folder) { if (position) *position = i; return page; }
    }
    return NULL;
}

/* TRUE when 'container' is (still) a page, the dock or a folder of the layout. */
static BOOL container_exists(Arrange* a, plist_t container)
{
    uint32_t p;
    if (!container) return FALSE;
    for (p = 0; p < plist_array_get_size(a->layout); p++)
        if (plist_array_get_item(a->layout, p) == container) return TRUE;
    return page_of_folder(a, container, NULL) != NULL;
}

static void item_title(plist_t item, WCHAR* dst, int dst_len)
{
    char name[TITLE_LEN * 3];
    home_item_name(item, name, sizeof(name));
    utf8_to_wide(name, dst, dst_len);
}

/* ---------------------------------------------------------------- icons */

/* The image for an icon of the list; application icons are fetched from the device once. */
static int image_of(Arrange* a, plist_t item)
{
    char bundle[APP_ID_MAX];
    HICON icon;
    int i;

    if (home_is_folder(item)) return IMAGE_FOLDER;
    if (!home_item_text(item, "bundleIdentifier", bundle, sizeof(bundle)) &&
        !home_item_text(item, "displayIdentifier", bundle, sizeof(bundle)))
        return IMAGE_APPLICATION;

    for (i = 0; i < a->slot_count; i++)
        if (strcmp(a->slots[i].bundle, bundle) == 0) return a->slots[i].image;
    if (a->slot_count == ICON_SLOTS_MAX) return IMAGE_APPLICATION;

    /* Remember the answer, also "no icon", so that we ask the device only once. */
    copy_text(a->slots[a->slot_count].bundle, APP_ID_MAX, bundle);
    a->slots[a->slot_count].image = IMAGE_APPLICATION;
    icon = icons_application(a->udid, bundle, a->icon_size);
    if (icon) {
        int image = ImageList_AddIcon(a->images, icon);
        if (image >= 0) a->slots[a->slot_count].image = image;
        DestroyIcon(icon);
    }
    return a->slots[a->slot_count++].image;
}

static void images_create(HWND dialog, Arrange* a)
{
    static const int stock[] = { STOCK_APPLICATION, STOCK_FOLDER };
    int i;

    a->icon_size = GetSystemMetrics(SM_CXICON) * 3 / 2;
    a->images = ImageList_Create(a->icon_size, a->icon_size, ILC_COLOR32 | ILC_MASK, 64, 64);
    for (i = 0; i < 2; i++) {
        HICON icon = icons_stock(stock[i], FALSE);
        if (!icon) icon = CopyIcon(LoadIcon(NULL, IDI_APPLICATION));
        ImageList_AddIcon(a->images, icon);
        DestroyIcon(icon);
    }
    ListView_SetImageList(GetDlgItem(dialog, IDC_A_LIST), a->images, LVSIL_NORMAL);
}

/* --------------------------------------------------- filling the window */

static HTREEITEM tree_add(HWND tree, HTREEITEM parent, const WCHAR* text, plist_t container)
{
    TVINSERTSTRUCTW insert;

    memset(&insert, 0, sizeof(insert));
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = (WCHAR*)text;
    insert.item.lParam = (LPARAM)container;
    return (HTREEITEM)SendMessageW(tree, TVM_INSERTITEMW, 0, (LPARAM)&insert);
}

static plist_t tree_container(HWND tree, HTREEITEM node)
{
    TVITEMW item;

    if (!node) return NULL;
    memset(&item, 0, sizeof(item));
    item.mask = TVIF_PARAM;
    item.hItem = node;
    if (!SendMessageW(tree, TVM_GETITEMW, 0, (LPARAM)&item)) return NULL;
    return (plist_t)item.lParam;
}

/* Finds the tree node of a container. */
static HTREEITEM tree_find(HWND tree, plist_t container)
{
    HTREEITEM page, folder;

    for (page = TreeView_GetRoot(tree); page; page = TreeView_GetNextSibling(tree, page)) {
        if (tree_container(tree, page) == container) return page;
        for (folder = TreeView_GetChild(tree, page); folder; folder = TreeView_GetNextSibling(tree, folder))
            if (tree_container(tree, folder) == container) return folder;
    }
    return NULL;
}

/* Rebuilds the tree: the dock, the pages, and below each its folders. */
static void fill_tree(HWND dialog, Arrange* a)
{
    HWND tree = GetDlgItem(dialog, IDC_A_TREE);
    WCHAR title[TITLE_LEN];
    uint32_t p;
    int count, i;

    a->filling = TRUE;
    SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree);
    for (p = 0; p < plist_array_get_size(a->layout); p++) {
        plist_t page = plist_array_get_item(a->layout, p);
        HTREEITEM node;

        if (p == 0) copy_wide(title, TITLE_LEN, tr(S_A_DOCK));
        else        format_wide(title, TITLE_LEN, tr(S_A_PAGE), (int)p);
        node = tree_add(tree, TVI_ROOT, title, page);

        count = home_collect(page, g_items, ITEMS_MAX);
        for (i = 0; i < count; i++) {
            if (!home_is_folder(g_items[i].node)) continue;
            item_title(g_items[i].node, title, TITLE_LEN);
            tree_add(tree, node, title, g_items[i].node);
        }
        TreeView_Expand(tree, node, TVE_EXPAND);
    }
    TreeView_SelectItem(tree, tree_find(tree, a->current));
    SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
    a->filling = FALSE;
}

/* Shows the icons of the current container. */
static void fill_list(HWND dialog, Arrange* a)
{
    HWND list = GetDlgItem(dialog, IDC_A_LIST);
    HCURSOR old_cursor = SetCursor(LoadCursor(NULL, IDC_WAIT));     /* icons may come from the device */
    plist_t nodes[ITEMS_MAX];
    WCHAR title[TITLE_LEN];
    int count, i;

    /* image_of() uses the scratch list, so keep our own copy of the nodes. */
    count = home_collect(a->current, g_items, ITEMS_MAX);
    for (i = 0; i < count; i++) nodes[i] = g_items[i].node;

    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    for (i = 0; i < count; i++) {
        LVITEMW item;
        memset(&item, 0, sizeof(item));
        item_title(nodes[i], title, TITLE_LEN);
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = i;
        item.pszText = title;
        item.iImage = image_of(a, nodes[i]);
        SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, NULL, TRUE);
    SetCursor(old_cursor);
}

/* Brings the whole window in line with the layout. */
static void refresh(HWND dialog, Arrange* a)
{
    WCHAR title[TITLE_LEN * 2];
    BOOL in_folder;

    /* The container we showed may be gone; fall back to the first page. */
    if (!container_exists(a, a->current))
        a->current = plist_array_get_item(a->layout, plist_array_get_size(a->layout) > 1 ? 1 : 0);

    fill_tree(dialog, a);
    fill_list(dialog, a);

    if (a->changed) format_wide(title, TITLE_LEN * 2, tr(S_A_CHANGED), tr(S_DLG_ARRANGE));
    else            copy_wide(title, TITLE_LEN * 2, tr(S_DLG_ARRANGE));
    SetWindowTextW(dialog, title);

    in_folder = home_is_folder(a->current);
    EnableWindow(GetDlgItem(dialog, IDC_A_NEWFOLDER), !in_folder);
    EnableWindow(GetDlgItem(dialog, IDC_A_APPLY), a->changed);
}

/* ------------------------------------------------------- moving icons */

/* The numbers of the icons selected in the list, ascending. */
static int selected_items(HWND dialog, int* selected)
{
    HWND list = GetDlgItem(dialog, IDC_A_LIST);
    int count = 0, i = -1;

    while (count < ITEMS_MAX && (i = ListView_GetNextItem(list, i, LVNI_SELECTED)) >= 0)
        selected[count++] = i;
    return count;
}

/*
 * Moves the selected icons of the current container into 'target', before
 * its icon number 'position' (-1: at the end). 'target' may be the current
 * container itself: that changes the order.
 */
static void move_selected(HWND dialog, Arrange* a, plist_t target, int position)
{
    int selected[ITEMS_MAX];
    plist_t taken[ITEMS_MAX];
    int count = selected_items(dialog, selected);
    int taken_count = 0, i;
    BOOL into_folder = home_is_folder(target);

    if (count == 0 || !target) return;

    /* Not onto itself, and no folders into folders. */
    home_collect(a->current, g_items, ITEMS_MAX);
    for (i = 0; i < count; i++)
        if (g_items[selected[i]].node == target) return;
    if (into_folder) {
        int kept = 0;
        for (i = 0; i < count; i++)
            if (!home_is_folder(g_items[selected[i]].node)) selected[kept++] = selected[i];
        count = kept;
        if (count == 0) return;
    }

    /* Within one container the icons before the target position move it forward. */
    if (target == a->current && position >= 0)
        for (i = count - 1; i >= 0; i--)
            if (selected[i] < position) position--;

    /* Take from the back, so that the numbers of the others stay valid. */
    for (i = count - 1; i >= 0; i--) {
        plist_t item = take_item(a->current, selected[i]);
        if (item) taken[taken_count++] = item;
    }
    if (home_is_folder(a->current)) folder_normalize(a->current);

    /* 'taken' is in reverse order. */
    for (i = taken_count - 1; i >= 0; i--) {
        put_item(target, position, taken[i]);
        if (position >= 0) position++;
    }
    a->changed = TRUE;
    refresh(dialog, a);
}

/* What is under the mouse: a place in the tree, or a position in the list. */
static BOOL drop_target(HWND dialog, Arrange* a, plist_t* target, int* position)
{
    HWND tree = GetDlgItem(dialog, IDC_A_TREE);
    HWND list = GetDlgItem(dialog, IDC_A_LIST);
    POINT screen, point;
    RECT area;

    GetCursorPos(&screen);
    GetWindowRect(tree, &area);
    if (PtInRect(&area, screen)) {
        TVHITTESTINFO hit;
        point = screen;
        ScreenToClient(tree, &point);
        memset(&hit, 0, sizeof(hit));
        hit.pt = point;
        TreeView_HitTest(tree, &hit);
        if (!hit.hItem) return FALSE;
        TreeView_SelectDropTarget(tree, hit.hItem);
        *target = tree_container(tree, hit.hItem);
        *position = -1;
        return *target != a->current;
    }
    TreeView_SelectDropTarget(tree, NULL);

    GetWindowRect(list, &area);
    if (PtInRect(&area, screen)) {
        LVHITTESTINFO hit;
        point = screen;
        ScreenToClient(list, &point);
        memset(&hit, 0, sizeof(hit));
        hit.pt = point;
        *target = a->current;
        *position = ListView_HitTest(list, &hit);       /* -1 on empty space: to the end */
        return TRUE;
    }
    return FALSE;
}

static void drag_end(HWND dialog, Arrange* a, BOOL drop)
{
    plist_t target = NULL;
    int position = -1;
    BOOL valid;

    if (!a->dragging) return;
    a->dragging = FALSE;
    valid = drop && drop_target(dialog, a, &target, &position);
    TreeView_SelectDropTarget(GetDlgItem(dialog, IDC_A_TREE), NULL);
    ReleaseCapture();
    if (valid) move_selected(dialog, a, target, position);
}

/* -------------------------------------------------------------- folders */

/* The folder a command applies to: the one shown, or the single one selected in the list. */
static plist_t chosen_folder(HWND dialog, Arrange* a)
{
    int selected[ITEMS_MAX];

    if (home_is_folder(a->current)) return a->current;
    if (selected_items(dialog, selected) == 1) {
        home_collect(a->current, g_items, ITEMS_MAX);
        if (home_is_folder(g_items[selected[0]].node)) return g_items[selected[0]].node;
    }
    return NULL;
}

static void start_rename(HWND dialog, plist_t folder)
{
    HWND tree = GetDlgItem(dialog, IDC_A_TREE);
    HTREEITEM node = tree_find(tree, folder);

    if (!node) return;
    SetFocus(tree);
    TreeView_EditLabel(tree, node);
}

/* Makes a folder from the selected icons, at the place of the first one, and lets the user name it. */
static void new_folder(HWND dialog, Arrange* a)
{
    int selected[ITEMS_MAX];
    plist_t taken[ITEMS_MAX];
    char name[TITLE_LEN * 3];
    plist_t folder;
    int count = selected_items(dialog, selected);
    int kept = 0, taken_count = 0, i;

    /* Folders stay where they are: a folder cannot hold another one. */
    home_collect(a->current, g_items, ITEMS_MAX);
    for (i = 0; i < count; i++)
        if (!home_is_folder(g_items[selected[i]].node)) selected[kept++] = selected[i];
    count = kept;
    if (count == 0 || home_is_folder(a->current)) {
        MessageBoxW(dialog, tr(S_A_SELECT_FIRST), tr(S_DLG_ARRANGE), MB_OK | MB_ICONINFORMATION);
        return;
    }

    wide_to_utf8(tr(S_A_FOLDER_NAME), name, sizeof(name));
    folder = home_new_folder(name);
    for (i = count - 1; i >= 0; i--) {
        plist_t item = take_item(a->current, selected[i]);
        if (item) taken[taken_count++] = item;
    }
    for (i = taken_count - 1; i >= 0; i--) put_item(folder, -1, taken[i]);
    put_item(a->current, selected[0], folder);

    a->current = folder;
    a->changed = TRUE;
    refresh(dialog, a);
    start_rename(dialog, folder);
}

/* Puts the icons of a folder onto its page, where the folder was. */
static void ungroup_folder(HWND dialog, Arrange* a)
{
    plist_t folder = chosen_folder(dialog, a);
    plist_t taken[ITEMS_MAX];
    plist_t page;
    int position = 0, count, i;

    if (!folder) return;
    page = page_of_folder(a, folder, &position);
    if (!page) return;

    count = home_collect(folder, g_items, ITEMS_MAX);
    for (i = 0; i < count; i++) taken[i] = plist_copy(g_items[i].node);
    plist_free(take_item(page, position));              /* the folder itself */
    for (i = 0; i < count; i++) put_item(page, position + i, taken[i]);

    a->current = page;
    a->changed = TRUE;
    refresh(dialog, a);
}

/* ------------------------------------------------------ device and files */

/* Replaces our copy of the layout. */
static void set_layout(HWND dialog, Arrange* a, plist_t layout, BOOL changed)
{
    if (a->layout) plist_free(a->layout);
    a->layout = layout;
    a->current = NULL;
    a->changed = changed;
    refresh(dialog, a);
}

/* TRUE when there is nothing to lose, or the user agrees to lose it. */
static BOOL may_discard(HWND dialog, Arrange* a)
{
    if (!a->changed) return TRUE;
    return MessageBoxW(dialog, tr(S_A_DISCARD), tr(S_DLG_ARRANGE), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

static void read_from_device(HWND dialog, Arrange* a)
{
    WCHAR err[MSG_MAX] = L"";
    HCURSOR old_cursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
    plist_t layout = home_read_layout(a->udid, err, MSG_MAX);

    SetCursor(old_cursor);
    if (!layout) {
        MessageBoxW(dialog, err[0] ? err : tr(S_ERR_HOME_FILE), tr(S_DLG_ARRANGE), MB_OK | MB_ICONWARNING);
        return;
    }
    set_layout(dialog, a, layout, FALSE);
}

static void apply_to_device(HWND dialog, Arrange* a)
{
    HCURSOR old_cursor;
    BOOL applied;

    if (MessageBoxW(dialog, tr(S_A_APPLY_QUESTION), tr(S_DLG_ARRANGE), MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    old_cursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
    applied = home_apply_layout(a->udid, a->layout);     /* reports a refusal itself */
    SetCursor(old_cursor);
    if (!applied) return;

    /* Show what the device made of it: it may have placed some icons differently. */
    a->changed = FALSE;
    read_from_device(dialog, a);
    SetDlgItemTextW(dialog, IDC_A_HINT, tr(S_A_APPLIED));
}

/* Asks for a file name; 'save' chooses between the two standard dialogs. */
static BOOL ask_file(HWND dialog, WCHAR* path, BOOL save)
{
    OPENFILENAMEW ask;

    memset(&ask, 0, sizeof(ask));
    ask.lStructSize = sizeof(ask);
    ask.hwndOwner = dialog;
    ask.lpstrFilter = L"Layout (*.plist)\0*.plist\0";
    ask.lpstrFile = path;
    ask.nMaxFile = MAX_PATH;
    ask.lpstrDefExt = L"plist";
    ask.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST) : (OFN_FILEMUSTEXIST | OFN_HIDEREADONLY);
    return save ? GetSaveFileNameW(&ask) : GetOpenFileNameW(&ask);
}

static void open_file(HWND dialog, Arrange* a)
{
    WCHAR path[MAX_PATH] = L"";
    plist_t layout;

    if (!may_discard(dialog, a) || !ask_file(dialog, path, FALSE)) return;
    layout = home_load_layout_file(path);
    if (!layout) {
        MessageBoxW(dialog, tr(S_ERR_HOME_FILE), tr(S_DLG_ARRANGE), MB_OK | MB_ICONWARNING);
        return;
    }
    set_layout(dialog, a, layout, TRUE);
}

static void save_file(HWND dialog, Arrange* a)
{
    WCHAR path[MAX_PATH] = L"home-screen.plist";
    char* xml = NULL;
    uint32_t xml_len = 0;
    HANDLE file;
    DWORD written;
    BOOL saved = FALSE;

    if (!ask_file(dialog, path, TRUE)) return;
    plist_to_xml(a->layout, &xml, &xml_len);
    if (xml) {
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            saved = WriteFile(file, xml, xml_len, &written, NULL);
            CloseHandle(file);
        }
        plist_mem_free(xml);
    }
    if (!saved) MessageBoxW(dialog, tr(S_ERR_SAVE), tr(S_DLG_ARRANGE), MB_OK | MB_ICONWARNING);
}

/* --------------------------------------------------------- the window */

static void anchors_init(HWND dialog, Arrange* a)
{
    static const Anchor rules[] = {
        /* id               move_x move_y size_x size_y */
        { IDC_A_NEWFOLDER,  FALSE, FALSE, FALSE, FALSE, { 0, 0, 0, 0 } },
        { IDC_A_TREE,       FALSE, FALSE, FALSE, TRUE,  { 0, 0, 0, 0 } },
        { IDC_A_LIST,       FALSE, FALSE, TRUE,  TRUE,  { 0, 0, 0, 0 } },
        { IDC_A_HINT,       FALSE, TRUE,  TRUE,  FALSE, { 0, 0, 0, 0 } },
        { IDC_A_RELOAD,     FALSE, TRUE,  FALSE, FALSE, { 0, 0, 0, 0 } },
        { IDC_A_LOAD,       FALSE, TRUE,  FALSE, FALSE, { 0, 0, 0, 0 } },
        { IDC_A_SAVE,       FALSE, TRUE,  FALSE, FALSE, { 0, 0, 0, 0 } },
        { IDC_A_APPLY,      TRUE,  TRUE,  FALSE, FALSE, { 0, 0, 0, 0 } },
        { IDCANCEL,         TRUE,  TRUE,  FALSE, FALSE, { 0, 0, 0, 0 } },
        { 0,                FALSE, FALSE, FALSE, FALSE, { 0, 0, 0, 0 } }
    };
    RECT client;
    int i;

    GetClientRect(dialog, &client);
    a->start_size.cx = client.right;
    a->start_size.cy = client.bottom;
    for (i = 0; i < 10; i++) {
        a->anchors[i] = rules[i];
        if (rules[i].id == 0) break;
        GetWindowRect(GetDlgItem(dialog, rules[i].id), &a->anchors[i].start);
        MapWindowPoints(NULL, dialog, (POINT*)&a->anchors[i].start, 2);
    }
}

/* Places the controls for the present size of the window. */
static void anchors_apply(HWND dialog, Arrange* a)
{
    RECT client;
    int dx, dy, i;

    if (a->start_size.cx == 0) return;                  /* not initialised yet */
    GetClientRect(dialog, &client);
    dx = client.right - a->start_size.cx;
    dy = client.bottom - a->start_size.cy;
    for (i = 0; i < 10 && a->anchors[i].id != 0; i++) {
        const Anchor* anchor = &a->anchors[i];
        RECT r = anchor->start;
        if (anchor->move_x) { r.left += dx; r.right += dx; }
        if (anchor->move_y) { r.top += dy; r.bottom += dy; }
        if (anchor->size_x) r.right += dx;
        if (anchor->size_y) r.bottom += dy;
        MoveWindow(GetDlgItem(dialog, anchor->id), r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    }
    ListView_Arrange(GetDlgItem(dialog, IDC_A_LIST), LVA_DEFAULT);
}

static void set_texts(HWND dialog)
{
    SetDlgItemTextW(dialog, IDC_A_NEWFOLDER, tr(S_A_NEWFOLDER));
    SetDlgItemTextW(dialog, IDC_A_NEWPAGE, tr(S_A_NEWPAGE));
    SetDlgItemTextW(dialog, IDC_A_UNGROUP, tr(S_A_UNGROUP));
    SetDlgItemTextW(dialog, IDC_A_RENAME, tr(S_A_RENAME));
    SetDlgItemTextW(dialog, IDC_A_RELOAD, tr(S_A_RELOAD));
    SetDlgItemTextW(dialog, IDC_A_LOAD, tr(S_A_LOAD));
    SetDlgItemTextW(dialog, IDC_A_SAVE, tr(S_A_SAVE));
    SetDlgItemTextW(dialog, IDC_A_APPLY, tr(S_A_APPLY));
    SetDlgItemTextW(dialog, IDCANCEL, tr(S_BTN_CLOSE));
    SetDlgItemTextW(dialog, IDC_A_HINT, tr(S_A_HINT));
}

/* Messages of the tree and the list. Returns the value for the control. */
static LRESULT on_notify(HWND dialog, Arrange* a, NMHDR* header)
{
    HWND tree = GetDlgItem(dialog, IDC_A_TREE);

    if (header->idFrom == IDC_A_TREE) {
        switch (header->code) {
        case TVN_SELCHANGEDW:
            /* Another place was chosen: show its icons. */
            if (!a->filling) {
                plist_t container = (plist_t)((NMTREEVIEWW*)header)->itemNew.lParam;
                if (container && container != a->current) {
                    a->current = container;
                    fill_list(dialog, a);
                    EnableWindow(GetDlgItem(dialog, IDC_A_NEWFOLDER), !home_is_folder(container));
                }
            }
            break;
        case TVN_BEGINLABELEDITW:
            /* Only folders have a name that can be changed. TRUE refuses. */
            return !home_is_folder((plist_t)((NMTVDISPINFOW*)header)->item.lParam);
        case TVN_ENDLABELEDITW: {
            NMTVDISPINFOW* info = (NMTVDISPINFOW*)header;
            plist_t folder = (plist_t)info->item.lParam;
            char name[TITLE_LEN * 3];
            if (!info->item.pszText || info->item.pszText[0] == 0 || !home_is_folder(folder)) return FALSE;
            wide_to_utf8(info->item.pszText, name, sizeof(name));
            plist_dict_set_item(folder, "displayName", plist_new_string(name));
            a->changed = TRUE;
            PostMessageW(dialog, WM_REFRESH, 0, 0);     /* the name is also shown in the list */
            return TRUE; }
        }
    } else if (header->idFrom == IDC_A_LIST) {
        switch (header->code) {
        case LVN_BEGINDRAG:
            a->dragging = TRUE;
            SetCapture(dialog);
            break;
        case NM_DBLCLK: {
            /* Double click on a folder opens it. */
            int index = ((NMITEMACTIVATE*)header)->iItem;
            int count = home_collect(a->current, g_items, ITEMS_MAX);
            if (index >= 0 && index < count && home_is_folder(g_items[index].node)) {
                a->current = g_items[index].node;
                TreeView_SelectItem(tree, tree_find(tree, a->current));
                fill_list(dialog, a);
                EnableWindow(GetDlgItem(dialog, IDC_A_NEWFOLDER), FALSE);
            }
            break; }
        }
    }
    return 0;
}

static void on_command(HWND dialog, Arrange* a, int id)
{
    HWND tree = GetDlgItem(dialog, IDC_A_TREE);
    BOOL editing = TreeView_GetEditControl(tree) != NULL;
    plist_t folder;

    switch (id) {
    case IDC_A_NEWFOLDER: new_folder(dialog, a); break;
    case IDC_A_NEWPAGE:
        /* An empty page; it reaches the device only when icons are put on it. */
        a->current = plist_new_array();
        plist_array_append_item(a->layout, a->current);
        refresh(dialog, a);
        break;
    case IDC_A_UNGROUP:   ungroup_folder(dialog, a); break;
    case IDC_A_RENAME:
        folder = chosen_folder(dialog, a);
        if (folder) start_rename(dialog, folder);
        break;
    case IDC_A_RELOAD:    if (may_discard(dialog, a)) read_from_device(dialog, a); break;
    case IDC_A_LOAD:      open_file(dialog, a); break;
    case IDC_A_SAVE:      save_file(dialog, a); break;
    case IDC_A_APPLY:     apply_to_device(dialog, a); break;
    case IDOK:
        /* Enter while a folder name is typed: accept the name. */
        if (editing) TreeView_EndEditLabelNow(tree, FALSE);
        break;
    case IDCANCEL:
        if (editing)                    TreeView_EndEditLabelNow(tree, TRUE);
        else if (may_discard(dialog, a)) EndDialog(dialog, 0);
        break;
    }
}

static INT_PTR CALLBACK arrange_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    Arrange* a = (Arrange*)GetWindowLongPtrW(dialog, GWLP_USERDATA);

    switch (message) {
    case WM_INITDIALOG: {
        HICON icon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_PLUGIN));
        a = (Arrange*)lparam;
        SetWindowLongPtrW(dialog, GWLP_USERDATA, (LONG_PTR)a);
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        set_texts(dialog);
        images_create(dialog, a);
        anchors_init(dialog, a);
        refresh(dialog, a);
        return TRUE; }

    case WM_SIZE:
        if (a) anchors_apply(dialog, a);
        return TRUE;

    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lparam)->ptMinTrackSize.x = 760;
        ((MINMAXINFO*)lparam)->ptMinTrackSize.y = 420;
        return TRUE;

    case WM_REFRESH:
        if (a) refresh(dialog, a);
        return TRUE;

    case WM_NOTIFY:
        if (!a) break;
        SetWindowLongPtrW(dialog, DWLP_MSGRESULT, on_notify(dialog, a, (NMHDR*)lparam));
        return TRUE;

    case WM_MOUSEMOVE:
        /* While dragging: show whether the icons can be dropped here. */
        if (a && a->dragging) {
            plist_t target;
            int position;
            SetCursor(LoadCursor(NULL, drop_target(dialog, a, &target, &position) ? IDC_HAND : IDC_NO));
            return TRUE;
        }
        break;

    case WM_LBUTTONUP:
        if (a && a->dragging) { drag_end(dialog, a, TRUE); return TRUE; }
        break;

    case WM_CAPTURECHANGED:
        if (a && a->dragging) drag_end(dialog, a, FALSE);
        break;

    case WM_COMMAND:
        if (a) on_command(dialog, a, LOWORD(wparam));
        return TRUE;
    }
    return FALSE;
}

/* Opens the window for one device ('udid' empty: the default device). */
void show_arrange(HWND parent, const char* udid)
{
    INITCOMMONCONTROLSEX controls;
    WCHAR err[MSG_MAX] = L"";
    HCURSOR old_cursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
    Arrange* a = (Arrange*)calloc(1, sizeof(Arrange));

    if (!a) return;
    copy_text(a->udid, UDID_MAX, udid);
    a->slots = (IconSlot*)calloc(ICON_SLOTS_MAX, sizeof(IconSlot));
    a->layout = a->slots ? home_read_layout(udid, err, MSG_MAX) : NULL;
    SetCursor(old_cursor);

    if (!a->layout) {
        if (err[0]) MessageBoxW(parent, err, PLUGIN_TITLE, MB_OK | MB_ICONWARNING);
    } else {
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES;
        InitCommonControlsEx(&controls);                /* the tree and the list live in comctl32 */
        DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_ARRANGE), parent, arrange_proc, (LPARAM)a);
        if (a->images) ImageList_Destroy(a->images);
        plist_free(a->layout);
    }
    free(a->slots);
    free(a);
}
