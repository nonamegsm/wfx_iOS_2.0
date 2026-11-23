/*
 * icons.c - icons for Total Commander's file list:
 *           Windows system icons for the top-level folders, and the real
 *           home-screen icons for applications.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#define COBJMACROS                  /* C-style macros for COM calls (WIC) */
#include "wfx_ios.h"
#include <objbase.h>
#include <wincodec.h>

/* Windows stock icon numbers (SHSTOCKICONID) used for our folders. */
#define ICON_APPLICATION   2
#define ICON_DRIVE         8
#define ICON_WORLD         13
#define ICON_IMAGE_FILES   72
#define ICON_CELLPHONE     97
#define ICON_CAMERA        98
#define ICON_SETTINGS      106

/* PNG data of one application icon, as delivered by the device. */
typedef struct {
    char           bundle[APP_ID_MAX];
    unsigned char* png;             /* NULL when the device has no icon for it */
    uint32_t       size;
} IconEntry;

static CRITICAL_SECTION    g_icon_lock;         /* protects everything below */
static sbservices_client_t g_springboard = NULL;/* open connection to SpringBoard */
static Device*             g_icon_device = NULL;/* device the cache belongs to */
static IconEntry*          g_icons = NULL;
static int                 g_icons_count = 0;
static int                 g_icons_capacity = 0;

void icons_init(void)
{
    InitializeCriticalSection(&g_icon_lock);
}

/* ------------------------------------------------------------ stock icons */

static HICON load_stock_icon(int stock_id, BOOL small_icon)
{
    SHSTOCKICONINFO info;
    UINT flags = SHGSI_ICON | (small_icon ? SHGSI_SMALLICON : SHGSI_LARGEICON);

    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (FAILED(SHGetStockIconInfo((SHSTOCKICONID)stock_id, flags, &info))) return NULL;
    return info.hIcon;
}

/* ------------------------------------------------------ application icons */

/* Caller holds g_icon_lock. Returns the cache entry or NULL. */
static IconEntry* icon_cache_find(const char* bundle)
{
    int i;
    for (i = 0; i < g_icons_count; i++)
        if (strcmp(g_icons[i].bundle, bundle) == 0) return &g_icons[i];
    return NULL;
}

/* Caller holds g_icon_lock. Forgets all icons and closes the SpringBoard connection. */
static void icon_cache_reset(void)
{
    int i;
    for (i = 0; i < g_icons_count; i++) free(g_icons[i].png);
    g_icons_count = 0;
    if (g_springboard) { sbservices_client_free(g_springboard); g_springboard = NULL; }
    if (g_icon_device) { device_release(g_icon_device); g_icon_device = NULL; }
}

static BOOL icon_is_cached(const char* bundle)
{
    BOOL cached;
    EnterCriticalSection(&g_icon_lock);
    cached = icon_cache_find(bundle) != NULL;
    LeaveCriticalSection(&g_icon_lock);
    return cached;
}

/*
 * Gets the PNG icon of an application from SpringBoard (cached per device).
 * On success *png points to a copy the caller must free().
 */
static BOOL get_application_png(const char* bundle, unsigned char** png, uint32_t* size)
{
    Device* device = device_acquire(FALSE, NULL, 0);
    IconEntry* entry;
    BOOL ok = FALSE;

    *png = NULL;
    *size = 0;
    EnterCriticalSection(&g_icon_lock);

    /* Another device (or none): the cached icons are no longer valid. */
    if (device != g_icon_device) {
        icon_cache_reset();
        g_icon_device = device;             /* the cache keeps our reference */
        device = NULL;
    }
    if (!g_icon_device) goto done;

    entry = icon_cache_find(bundle);
    if (!entry) {
        char* data = NULL;
        uint64_t data_size = 0;

        if (!g_springboard) {
            lockdownd_service_descriptor_t service = NULL;
            if (!start_service(g_icon_device, SBSERVICES_SERVICE_NAME, &service, NULL, 0)) goto done;
            if (sbservices_client_new(g_icon_device->dev, service, &g_springboard) != SBSERVICES_E_SUCCESS)
                g_springboard = NULL;
            lockdownd_service_descriptor_free(service);
            if (!g_springboard) goto done;
        }
        if (sbservices_get_icon_pngdata(g_springboard, bundle, &data, &data_size) != SBSERVICES_E_SUCCESS) {
            /* The connection is unusable after an error; reconnect next time. */
            sbservices_client_free(g_springboard);
            g_springboard = NULL;
            data_size = 0;
        }

        /* Remember the answer, also "no icon", so we do not ask again. */
        if (g_icons_count == g_icons_capacity) {
            int capacity = g_icons_capacity ? g_icons_capacity * 2 : 64;
            IconEntry* grown = (IconEntry*)realloc(g_icons, capacity * sizeof(IconEntry));
            if (!grown) { free(data); goto done; }
            g_icons = grown;
            g_icons_capacity = capacity;
        }
        entry = &g_icons[g_icons_count++];
        copy_text(entry->bundle, APP_ID_MAX, bundle);
        entry->png = NULL;
        entry->size = 0;
        if (data && data_size > 0) {
            entry->png = (unsigned char*)malloc((size_t)data_size);
            if (entry->png) {
                memcpy(entry->png, data, (size_t)data_size);
                entry->size = (uint32_t)data_size;
            }
        }
        free(data);
    }

    if (entry->png) {
        *png = (unsigned char*)malloc(entry->size);
        if (*png) {
            memcpy(*png, entry->png, entry->size);
            *size = entry->size;
            ok = TRUE;
        }
    }

done:
    LeaveCriticalSection(&g_icon_lock);
    if (device) device_release(device);
    return ok;
}

/*
 * PNG data -> icon of size x size pixels. Uses the Windows Imaging Component:
 * decode, scale with a smooth filter, convert to 32-bit BGRA, build the icon.
 */
static HICON png_to_icon(const unsigned char* png, uint32_t png_size, int size)
{
    IWICImagingFactory* factory = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICBitmapScaler* scaler = NULL;
    IWICFormatConverter* converter = NULL;
    IStream* stream = NULL;
    HICON icon = NULL;
    HBITMAP color = NULL, mask = NULL;
    void* bits = NULL;
    BITMAPINFO bmi;
    ICONINFO info;
    HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* this may be a plain worker thread */

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void**)&factory))) goto done;
    stream = SHCreateMemStream(png, png_size);
    if (!stream) goto done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromStream(factory, stream, NULL,
                                                          WICDecodeMetadataCacheOnLoad, &decoder))) goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame))) goto done;

    if (FAILED(IWICImagingFactory_CreateBitmapScaler(factory, &scaler))) goto done;
    if (FAILED(IWICBitmapScaler_Initialize(scaler, (IWICBitmapSource*)frame, size, size,
                                           WICBitmapInterpolationModeFant))) goto done;

    if (FAILED(IWICImagingFactory_CreateFormatConverter(factory, &converter))) goto done;
    if (FAILED(IWICFormatConverter_Initialize(converter, (IWICBitmapSource*)scaler,
                                              &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                              NULL, 0.0, WICBitmapPaletteTypeCustom))) goto done;

    /* A top-down 32-bit bitmap that receives the pixels. */
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    color = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!color || !bits) goto done;
    if (FAILED(IWICFormatConverter_CopyPixels(converter, NULL, size * 4, size * size * 4, (BYTE*)bits))) goto done;

    /* The mask is not used for 32-bit icons, but CreateIconIndirect wants one. */
    mask = CreateBitmap(size, size, 1, 1, NULL);
    memset(&info, 0, sizeof(info));
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    icon = CreateIconIndirect(&info);

done:
    if (mask) DeleteObject(mask);
    if (color) DeleteObject(color);
    if (converter) IWICFormatConverter_Release(converter);
    if (scaler) IWICBitmapScaler_Release(scaler);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (stream) IStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    if (SUCCEEDED(com)) CoUninitialize();
    return icon;
}

/* ------------------------------------------------------------ entry point */

/*
 * Called by FsExtractCustomIconW. 'remote_name' is the item; on success we
 * replace it with a short key so that Total Commander caches each icon once.
 */
int extract_icon(WCHAR* remote_name, int extract_flags, HICON* icon)
{
    BOOL small_icon = (extract_flags & FS_ICONFLAG_SMALL) != 0;
    RemotePath rp;
    int stock_id = -1;
    HICON handle;

    parse_path(remote_name, &rp);

    switch (rp.mode) {
    case M_ROOT:      if (strcmp(rp.path, "/") == 0)        stock_id = ICON_DRIVE;       break;
    case M_USER:      if (strcmp(rp.path, "/") == 0)        stock_id = ICON_CELLPHONE;   break;
    case M_PHOTOS:    if (strcmp(rp.path, "/DCIM") == 0)    stock_id = ICON_CAMERA;      break;
    case M_PROCESSED: if (strcmp(rp.path, "/DCIM") == 0)    stock_id = ICON_IMAGE_FILES; break;
    case M_ICLOUD:    if (strcmp(rp.path, ICLOUD_DIR) == 0) stock_id = ICON_WORLD;       break;
    case M_APPLIST:   stock_id = ICON_APPLICATION; break;
    case M_SETTINGS:  stock_id = ICON_SETTINGS;    break;
    case M_APP:
        if (strcmp(rp.path, "/") == 0) {
            unsigned char* png = NULL;
            uint32_t png_size = 0;

            /* Asking the device takes time: let Total Commander call us again
               from its background thread. */
            if (!icon_is_cached(rp.app) && !(extract_flags & FS_ICONFLAG_BACKGROUND)) return FS_ICON_DELAYED;

            if (get_application_png(rp.app, &png, &png_size)) {
                handle = png_to_icon(png, png_size, GetSystemMetrics(small_icon ? SM_CXSMICON : SM_CXICON));
                free(png);
                if (handle) {
                    WCHAR id[APP_ID_MAX];
                    utf8_to_wide(rp.app, id, APP_ID_MAX);
                    format_wide(remote_name, MAX_PATH, L"wfx_iOS:app:%s", id);
                    *icon = handle;
                    return FS_ICON_EXTRACTED_DESTROY;
                }
            }
            stock_id = ICON_APPLICATION;    /* no icon from the device */
        }
        break;
    default:
        break;
    }

    if (stock_id < 0) return FS_ICON_USEDEFAULT;
    handle = load_stock_icon(stock_id, small_icon);
    if (!handle) return FS_ICON_USEDEFAULT;
    format_wide(remote_name, MAX_PATH, L"wfx_iOS:stock:%d", stock_id);
    *icon = handle;
    return FS_ICON_EXTRACTED_DESTROY;
}
