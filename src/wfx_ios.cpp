// wfx_iOS 2.0 - Total Commander file-system plugin for iOS devices.
//
// An update of the wfx_iOS 1.3 plugin (2017) by Domo, the original author.
// The folder layout, feature set and behaviour are his design; this version
// re-implements them on current libimobiledevice / OpenSSL 3 so the plugin
// works with modern iOS versions. Requires Apple Mobile Device Support
// (usbmuxd) on the PC.
//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Virtual layout (same as 1.3):
//   \Root              com.apple.afc2 (jailbroken devices only; hidden if absent)
//   \User              com.apple.afc  (media partition)
//   \Photos            com.apple.afc, rooted at /DCIM
//   \Processed photos  like Photos, but downloads prefer the edited rendition
//   \Applications\<bundle id>\...   com.apple.mobile.house_arrest

#include <winsock2.h>
#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <algorithm>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/afc.h>
#include <libimobiledevice/house_arrest.h>
#include <libimobiledevice/installation_proxy.h>
#include <libimobiledevice/service.h>
#include <libimobiledevice/sbservices.h>
#include <plist/plist.h>
#include <usbmuxd.h>

#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include "fsplugin.h"
#include "resource.h"

#define PLUGIN_LABEL   "wfx_iOS"
#define PLUGIN_TITLE   L"iOS"
#define INI_SECTION    "wfx_iOS"
#define IO_CHUNK       (1024 * 1024)

// ---------------------------------------------------------------- globals

static HINSTANCE      g_hInst = nullptr;
static int            g_pluginNr = 0;
static tProgressProcW g_progressW = nullptr;
static tLogProcW      g_logW = nullptr;
static tRequestProcW  g_requestW = nullptr;
static tProgressProc  g_progressA = nullptr;
static std::string    g_ini = "wfx_iOS.ini";

struct Device {
    idevice_t dev = nullptr;
    std::string udid;
    int iosMajor = 0;
    int hasAfc2 = -1;            // -1 unknown, 0 no, 1 yes
    bool network = false;        // reached over Wi-Fi instead of USB
    ~Device() { if (dev) idevice_free(dev); }
};
typedef std::shared_ptr<Device> DevicePtr;

struct Afc {
    DevicePtr device;            // keeps idevice_t alive while the client exists
    afc_client_t afc = nullptr;
    house_arrest_client_t ha = nullptr;
    ~Afc() {
        if (afc) afc_client_free(afc);
        if (ha) house_arrest_client_free(ha);
    }
};
typedef std::shared_ptr<Afc> AfcPtr;

static std::mutex            g_devLock;   // guards g_device
static DevicePtr             g_device;
static std::recursive_mutex  g_opLock;    // serialises foreground ops + cache
static std::map<std::string, AfcPtr> g_cache;
static std::atomic<bool>     g_dropCache(false);   // device vanished: cached connections are dead
static std::atomic<bool>     g_deviceSeen(false);  // reset once the "no device" box was shown

// ---------------------------------------------------------------- helpers

static std::string to_utf8(const WCHAR* w)
{
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring to_wide(const std::string& s, UINT cp = CP_UTF8)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static FILETIME unix_ns_to_filetime(uint64_t ns)
{
    FILETIME ft;
    if (!ns) { ft.dwLowDateTime = 0xFFFFFFFE; ft.dwHighDateTime = 0xFFFFFFFF; return ft; }
    uint64_t v = ns / 100 + 116444736000000000ULL;
    ft.dwLowDateTime = (DWORD)v; ft.dwHighDateTime = (DWORD)(v >> 32);
    return ft;
}

static uint64_t filetime_to_unix_ns(const FILETIME& ft)
{
    uint64_t v = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    if (v < 116444736000000000ULL) return 0;
    return (v - 116444736000000000ULL) * 100;
}

static bool ini_bool(const char* key, bool def)
{
    return GetPrivateProfileIntA(INI_SECTION, key, def ? 1 : 0, g_ini.c_str()) != 0;
}

static std::string ini_string(const char* key)
{
    char buf[256] = "";
    GetPrivateProfileStringA(INI_SECTION, key, "", buf, sizeof(buf), g_ini.c_str());
    return buf;
}

static void show_error(const std::wstring& msg)
{
    static std::mutex m; static std::wstring last; static DWORD lastTick = 0;
    {
        std::lock_guard<std::mutex> l(m);
        DWORD now = GetTickCount();
        bool same = msg == last;
        // One box per problem: not again for the same text within 30 s, and not at all
        // while the device stays disconnected (Total Commander retries parent folders).
        bool noDevice = msg.compare(0, 13, L"No iOS device") == 0 || msg.compare(0, 12, L"Cannot reach") == 0 ||
                        msg.compare(0, 16, L"Preferred device") == 0 || msg.compare(0, 25, L"Could not open the device") == 0;
        if (same && (now - lastTick < 30000 || (noDevice && !g_deviceSeen))) return;
        if (!same && now - lastTick < 1500) { last = msg; return; }
        last = msg; lastTick = now;
        if (noDevice) g_deviceSeen = false;
    }
    if (g_logW) g_logW(g_pluginNr, MSGTYPE_IMPORTANTERROR, (WCHAR*)msg.c_str());
    MessageBoxW(GetActiveWindow(), msg.c_str(), PLUGIN_TITLE, MB_OK | MB_ICONWARNING);
}

// Line in the trace file (stderr is redirected to %TEMP%\wfx_iOS.log when Debug is on).
static bool g_trace = false;
static std::wstring g_logPath;
static void trace(const char* fmt, ...)
{
    if (!g_trace) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(stderr, "%02d:%02d:%02d.%03d wfx_iOS: ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

// ---------------------------------------------------------------- paths

enum Mode { M_INVALID, M_TOP, M_ROOT, M_USER, M_PHOTOS, M_PROCESSED, M_ICLOUD, M_APPLIST, M_APP, M_SETTINGS };

#define SETTINGS_ITEM L"Settings"

#define ICLOUD_DIR "/PhotoData/CPLAssets"

struct RPath {
    Mode mode = M_INVALID;
    std::string app;     // bundle id (M_APP)
    std::string path;    // AFC path, UTF-8, always starts with '/'
};

// Folder name shown under \Applications -> bundle id.
static std::string resolve_app(const std::wstring& shown);

static RPath parse_path(const WCHAR* remote)
{
    RPath r;
    std::wstring p = remote ? remote : L"";
    std::replace(p.begin(), p.end(), L'/', L'\\');
    while (p.size() > 1 && p.back() == L'\\') p.pop_back();
    if (p.empty() || p == L"\\") { r.mode = M_TOP; r.path = "/"; return r; }
    if (p[0] == L'\\') p.erase(0, 1);

    size_t sep = p.find(L'\\');
    std::wstring first = p.substr(0, sep);
    std::wstring rest = sep == std::wstring::npos ? L"" : p.substr(sep);   // "\a\b" or ""
    std::string prefix;

    if (first == L"Root") r.mode = M_ROOT;
    else if (first == L"User") r.mode = M_USER;
    else if (first == L"Photos") { r.mode = M_PHOTOS; prefix = "/DCIM"; }
    else if (first == L"Processed photos") { r.mode = M_PROCESSED; prefix = "/DCIM"; }
    else if (first == L"iCloud photos") { r.mode = M_ICLOUD; prefix = ICLOUD_DIR; }
    else if (first == SETTINGS_ITEM && rest.empty()) { r.mode = M_SETTINGS; r.path = "/"; return r; }
    else if (first == L"Applications") {
        if (rest.empty()) { r.mode = M_APPLIST; r.path = "/"; return r; }
        rest.erase(0, 1);
        size_t s2 = rest.find(L'\\');
        r.mode = M_APP;
        r.app = resolve_app(rest.substr(0, s2));
        rest = s2 == std::wstring::npos ? L"" : rest.substr(s2);
    } else return r;   // M_INVALID

    std::replace(rest.begin(), rest.end(), L'\\', L'/');
    r.path = prefix + to_utf8(rest.c_str());
    if (r.path.empty()) r.path = "/";
    return r;
}

static bool is_afc_mode(Mode m) { return m == M_ROOT || m == M_USER || m == M_PHOTOS || m == M_PROCESSED || m == M_ICLOUD || m == M_APP; }

static std::string cache_key(const RPath& r)
{
    switch (r.mode) {
    case M_ROOT: return "afc2";
    case M_APP:  return "app:" + r.app;
    default:     return "afc";
    }
}

static std::string parent_of(const std::string& p)
{
    size_t s = p.rfind('/');
    if (s == std::string::npos || s == 0) return "/";
    return p.substr(0, s);
}

static std::string join_path(const std::string& dir, const std::string& name)
{
    if (!name.empty() && name[0] == '/') return name;
    return (dir.empty() || dir.back() != '/') ? dir + "/" + name : dir + name;
}

// ---------------------------------------------------------------- device / services

static std::wstring lockdown_message(lockdownd_error_t e)
{
    switch (e) {
    case LOCKDOWN_E_PASSWORD_PROTECTED:
        return L"The device is locked with a passcode.\nUnlock it and try again.";
    case LOCKDOWN_E_PAIRING_DIALOG_RESPONSE_PENDING:
        return L"Tap \"Trust\" on the device, then try again.";
    case LOCKDOWN_E_USER_DENIED_PAIRING:
        return L"Pairing was declined on the device (\"Don't Trust\").\nReconnect the device and tap \"Trust\".";
    case LOCKDOWN_E_INVALID_HOST_ID:
        return L"The device is not paired with this computer.\nReconnect it and tap \"Trust\".";
    default:
        return L"Could not connect to lockdownd: " + to_wide(lockdownd_strerror(e)) +
               L" (" + std::to_wstring((int)e) + L")";
    }
}

static DevicePtr get_device(bool fresh, std::wstring* err)
{
    std::lock_guard<std::mutex> l(g_devLock);
    static bool wsa = false;
    if (!wsa) {
        WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd); wsa = true;
        // %TEMP%\\wfx_iOS.log: a short plugin log by default (Log=0 turns it off);
        // Debug=1 adds the full libimobiledevice protocol trace, which is large and slow.
        if (ini_bool("Log", true)) {
            WCHAR tmp[MAX_PATH] = L"";
            GetTempPathW(MAX_PATH, tmp);
            std::wstring log = std::wstring(tmp) + L"wfx_iOS.log";
            std::string custom = ini_string("LogFile");
            if (!custom.empty()) log = to_wide(custom, CP_ACP);
            if (_wfreopen(log.c_str(), L"w", stderr)) {
                g_logPath = log;
                setvbuf(stderr, nullptr, _IONBF, 0);
                if (ini_bool("Debug", false)) {
                    idevice_set_debug_level(1);
                    libusbmuxd_set_debug_level(1);
                }
                g_trace = true;
                OSVERSIONINFOW os; memset(&os, 0, sizeof(os)); os.dwOSVersionInfoSize = sizeof(os);
                trace("wfx_iOS 2.0 (built " __DATE__ " " __TIME__ ", %d-bit), ini %s", (int)(sizeof(void*) * 8), g_ini.c_str());
            }
        }
    }
    bool wifi = ini_bool("AllowWiFi", false);
    if (g_device && !fresh) {
        // Cheap check with the local service so a pulled cable fails at once
        // instead of waiting on a dead connection.
        static DWORD lastCheck = 0;
        DWORD now = GetTickCount();
        if (now - lastCheck < 700) return g_device;
        usbmuxd_device_info_t* list = nullptr;
        int n = usbmuxd_get_device_list(&list);
        bool present = false;
        for (int i = 0; i < n; i++)
            if (g_device->udid == list[i].udid &&
                (list[i].conn_type == CONNECTION_TYPE_NETWORK) == g_device->network) present = true;
        if (list) usbmuxd_device_list_free(&list);
        if (present) { lastCheck = now; return g_device; }
        trace("device %s is gone", g_device->udid.c_str());
        g_dropCache = true;
    }
    g_device.reset();

    std::string preferred = ini_string("PreferredDevice");
    idevice_t dev = nullptr;
    const char* want = preferred.empty() ? nullptr : preferred.c_str();
    bool network = false;
    idevice_error_t e = idevice_new_with_options(&dev, want, IDEVICE_LOOKUP_USBMUX);   // USB first
    if ((e != IDEVICE_E_SUCCESS || !dev) && wifi) {      // Wi-Fi only on request: connecting can block for long
        dev = nullptr; network = true;
        e = idevice_new_with_options(&dev, want, IDEVICE_LOOKUP_NETWORK);
    }
    if (e != IDEVICE_E_SUCCESS || !dev) {
        if (err) {
            // Ask usbmuxd directly so the message says which step failed.
            usbmuxd_device_info_t* list = nullptr;
            int n = usbmuxd_get_device_list(&list);
            if (n < 0) {
                *err = L"Cannot reach the Apple Mobile Device Service (usbmuxd, 127.0.0.1:27015), error " + std::to_wstring(n) +
                       L".\nInstall Apple Mobile Device Support and make sure the \"Apple Mobile Device Service\" is running.";
            } else if (n == 0) {
                *err = L"No iOS device is connected.\n"
                       L"Connect and unlock the device. (The Apple Mobile Device Service is running but reports no device.)";
            } else {
                std::wstring seen;
                for (int i = 0; i < n; i++) seen += L"\n  " + to_wide(list[i].udid) + (list[i].conn_type == CONNECTION_TYPE_NETWORK ? L" (network)" : L" (USB)");
                *err = (preferred.empty() ? std::wstring(L"Could not open the device") : L"Preferred device " + to_wide(preferred) + L" is not connected") +
                       L" (idevice error " + std::to_wstring((int)e) + L").\nDevices reported by the service:" + seen;
            }
            if (list) usbmuxd_device_list_free(&list);
        }
        return nullptr;
    }
    DevicePtr d = std::make_shared<Device>();
    d->dev = dev;
    d->network = network;
    char* udid = nullptr;
    if (idevice_get_udid(dev, &udid) == IDEVICE_E_SUCCESS && udid) { d->udid = udid; free(udid); }
    g_device = d;
    g_deviceSeen = true;
    trace("device %s opened over %s", d->udid.c_str(), network ? "Wi-Fi" : "USB");
    return d;
}

// Opens a lockdown session and starts one service. On success *svc must be freed by the caller.
static bool start_service(const DevicePtr& d, const char* name, lockdownd_service_descriptor_t* svc, std::wstring* err)
{
    lockdownd_client_t lc = nullptr;
    lockdownd_error_t le = lockdownd_client_new_with_handshake(d->dev, &lc, PLUGIN_LABEL);
    if (le != LOCKDOWN_E_SUCCESS || !lc) {
        trace("lockdown handshake failed: %d (%s)", (int)le, lockdownd_strerror(le));
        if (err) {
            *err = lockdown_message(le);
            if (le == LOCKDOWN_E_MUX_ERROR) {
                // Tell a refused connection apart from a failure later in the handshake.
                idevice_connection_t c = nullptr;
                idevice_error_t ce = idevice_connect(d->dev, 62078, &c);
                if (c) idevice_disconnect(c);
                *err += std::wstring(L"\n\nDevice ") + to_wide(d->udid) + (d->network ? L" over Wi-Fi" : L" over USB") +
                        L"; raw connection to lockdownd: " + (ce == IDEVICE_E_SUCCESS ? std::wstring(L"OK") : L"failed (" + std::to_wstring((int)ce) + L")") +
                        L".\nA trace is written to %TEMP%\\wfx_iOS.log.";
            }
        }
        return false;
    }
    if (!d->iosMajor) {
        for (const char* key : { "DeviceName", "DeviceClass", "ProductType", "ProductVersion", "BuildVersion" }) {
            plist_t pv = nullptr; char* ps = nullptr;
            if (lockdownd_get_value(lc, nullptr, key, &pv) == LOCKDOWN_E_SUCCESS && pv) {
                plist_get_string_val(pv, &ps);
                trace("%s: %s", key, ps ? ps : "?");
                if (ps) plist_mem_free(ps);
                plist_free(pv);
            }
        }
        plist_t v = nullptr;
        if (lockdownd_get_value(lc, nullptr, "ProductVersion", &v) == LOCKDOWN_E_SUCCESS && v) {
            char* s = nullptr; plist_get_string_val(v, &s);
            if (s) { d->iosMajor = atoi(s); plist_mem_free(s); }
            plist_free(v);
        }
        if (!d->iosMajor) d->iosMajor = 1;
    }
    *svc = nullptr;
    le = lockdownd_start_service(lc, name, svc);
    lockdownd_client_free(lc);
    trace("start %s: %d (%s), port %d, ssl %d", name, (int)le, lockdownd_strerror(le),
          *svc ? (int)(*svc)->port : 0, *svc ? (int)(*svc)->ssl_enabled : -1);
    if (le != LOCKDOWN_E_SUCCESS || !*svc || !(*svc)->port) {
        if (*svc) { lockdownd_service_descriptor_free(*svc); *svc = nullptr; }
        if (err) *err = L"Could not start service " + to_wide(name) + L": " + to_wide(lockdownd_strerror(le));
        return false;
    }
    return true;
}

enum OpenResult { OPEN_OK, OPEN_NO_DEVICE, OPEN_FAILED };

static OpenResult open_afc_on(const DevicePtr& d, const RPath& rp, AfcPtr& out, std::wstring* err)
{
    AfcPtr a = std::make_shared<Afc>();
    a->device = d;
    lockdownd_service_descriptor_t svc = nullptr;

    if (rp.mode == M_APP) {
        if (!start_service(d, HOUSE_ARREST_SERVICE_NAME, &svc, err)) return OPEN_FAILED;
        house_arrest_error_t he = house_arrest_client_new(d->dev, svc, &a->ha);
        lockdownd_service_descriptor_free(svc);
        if (he != HOUSE_ARREST_E_SUCCESS || !a->ha) {
            if (err) *err = L"Could not connect to house_arrest.";
            return OPEN_FAILED;
        }
        // 1.3 behaviour: VendContainer before iOS 8, VendDocuments from iOS 8 on.
        // ShowAllApps=1 additionally tries the full container first (developer-signed apps only).
        std::vector<const char*> cmds;
        if (d->iosMajor < 8) cmds.push_back("VendContainer");
        else {
            if (ini_bool("ShowAllApps", false)) cmds.push_back("VendContainer");
            cmds.push_back("VendDocuments");
        }
        std::string lastError = "unknown error";
        for (size_t i = 0; i < cmds.size(); i++) {
            if (i > 0) {   // a failed command leaves the client unusable; reconnect
                house_arrest_client_free(a->ha); a->ha = nullptr;
                if (!start_service(d, HOUSE_ARREST_SERVICE_NAME, &svc, err)) return OPEN_FAILED;
                he = house_arrest_client_new(d->dev, svc, &a->ha);
                lockdownd_service_descriptor_free(svc);
                if (he != HOUSE_ARREST_E_SUCCESS || !a->ha) return OPEN_FAILED;
            }
            if (house_arrest_send_command(a->ha, cmds[i], rp.app.c_str()) != HOUSE_ARREST_E_SUCCESS) continue;
            plist_t res = nullptr;
            if (house_arrest_get_result(a->ha, &res) != HOUSE_ARREST_E_SUCCESS || !res) continue;
            plist_t perr = plist_dict_get_item(res, "Error");
            bool ok = perr == nullptr;
            if (perr) { char* s = nullptr; plist_get_string_val(perr, &s); if (s) { lastError = s; plist_mem_free(s); } }
            plist_free(res);
            if (!ok) continue;
            if (afc_client_new_from_house_arrest_client(a->ha, &a->afc) == AFC_E_SUCCESS && a->afc) {
                out = a; return OPEN_OK;
            }
        }
        trace("house_arrest %s: %s", rp.app.c_str(), lastError.c_str());
        if (err) *err = L"The application " + to_wide(rp.app) + L" does not share its files (" + to_wide(lastError) + L").";
        return OPEN_FAILED;
    }

    const char* name = rp.mode == M_ROOT ? "com.apple.afc2" : AFC_SERVICE_NAME;
    if (!start_service(d, name, &svc, err)) return OPEN_FAILED;
    afc_error_t ae = afc_client_new(d->dev, svc, &a->afc);
    lockdownd_service_descriptor_free(svc);
    if (ae != AFC_E_SUCCESS || !a->afc) {
        if (err) *err = L"Could not connect to the AFC service.";
        return OPEN_FAILED;
    }
    out = a;
    return OPEN_OK;
}

// Opens a new, private AFC connection. Retries once with a re-enumerated device
// (handles the device having been unplugged and plugged back in).
static bool open_afc(const RPath& rp, AfcPtr& out, std::wstring* err)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        DevicePtr d = get_device(attempt == 1, err);
        if (!d) { if (attempt == 1) return false; continue; }
        if (open_afc_on(d, rp, out, err) == OPEN_OK) return true;
        if (attempt == 0) {
            std::lock_guard<std::recursive_mutex> l(g_opLock);
            g_cache.clear();
        }
    }
    return false;
}

static bool is_connection_error(afc_error_t e)
{
    return e == AFC_E_MUX_ERROR || e == AFC_E_NOT_ENOUGH_DATA || e == AFC_E_INTERNAL_ERROR ||
           e == AFC_E_OP_TIMEOUT || e == AFC_E_UNKNOWN_ERROR || e == AFC_E_OP_HEADER_INVALID;
}

// Runs fn on the cached connection for this path, reconnecting once if the link died.
// Caller must hold g_opLock.
template <class Fn>
static afc_error_t with_afc(const RPath& rp, std::wstring* err, Fn fn)
{
    std::string key = cache_key(rp);
    afc_error_t r = AFC_E_MUX_ERROR;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!get_device(false, err)) { g_cache.clear(); g_dropCache = false; return AFC_E_MUX_ERROR; }
        if (g_dropCache.exchange(false)) g_cache.clear();
        AfcPtr a;
        auto it = g_cache.find(key);
        if (it != g_cache.end()) a = it->second;
        else {
            if (!open_afc(rp, a, err)) return AFC_E_MUX_ERROR;
            g_cache[key] = a;
        }
        r = fn(a->afc);
        if (!is_connection_error(r)) return r;
        g_cache.erase(key);
    }
    return r;
}

// Diagnostic for a dead AFC link: repeats the first request by hand and reports each step.
static std::wstring afc_probe(const RPath& rp)
{
    std::wstring out = L"\n\nDiagnostics:";
    std::wstring err;
    DevicePtr d = get_device(false, &err);
    if (!d) return out + L" no device (" + err + L")";
    out += L"\n  device " + to_wide(d->udid) + (d->network ? L" over Wi-Fi" : L" over USB") + L", iOS major " + std::to_wstring(d->iosMajor);
    const char* name = rp.mode == M_ROOT ? "com.apple.afc2" : rp.mode == M_APP ? HOUSE_ARREST_SERVICE_NAME : AFC_SERVICE_NAME;
    lockdownd_service_descriptor_t svc = nullptr;
    if (!start_service(d, name, &svc, &err)) return out + L"\n  start " + to_wide(name) + L": " + err;
    out += L"\n  " + to_wide(name) + L": port " + std::to_wstring(svc->port) + L", ssl " + std::to_wstring((int)svc->ssl_enabled);
    service_client_t sc = nullptr;
    DWORD t0 = GetTickCount();
    service_error_t se = service_client_new(d->dev, svc, &sc);
    lockdownd_service_descriptor_free(svc);
    out += L"\n  connect: " + std::to_wstring((int)se) + L" (" + std::to_wstring(GetTickCount() - t0) + L" ms)";
    if (se != SERVICE_E_SUCCESS || !sc) return out;
    if (rp.mode != M_APP) {
        unsigned char pkt[42] = { 'C','F','A','6','L','P','A','A', 42,0,0,0,0,0,0,0, 42,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0, 3,0,0,0,0,0,0,0, '/', 0 };
        uint32_t n = 0;
        se = service_send(sc, (const char*)pkt, sizeof(pkt), &n);
        out += L"\n  send: " + std::to_wstring((int)se) + L", " + std::to_wstring(n) + L" bytes";
        char buf[40]; n = 0; t0 = GetTickCount();
        se = service_receive_with_timeout(sc, buf, sizeof(buf), &n, 10000);
        out += L"\n  receive: " + std::to_wstring((int)se) + L", " + std::to_wstring(n) + L" bytes (" + std::to_wstring(GetTickCount() - t0) + L" ms)";
        if (n >= 8) { out += L", starts with "; for (int i = 0; i < 8; i++) out += (wchar_t)(buf[i] >= 32 && buf[i] < 127 ? buf[i] : L'.'); }
    }
    service_client_free(sc);
    return out;
}

// ---------------------------------------------------------------- stat

struct St {
    bool dir = false, link = false;
    uint64_t size = 0, mtime = 0, birth = 0, nlink = 0, blocks = 0;
    std::string ifmt, target;
};

static afc_error_t afc_stat_raw(afc_client_t afc, const std::string& path, St& st)
{
    char** info = nullptr;
    afc_error_t e = afc_get_file_info(afc, path.c_str(), &info);
    if (e != AFC_E_SUCCESS || !info) return e == AFC_E_SUCCESS ? AFC_E_UNKNOWN_ERROR : e;
    for (int i = 0; info[i] && info[i + 1]; i += 2) {
        const char* k = info[i]; const char* v = info[i + 1];
        if (!strcmp(k, "st_size")) st.size = _strtoui64(v, nullptr, 10);
        else if (!strcmp(k, "st_ifmt")) st.ifmt = v;
        else if (!strcmp(k, "st_mtime")) st.mtime = _strtoui64(v, nullptr, 10);
        else if (!strcmp(k, "st_birthtime")) st.birth = _strtoui64(v, nullptr, 10);
        else if (!strcmp(k, "st_nlink")) st.nlink = _strtoui64(v, nullptr, 10);
        else if (!strcmp(k, "st_blocks")) st.blocks = _strtoui64(v, nullptr, 10);
        else if (!strcmp(k, "LinkTarget")) st.target = v;
    }
    afc_dictionary_free(info);
    st.dir = st.ifmt == "S_IFDIR";
    st.link = st.ifmt == "S_IFLNK";
    return AFC_E_SUCCESS;
}

// Like afc_stat_raw, but a symlink pointing at a directory is reported as a directory.
static afc_error_t afc_stat(afc_client_t afc, const std::string& path, St& st)
{
    afc_error_t e = afc_stat_raw(afc, path, st);
    if (e == AFC_E_SUCCESS && st.link && !st.target.empty()) {
        St t;
        if (afc_stat_raw(afc, join_path(parent_of(path), st.target), t) == AFC_E_SUCCESS && t.dir) st.dir = true;
    }
    return e;
}

static void fill_find_data(WIN32_FIND_DATAW& fd, const std::string& name, const St& st)
{
    memset(&fd, 0, sizeof(fd));
    std::wstring w = to_wide(name);
    wcsncpy(fd.cFileName, w.c_str(), MAX_PATH - 1);
    fd.dwFileAttributes = st.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (st.link) fd.dwFileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    if (!name.empty() && name[0] == '.') fd.dwFileAttributes |= FILE_ATTRIBUTE_HIDDEN;
    if (!st.dir) { fd.nFileSizeLow = (DWORD)st.size; fd.nFileSizeHigh = (DWORD)(st.size >> 32); }
    fd.ftLastWriteTime = unix_ns_to_filetime(st.mtime);
    fd.ftCreationTime = unix_ns_to_filetime(st.birth);
}

static void fill_virtual_dir(WIN32_FIND_DATAW& fd, const std::wstring& name)
{
    memset(&fd, 0, sizeof(fd));
    wcsncpy(fd.cFileName, name.c_str(), MAX_PATH - 1);
    fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    fd.ftLastWriteTime = unix_ns_to_filetime(0);
}

// ---------------------------------------------------------------- listings

struct FindHandle {
    std::vector<WIN32_FIND_DATAW> items;
    size_t next = 0;
};

static bool device_has_afc2(std::wstring* err)
{
    DevicePtr d = get_device(false, err);
    if (!d) return false;
    if (d->hasAfc2 < 0) {
        lockdownd_service_descriptor_t svc = nullptr;
        std::wstring ignored;
        bool ok = start_service(d, "com.apple.afc2", &svc, &ignored);
        if (svc) lockdownd_service_descriptor_free(svc);
        d->hasAfc2 = ok ? 1 : 0;
    }
    return d->hasAfc2 == 1;
}

static bool list_top(FindHandle* h)
{
    WIN32_FIND_DATAW fd;
    std::wstring ignored;
    if (device_has_afc2(&ignored)) { fill_virtual_dir(fd, L"Root"); h->items.push_back(fd); }
    for (const WCHAR* n : { L"User", L"Photos", L"Processed photos" }) {
        fill_virtual_dir(fd, n); h->items.push_back(fd);
    }
    // Photos that came from iCloud Photos are stored outside /DCIM; show them when present.
    RPath user; user.mode = M_USER; user.path = "/";
    St st;
    afc_error_t e = with_afc(user, nullptr, [&](afc_client_t afc) { return afc_stat_raw(afc, ICLOUD_DIR, st); });
    trace("iCloud photo store %s: %d", ICLOUD_DIR, (int)e);
    if (e == AFC_E_SUCCESS && st.dir) { fill_virtual_dir(fd, L"iCloud photos"); h->items.push_back(fd); }
    fill_virtual_dir(fd, L"Applications"); h->items.push_back(fd);
    // Pseudo file: Enter opens the settings dialog.
    memset(&fd, 0, sizeof(fd));
    wcscpy(fd.cFileName, SETTINGS_ITEM);
    fd.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    fd.ftLastWriteTime = unix_ns_to_filetime(0);
    h->items.push_back(fd);
    return true;
}

static std::mutex g_appLock;
static std::map<std::wstring, std::string> g_appNames;     // lower-case shown name -> bundle id
static bool g_appsLoaded = false;

static std::wstring lower(std::wstring s)
{
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

static std::string plist_dict_string(plist_t dict, const char* key)
{
    std::string out;
    plist_t p = plist_dict_get_item(dict, key);
    if (p && plist_get_node_type(p) == PLIST_STRING) {
        char* v = nullptr; plist_get_string_val(p, &v);
        if (v) { out = v; plist_mem_free(v); }
    }
    return out;
}

// Turns an app's display name into something usable as a folder name.
static std::wstring folder_name(const std::string& display)
{
    std::wstring w = to_wide(display);
    for (auto& c : w) if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    while (!w.empty() && (w.back() == L' ' || w.back() == L'.')) w.pop_back();
    while (!w.empty() && w.front() == L' ') w.erase(0, 1);
    if (w.size() > 120) w.resize(120);
    return w;
}

static bool list_apps(FindHandle* h, std::wstring* err);

static std::string resolve_app(const std::wstring& shown)
{
    for (int pass = 0; pass < 2; pass++) {
        {
            std::lock_guard<std::mutex> l(g_appLock);
            auto it = g_appNames.find(lower(shown));
            if (it != g_appNames.end()) return it->second;
            if (g_appsLoaded) break;
        }
        // Panel restored inside an app before the list was ever read: load it once.
        FindHandle tmp; std::wstring ignored;
        std::lock_guard<std::recursive_mutex> l(g_opLock);
        if (!list_apps(&tmp, &ignored)) break;
    }
    return to_utf8(shown.c_str());     // not a known name: treat it as a bundle id
}

static bool list_apps(FindHandle* h, std::wstring* err)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        DevicePtr d = get_device(attempt == 1, err);
        if (!d) continue;
        lockdownd_service_descriptor_t svc = nullptr;
        if (!start_service(d, INSTPROXY_SERVICE_NAME, &svc, err)) continue;
        instproxy_client_t ipc = nullptr;
        instproxy_error_t ie = instproxy_client_new(d->dev, svc, &ipc);
        lockdownd_service_descriptor_free(svc);
        if (ie != INSTPROXY_E_SUCCESS || !ipc) { if (err) *err = L"Could not connect to installation_proxy."; continue; }

        plist_t opts = instproxy_client_options_new();
        instproxy_client_options_add(opts, "ApplicationType", "User", NULL);
        instproxy_client_options_set_return_attributes(opts, "CFBundleIdentifier", "UIFileSharingEnabled",
                                                       "CFBundleDisplayName", "CFBundleName", NULL);
        plist_t apps = nullptr;
        ie = instproxy_browse(ipc, opts, &apps);
        instproxy_client_options_free(opts);
        instproxy_client_free(ipc);
        if (ie != INSTPROXY_E_SUCCESS || !apps) { if (err) *err = L"Could not read the list of applications."; continue; }

        bool all = d->iosMajor < 8 || ini_bool("ShowAllApps", false);
        bool friendly = ini_bool("AppNames", true);
        std::vector<std::pair<std::wstring, std::string>> found;      // shown name, bundle id
        uint32_t n = plist_array_get_size(apps);
        for (uint32_t i = 0; i < n; i++) {
            plist_t app = plist_array_get_item(apps, i);
            if (!app || plist_get_node_type(app) != PLIST_DICT) continue;
            uint8_t sharing = 0;
            plist_t ps = plist_dict_get_item(app, "UIFileSharingEnabled");
            if (ps && plist_get_node_type(ps) == PLIST_BOOLEAN) plist_get_bool_val(ps, &sharing);
            plist_t pid = plist_dict_get_item(app, "CFBundleIdentifier");
            char* id = nullptr;
            if (pid) plist_get_string_val(pid, &id);
            if (!id) continue;
            std::string display = plist_dict_string(app, "CFBundleDisplayName");
            if (display.empty()) display = plist_dict_string(app, "CFBundleName");
            trace("app %s (%s), file sharing %d", id, display.c_str(), (int)sharing);
            std::string bundle = id;
            plist_mem_free(id);
            if (!all && !sharing) continue;
            std::wstring shown = friendly ? folder_name(display) : std::wstring();
            if (shown.empty()) shown = to_wide(bundle);
            found.push_back(std::make_pair(shown, bundle));
        }
        plist_free(apps);

        // Two apps with the same display name get the bundle id appended.
        std::map<std::wstring, int> count;
        for (auto& f : found) count[lower(f.first)]++;
        {
            std::lock_guard<std::mutex> l(g_appLock);
            g_appNames.clear();
            for (auto& f : found) {
                if (count[lower(f.first)] > 1) f.first += L" (" + to_wide(f.second) + L")";
                g_appNames[lower(f.first)] = f.second;
                g_appNames[lower(to_wide(f.second))] = f.second;      // bundle id keeps working too
                WIN32_FIND_DATAW fd;
                fill_virtual_dir(fd, f.first);
                h->items.push_back(fd);
            }
            g_appsLoaded = true;
        }
        std::sort(h->items.begin(), h->items.end(), [](const WIN32_FIND_DATAW& a, const WIN32_FIND_DATAW& b) {
            return _wcsicmp(a.cFileName, b.cFileName) < 0;
        });
        return true;
    }
    return false;
}

static afc_error_t list_dir(afc_client_t afc, const std::string& path, FindHandle* h)
{
    char** names = nullptr;
    afc_error_t e = afc_read_directory(afc, path.c_str(), &names);
    if (e != AFC_E_SUCCESS) return e;
    h->items.clear();
    for (int i = 0; names && names[i]; i++) {
        if (!strcmp(names[i], ".") || !strcmp(names[i], "..")) continue;
        St st;
        afc_error_t se = afc_stat(afc, join_path(path, names[i]), st);
        if (is_connection_error(se)) { afc_dictionary_free(names); return se; }
        WIN32_FIND_DATAW fd;
        fill_find_data(fd, names[i], st);     // unreadable entries are still listed
        h->items.push_back(fd);
    }
    if (names) afc_dictionary_free(names);
    return AFC_E_SUCCESS;
}

// ---------------------------------------------------------------- transfers

static int progress(const WCHAR* src, const WCHAR* dst, uint64_t done, uint64_t total)
{
    if (!g_progressW) return 0;
    int pct = total ? (int)(done * 100 / total) : 100;
    if (pct > 100) pct = 100;
    return g_progressW(g_pluginNr, (WCHAR*)src, (WCHAR*)dst, pct);
}

// "Processed photos": for /DCIM/xxxAPPLE/IMG_nnnn.EXT return the edited rendition if present.
static std::string processed_rendition(afc_client_t afc, const std::string& path, St& st)
{
    // expected shape: /DCIM/<dir>/<stem>.<ext>
    if (path.compare(0, 6, "/DCIM/") != 0) return std::string();
    size_t s = path.find('/', 6);
    if (s == std::string::npos || path.find('/', s + 1) != std::string::npos) return std::string();
    size_t dot = path.rfind('.');
    if (dot == std::string::npos || dot < s) return std::string();
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)tolower(c); });
    if (ext == "jpeg") ext = "jpg";
    if (ext != "jpg" && ext != "heic" && ext != "png") return std::string();
    std::string cand = "/PhotoData/Mutations" + path.substr(0, dot) + "/Adjustments/FullSizeRender." + ext;
    St t;
    if (afc_stat_raw(afc, cand, t) == AFC_E_SUCCESS && !t.dir) { st = t; return cand; }
    return std::string();
}

static int download(const WCHAR* remoteName, const WCHAR* localName, int flags, const RPath& rp)
{
    bool overwrite = (flags & FS_COPYFLAGS_OVERWRITE) != 0;
    bool resume = (flags & FS_COPYFLAGS_RESUME) != 0;
    if (!overwrite && !resume && GetFileAttributesW(localName) != INVALID_FILE_ATTRIBUTES)
        return FS_FILE_EXISTSRESUMEALLOWED;

    AfcPtr a;
    if (!open_afc(rp, a, nullptr)) return FS_FILE_READERROR;

    std::string src = rp.path;
    St st;
    bool haveSt = false;
    if (rp.mode == M_PROCESSED) {
        std::string alt = processed_rendition(a->afc, src, st);
        if (!alt.empty()) { src = alt; haveSt = true; }
    }
    if (!haveSt) {
        afc_error_t e = afc_stat_raw(a->afc, src, st);
        if (e != AFC_E_SUCCESS) return e == AFC_E_OBJECT_NOT_FOUND ? FS_FILE_NOTFOUND : FS_FILE_READERROR;
        if (st.link && !st.target.empty()) {       // download what the link points to
            St t; std::string tp = join_path(parent_of(src), st.target);
            if (afc_stat_raw(a->afc, tp, t) == AFC_E_SUCCESS) { st = t; src = tp; }
        }
    }
    if (st.dir) return FS_FILE_NOTSUPPORTED;

    if (progress(remoteName, localName, 0, st.size)) return FS_FILE_USERABORT;

    uint64_t fh = 0;
    afc_error_t e = afc_file_open(a->afc, src.c_str(), AFC_FOPEN_RDONLY, &fh);
    if (e != AFC_E_SUCCESS) return e == AFC_E_OBJECT_NOT_FOUND ? FS_FILE_NOTFOUND : FS_FILE_READERROR;

    HANDLE out = CreateFileW(localName, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             resume ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (out == INVALID_HANDLE_VALUE) { afc_file_close(a->afc, fh); return FS_FILE_WRITEERROR; }

    int result = FS_FILE_OK;
    uint64_t done = 0;
    if (resume) {
        LARGE_INTEGER sz; sz.QuadPart = 0;
        GetFileSizeEx(out, &sz);
        if ((uint64_t)sz.QuadPart > st.size) sz.QuadPart = 0;
        done = (uint64_t)sz.QuadPart;
        SetFilePointerEx(out, sz, nullptr, FILE_BEGIN);
        SetEndOfFile(out);
        if (done && afc_file_seek(a->afc, fh, (int64_t)done, SEEK_SET) != AFC_E_SUCCESS) result = FS_FILE_READERROR;
    }

    std::vector<char> buf(IO_CHUNK);
    while (result == FS_FILE_OK) {
        uint32_t got = 0;
        e = afc_file_read(a->afc, fh, buf.data(), (uint32_t)buf.size(), &got);
        if (e != AFC_E_SUCCESS) { result = FS_FILE_READERROR; break; }
        if (!got) break;
        DWORD written = 0;
        if (!WriteFile(out, buf.data(), got, &written, nullptr) || written != got) { result = FS_FILE_WRITEERROR; break; }
        done += got;
        if (progress(remoteName, localName, done, st.size)) { result = FS_FILE_USERABORT; break; }
    }
    afc_file_close(a->afc, fh);

    if (result == FS_FILE_OK && st.mtime) {
        FILETIME ft = unix_ns_to_filetime(st.mtime);
        SetFileTime(out, nullptr, nullptr, &ft);
    }
    CloseHandle(out);

    if (result == FS_FILE_OK) {
        if (flags & FS_COPYFLAGS_MOVE) afc_remove_path(a->afc, rp.path.c_str());
        progress(remoteName, localName, 1, 1);
    } else if (!resume) {
        DeleteFileW(localName);                     // keep partial data only when resuming
    }
    return result;
}

static int upload(const WCHAR* localName, const WCHAR* remoteName, int flags, const RPath& rp)
{
    bool overwrite = (flags & FS_COPYFLAGS_OVERWRITE) != 0;
    bool resume = (flags & FS_COPYFLAGS_RESUME) != 0;

    AfcPtr a;
    if (!open_afc(rp, a, nullptr)) return FS_FILE_WRITEERROR;

    St st;
    bool exists = afc_stat_raw(a->afc, rp.path, st) == AFC_E_SUCCESS;
    if (exists && st.dir) return FS_FILE_WRITEERROR;
    if (exists && !overwrite && !resume) return FS_FILE_EXISTSRESUMEALLOWED;

    HANDLE in = CreateFileW(localName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) return FS_FILE_NOTFOUND;
    LARGE_INTEGER total; total.QuadPart = 0;
    GetFileSizeEx(in, &total);

    if (progress(localName, remoteName, 0, (uint64_t)total.QuadPart)) { CloseHandle(in); return FS_FILE_USERABORT; }

    bool append = resume && exists && st.size <= (uint64_t)total.QuadPart;
    uint64_t fh = 0;
    afc_error_t e = afc_file_open(a->afc, rp.path.c_str(), append ? AFC_FOPEN_RW : AFC_FOPEN_WRONLY, &fh);
    if (e != AFC_E_SUCCESS) { CloseHandle(in); return FS_FILE_WRITEERROR; }

    int result = FS_FILE_OK;
    uint64_t done = 0;
    if (append && st.size) {
        LARGE_INTEGER off; off.QuadPart = (LONGLONG)st.size;
        done = st.size;
        if (!SetFilePointerEx(in, off, nullptr, FILE_BEGIN) ||
            afc_file_seek(a->afc, fh, (int64_t)done, SEEK_SET) != AFC_E_SUCCESS) result = FS_FILE_WRITEERROR;
    }

    std::vector<char> buf(IO_CHUNK);
    while (result == FS_FILE_OK) {
        DWORD got = 0;
        if (!ReadFile(in, buf.data(), (DWORD)buf.size(), &got, nullptr)) { result = FS_FILE_READERROR; break; }
        if (!got) break;
        uint32_t off = 0;
        while (off < got) {
            uint32_t w = 0;
            e = afc_file_write(a->afc, fh, buf.data() + off, got - off, &w);
            if (e != AFC_E_SUCCESS || !w) { result = FS_FILE_WRITEERROR; break; }
            off += w;
        }
        if (result != FS_FILE_OK) break;
        done += got;
        if (progress(localName, remoteName, done, (uint64_t)total.QuadPart)) { result = FS_FILE_USERABORT; break; }
    }
    afc_file_close(a->afc, fh);

    FILETIME ft; bool haveTime = GetFileTime(in, nullptr, nullptr, &ft) != 0;
    CloseHandle(in);

    if (result == FS_FILE_OK) {
        if (haveTime) afc_set_file_time(a->afc, rp.path.c_str(), filetime_to_unix_ns(ft));
        if (flags & FS_COPYFLAGS_MOVE) DeleteFileW(localName);
        progress(localName, remoteName, 1, 1);
    } else if (!append) {
        afc_remove_path(a->afc, rp.path.c_str());
    }
    return result;
}

// Device-to-device copy of a single file (possibly between two containers).
static int remote_copy(const WCHAR* oldName, const WCHAR* newName, const RPath& src, const RPath& dst)
{
    AfcPtr a, b;
    if (!open_afc(src, a, nullptr)) return FS_FILE_READERROR;
    if (cache_key(src) == cache_key(dst)) b = a;
    else if (!open_afc(dst, b, nullptr)) return FS_FILE_WRITEERROR;

    St st;
    afc_error_t e = afc_stat_raw(a->afc, src.path, st);
    if (e != AFC_E_SUCCESS) return e == AFC_E_OBJECT_NOT_FOUND ? FS_FILE_NOTFOUND : FS_FILE_READERROR;
    if (st.dir) return FS_FILE_NOTSUPPORTED;

    uint64_t in = 0, out = 0;
    if (afc_file_open(a->afc, src.path.c_str(), AFC_FOPEN_RDONLY, &in) != AFC_E_SUCCESS) return FS_FILE_READERROR;
    if (afc_file_open(b->afc, dst.path.c_str(), AFC_FOPEN_WRONLY, &out) != AFC_E_SUCCESS) {
        afc_file_close(a->afc, in); return FS_FILE_WRITEERROR;
    }
    int result = FS_FILE_OK;
    uint64_t done = 0;
    std::vector<char> buf(IO_CHUNK);
    while (result == FS_FILE_OK) {
        uint32_t got = 0;
        if (afc_file_read(a->afc, in, buf.data(), (uint32_t)buf.size(), &got) != AFC_E_SUCCESS) { result = FS_FILE_READERROR; break; }
        if (!got) break;
        uint32_t off = 0;
        while (off < got) {
            uint32_t w = 0;
            if (afc_file_write(b->afc, out, buf.data() + off, got - off, &w) != AFC_E_SUCCESS || !w) { result = FS_FILE_WRITEERROR; break; }
            off += w;
        }
        if (result != FS_FILE_OK) break;
        done += got;
        if (progress(oldName, newName, done, st.size)) { result = FS_FILE_USERABORT; break; }
    }
    afc_file_close(a->afc, in);
    afc_file_close(b->afc, out);
    if (result == FS_FILE_OK) { if (st.mtime) afc_set_file_time(b->afc, dst.path.c_str(), st.mtime); }
    else afc_remove_path(b->afc, dst.path.c_str());
    return result;
}

// ---------------------------------------------------------------- properties

static std::wstring format_time(uint64_t ns)
{
    if (!ns) return L"-";
    FILETIME ft = unix_ns_to_filetime(ns), lft; SYSTEMTIME stUtc, stLocal;
    FileTimeToSystemTime(&ft, &stUtc);
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &stUtc, &stLocal)) stLocal = stUtc;
    (void)lft;
    WCHAR d[64] = L"", t[64] = L"";
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &stLocal, nullptr, d, 64);
    GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &stLocal, L"H:mm:ss", t, 64);
    return std::wstring(d) + L", " + t;
}

static void show_file_properties(HWND wnd, const WCHAR* remoteName, const RPath& rp)
{
    St st; std::wstring err; uint32_t count = 0; bool counted = false;
    afc_error_t e;
    {
        std::lock_guard<std::recursive_mutex> l(g_opLock);
        e = with_afc(rp, &err, [&](afc_client_t afc) {
            afc_error_t r = afc_stat_raw(afc, rp.path, st);
            if (r == AFC_E_SUCCESS && st.dir) {
                char** names = nullptr;
                if (afc_read_directory(afc, rp.path.c_str(), &names) == AFC_E_SUCCESS && names) {
                    for (int i = 0; names[i]; i++) if (strcmp(names[i], ".") && strcmp(names[i], "..")) count++;
                    afc_dictionary_free(names); counted = true;
                }
            }
            return r;
        });
    }
    if (e != AFC_E_SUCCESS) { show_error(err.empty() ? L"Could not read file information." : err); return; }

    std::wstring type = st.link ? L"Symbolic link" : st.dir ? L"Directory" :
                        st.ifmt == "S_IFBLK" ? L"Block device" :
                        (st.ifmt == "S_IFCHR" || st.ifmt == "S_IFIFO" || st.ifmt == "S_IFSOCK") ? L"Device" : L"File";
    WCHAR size[64] = L"";
    StrFormatByteSizeW((LONGLONG)st.size, size, 64);
    std::wstring msg = L"Type:\t\t" + type;
    if (st.link) msg += L"\nTarget:\t\t" + to_wide(st.target);
    if (st.dir && counted) msg += L"\nContains:\t" + std::to_wstring(count) + L" elements";
    if (!st.dir) msg += L"\nSize:\t\t" + std::wstring(size) + L" (" + std::to_wstring(st.size) + L" bytes)";
    msg += L"\nLinks:\t\t" + std::to_wstring(st.nlink);
    msg += L"\nBlocks:\t\t" + std::to_wstring(st.blocks);
    msg += L"\nCreated:\t\t" + format_time(st.birth);
    msg += L"\nModified:\t" + format_time(st.mtime);
    std::wstring title = L"Properties of " + std::wstring(remoteName);
    MessageBoxW(wnd, msg.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
}

static std::string lockdown_string(lockdownd_client_t lc, const char* key)
{
    std::string out; plist_t v = nullptr;
    if (lockdownd_get_value(lc, nullptr, key, &v) == LOCKDOWN_E_SUCCESS && v) {
        char* s = nullptr; plist_get_string_val(v, &s);
        if (s) { out = s; plist_mem_free(s); }
        plist_free(v);
    }
    return out;
}

// ---------------------------------------------------------------- settings dialog

static void ini_write(const char* key, const char* value)
{
    WritePrivateProfileStringA(INI_SECTION, key, value, g_ini.c_str());
}

// Name, model, OS version and free space of one connected device (no pairing needed for the first three).
static std::wstring device_summary(const std::string& udid, std::wstring* nameOut)
{
    std::wstring out;
    idevice_t dev = nullptr;
    if (idevice_new_with_options(&dev, udid.c_str(), IDEVICE_LOOKUP_USBMUX) != IDEVICE_E_SUCCESS || !dev) {
        if (!ini_bool("AllowWiFi", false) ||
            idevice_new_with_options(&dev, udid.c_str(), IDEVICE_LOOKUP_NETWORK) != IDEVICE_E_SUCCESS || !dev)
            return L"Not connected.";
    }
    lockdownd_client_t lc = nullptr;
    lockdownd_error_t le = lockdownd_client_new(dev, &lc, PLUGIN_LABEL);
    if (le != LOCKDOWN_E_SUCCESS || !lc) { idevice_free(dev); return lockdown_message(le); }
    std::wstring name = to_wide(lockdown_string(lc, "DeviceName"));
    if (nameOut) *nameOut = name;
    out = L"Name:\t" + name;
    out += L"\r\nModel:\t" + to_wide(lockdown_string(lc, "ProductType"));
    out += L"\r\niOS:\t" + to_wide(lockdown_string(lc, "ProductVersion")) + L" (" + to_wide(lockdown_string(lc, "BuildVersion")) + L")";
    lockdownd_client_free(lc);
    idevice_free(dev);
    return out;
}

static std::wstring free_space_line()
{
    std::wstring line;
    RPath user; user.mode = M_USER; user.path = "/";
    std::lock_guard<std::recursive_mutex> l(g_opLock);
    with_afc(user, nullptr, [&](afc_client_t afc) {
        char *t = nullptr, *f = nullptr;
        afc_error_t r = afc_get_device_info_key(afc, "FSTotalBytes", &t);
        if (r == AFC_E_SUCCESS) r = afc_get_device_info_key(afc, "FSFreeBytes", &f);
        if (r == AFC_E_SUCCESS && t && f) {
            WCHAR b[128];
            swprintf(b, 128, L"\r\nFree:\t%.1f GB of %.1f GB",
                     _strtoui64(f, nullptr, 10) / 1073741824.0, _strtoui64(t, nullptr, 10) / 1073741824.0);
            line = b;
        }
        free(t); free(f);
        return r;
    });
    return line;
}

struct SettingsState {
    std::vector<std::string> udids;      // combo index -> UDID ("" = any device)
};

static void settings_show_info(HWND dlg, SettingsState* st)
{
    int sel = (int)SendDlgItemMessageW(dlg, IDC_DEVICE, CB_GETCURSEL, 0, 0);
    std::wstring info;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    if (sel > 0 && sel < (int)st->udids.size()) {
        info = device_summary(st->udids[sel], nullptr) + L"\r\nUDID:\t" + to_wide(st->udids[sel]);
    } else {
        std::wstring err;
        DevicePtr d = get_device(false, &err);
        if (!d) info = err;
        else info = device_summary(d->udid, nullptr) + free_space_line() + L"\r\nUDID:\t" + to_wide(d->udid);
    }
    SetCursor(old);
    SetDlgItemTextW(dlg, IDC_INFO, info.c_str());
}

static void settings_fill_devices(HWND dlg, SettingsState* st)
{
    HWND combo = GetDlgItem(dlg, IDC_DEVICE);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    st->udids.clear();
    st->udids.push_back("");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"<Any connected device>");

    std::string preferred = ini_string("PreferredDevice");
    bool wifi = IsDlgButtonChecked(dlg, IDC_WIFI) == BST_CHECKED;
    usbmuxd_device_info_t* list = nullptr;
    int n = usbmuxd_get_device_list(&list);
    for (int i = 0; i < n; i++) {
        bool net = list[i].conn_type == CONNECTION_TYPE_NETWORK;
        if (net && !wifi) continue;
        std::string udid = list[i].udid;
        if (std::find(st->udids.begin(), st->udids.end(), udid) != st->udids.end()) continue;
        std::wstring name;
        device_summary(udid, &name);
        std::wstring label = (name.empty() ? to_wide(udid) : name + L"  (" + to_wide(udid) + L")") + (net ? L"  [Wi-Fi]" : L"");
        st->udids.push_back(udid);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)label.c_str());
    }
    if (list) usbmuxd_device_list_free(&list);

    int sel = 0;
    if (!preferred.empty()) {
        auto it = std::find(st->udids.begin(), st->udids.end(), preferred);
        if (it == st->udids.end()) {
            st->udids.push_back(preferred);
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)(to_wide(preferred) + L"  (not connected)").c_str());
            sel = (int)st->udids.size() - 1;
        } else sel = (int)(it - st->udids.begin());
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
    settings_show_info(dlg, st);
}

static INT_PTR CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    SettingsState* st = (SettingsState*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        st = (SettingsState*)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)st);
        HICON ico = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_PLUGIN));
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)ico);
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)ico);
        CheckDlgButton(dlg, IDC_APPNAMES, ini_bool("AppNames", true) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_ALLAPPS, ini_bool("ShowAllApps", false) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_WIFI, ini_bool("AllowWiFi", false) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_LOG, ini_bool("Log", true) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_DEBUG, ini_bool("Debug", false) ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(GetDlgItem(dlg, IDC_DEBUG), IsDlgButtonChecked(dlg, IDC_LOG) == BST_CHECKED);
        EnableWindow(GetDlgItem(dlg, IDC_OPENLOG), !g_logPath.empty());
        SetDlgItemTextW(dlg, IDC_ABOUT, L"wfx_iOS 2.0 (" __DATE__ L") \x2013 update of wfx_iOS 1.3 by Domo, the original author");
        settings_fill_devices(dlg, st);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_DEVICE:
            if (HIWORD(wp) == CBN_SELCHANGE) settings_show_info(dlg, st);
            return TRUE;
        case IDC_REFRESH:
        case IDC_WIFI:
            settings_fill_devices(dlg, st);
            return TRUE;
        case IDC_LOG:
            EnableWindow(GetDlgItem(dlg, IDC_DEBUG), IsDlgButtonChecked(dlg, IDC_LOG) == BST_CHECKED);
            return TRUE;
        case IDC_OPENLOG:
            if (!g_logPath.empty()) ShellExecuteW(dlg, L"open", L"notepad.exe", (L"\"" + g_logPath + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            return TRUE;
        case IDOK: {
            int sel = (int)SendDlgItemMessageW(dlg, IDC_DEVICE, CB_GETCURSEL, 0, 0);
            std::string udid = (sel > 0 && sel < (int)st->udids.size()) ? st->udids[sel] : std::string();
            bool deviceChanged = udid != ini_string("PreferredDevice");
            bool logWas = ini_bool("Log", true);
            ini_write("PreferredDevice", udid.empty() ? nullptr : udid.c_str());
            ini_write("AppNames", IsDlgButtonChecked(dlg, IDC_APPNAMES) == BST_CHECKED ? "1" : "0");
            ini_write("ShowAllApps", IsDlgButtonChecked(dlg, IDC_ALLAPPS) == BST_CHECKED ? "1" : "0");
            ini_write("AllowWiFi", IsDlgButtonChecked(dlg, IDC_WIFI) == BST_CHECKED ? "1" : "0");
            bool log = IsDlgButtonChecked(dlg, IDC_LOG) == BST_CHECKED;
            bool debug = log && IsDlgButtonChecked(dlg, IDC_DEBUG) == BST_CHECKED;
            ini_write("Log", log ? "1" : "0");
            ini_write("Debug", debug ? "1" : "0");
            if (g_trace) {                       // the trace level can change while running
                idevice_set_debug_level(debug ? 1 : 0);
                libusbmuxd_set_debug_level(debug ? 1 : 0);
            }
            if (deviceChanged) {
                std::lock_guard<std::recursive_mutex> l(g_opLock);
                g_cache.clear();
                std::lock_guard<std::mutex> l2(g_devLock);
                g_device.reset();
            }
            {
                std::lock_guard<std::mutex> l(g_appLock);     // names / all-apps may have changed
                g_appNames.clear(); g_appsLoaded = false;
            }
            if (log != logWas)
                MessageBoxW(dlg, L"The log file setting takes effect after Total Commander is restarted.", PLUGIN_TITLE, MB_OK | MB_ICONINFORMATION);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// Opened by Enter on "Settings" and by Alt+Enter on the plugin root.
static void show_settings(HWND wnd)
{
    std::wstring ignored;
    get_device(false, &ignored);          // makes sure sockets and the log are initialised
    SettingsState st;
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_SETTINGS), wnd, settings_proc, (LPARAM)&st);
}

// ---------------------------------------------------------------- icons

static HICON stock_icon(int siid, bool smallIcon)
{
    SHSTOCKICONINFO sii; memset(&sii, 0, sizeof(sii)); sii.cbSize = sizeof(sii);
    if (FAILED(SHGetStockIconInfo((SHSTOCKICONID)siid, SHGSI_ICON | (smallIcon ? SHGSI_SMALLICON : SHGSI_LARGEICON), &sii))) return nullptr;
    return sii.hIcon;
}

static std::mutex g_sbLock;
static sbservices_client_t g_sb = nullptr;
static DevicePtr g_sbDevice;
static std::map<std::string, std::vector<unsigned char>> g_iconPng;    // bundle id -> PNG (empty = none)

static bool icon_cached(const std::string& bundle)
{
    std::lock_guard<std::mutex> l(g_sbLock);
    return g_iconPng.count(bundle) != 0;
}

// Home-screen icon of an app as PNG, from SpringBoard. Cached per device connection.
static bool app_icon_png(const std::string& bundle, std::vector<unsigned char>& out)
{
    std::wstring ignored;
    DevicePtr d = get_device(false, &ignored);
    std::lock_guard<std::mutex> l(g_sbLock);
    if (d != g_sbDevice) {
        if (g_sb) { sbservices_client_free(g_sb); g_sb = nullptr; }
        g_iconPng.clear();
        g_sbDevice = d;
    }
    if (!d) return false;
    auto it = g_iconPng.find(bundle);
    if (it != g_iconPng.end()) { out = it->second; return !out.empty(); }

    if (!g_sb) {
        lockdownd_service_descriptor_t svc = nullptr;
        if (!start_service(d, SBSERVICES_SERVICE_NAME, &svc, &ignored)) return false;
        sbservices_error_t se = sbservices_client_new(d->dev, svc, &g_sb);
        lockdownd_service_descriptor_free(svc);
        if (se != SBSERVICES_E_SUCCESS || !g_sb) { g_sb = nullptr; return false; }
    }
    char* png = nullptr; uint64_t size = 0;
    sbservices_error_t se = sbservices_get_icon_pngdata(g_sb, bundle.c_str(), &png, &size);
    if (se != SBSERVICES_E_SUCCESS) {            // connection is unusable after an error
        sbservices_client_free(g_sb); g_sb = nullptr;
        trace("icon for %s: sbservices error %d", bundle.c_str(), (int)se);
    }
    std::vector<unsigned char>& slot = g_iconPng[bundle];
    if (se == SBSERVICES_E_SUCCESS && png && size) slot.assign((unsigned char*)png, (unsigned char*)png + size);
    if (png) free(png);
    out = slot;
    return !out.empty();
}

static HICON png_to_icon(const std::vector<unsigned char>& png, int size)
{
    static std::once_flag once; static ULONG_PTR token = 0;
    std::call_once(once, [] { Gdiplus::GdiplusStartupInput in; Gdiplus::GdiplusStartup(&token, &in, nullptr); });
    if (!token || png.empty()) return nullptr;
    IStream* stream = SHCreateMemStream(png.data(), (UINT)png.size());
    if (!stream) return nullptr;
    HICON icon = nullptr;
    {
        Gdiplus::Bitmap src(stream);
        if (src.GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Bitmap dst(size, size, PixelFormat32bppARGB);
            Gdiplus::Graphics g(&dst);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            g.DrawImage(&src, Gdiplus::Rect(0, 0, size, size), 0, 0, (INT)src.GetWidth(), (INT)src.GetHeight(), Gdiplus::UnitPixel);
            dst.GetHICON(&icon);
        }
    }
    stream->Release();
    return icon;
}

// ---------------------------------------------------------------- exported API

extern "C" {

int __stdcall FsInit(int PluginNr, tProgressProc pProgressProc, tLogProc, tRequestProc)
{
    g_pluginNr = PluginNr;
    g_progressA = pProgressProc;
    return 0;
}

int __stdcall FsInitW(int PluginNr, tProgressProcW pProgressProcW, tLogProcW pLogProcW, tRequestProcW pRequestProcW)
{
    g_pluginNr = PluginNr;
    g_progressW = pProgressProcW;
    g_logW = pLogProcW;
    g_requestW = pRequestProcW;
    return 0;
}

void __stdcall FsGetDefRootName(char* DefRootName, int maxlen)
{
    if (maxlen > 0) { strncpy(DefRootName, "iOS", maxlen - 1); DefRootName[maxlen - 1] = 0; }
}

void __stdcall FsSetDefaultParams(FsDefaultParamStruct* dps)
{
    if (dps && dps->DefaultIniName[0]) g_ini = dps->DefaultIniName;
}

int __stdcall FsGetBackgroundFlags(void)
{
    return BG_DOWNLOAD | BG_UPLOAD;
}

HANDLE __stdcall FsFindFirstW(WCHAR* Path, WIN32_FIND_DATAW* FindData)
{
    RPath rp = parse_path(Path);
    std::unique_ptr<FindHandle> h(new FindHandle());
    std::wstring err;
    bool ok = false;

    std::lock_guard<std::recursive_mutex> l(g_opLock);
    switch (rp.mode) {
    case M_TOP:     ok = list_top(h.get()); break;
    case M_APPLIST: ok = list_apps(h.get(), &err); break;
    case M_INVALID: break;
    default: {
        afc_error_t e = with_afc(rp, &err, [&](afc_client_t afc) { return list_dir(afc, rp.path, h.get()); });
        ok = e == AFC_E_SUCCESS;
        if (!ok && rp.mode == M_APP && rp.path == "/" && e == AFC_E_PERM_DENIED) {
            // VendDocuments exposes only /Documents; the container root itself cannot be listed.
            St st;
            afc_error_t se = with_afc(rp, &err, [&](afc_client_t afc) { return afc_stat(afc, "/Documents", st); });
            trace("app root not listable, /Documents: %d", (int)se);
            if (se == AFC_E_SUCCESS) {
                WIN32_FIND_DATAW fd;
                fill_find_data(fd, "Documents", st);
                fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                h->items.clear();
                h->items.push_back(fd);
                ok = true;
            }
        }
        if (!ok) trace("AFC error %d for %s", (int)e, rp.path.c_str());
        if (!ok && err.empty() && e != AFC_E_OBJECT_NOT_FOUND && e != AFC_E_PERM_DENIED && e != AFC_E_READ_ERROR)
            err = L"Could not read the directory (AFC error " + std::to_wstring((int)e) + L")." +
                  (is_connection_error(e) ? afc_probe(rp) : std::wstring());
        break; }
    }
    trace("list %s -> %s%s", to_utf8(Path).c_str(), ok ? "ok, entries: " : "FAILED", ok ? std::to_string(h->items.size()).c_str() : "");
    if (!ok) {
        if (!err.empty()) { trace("%s", to_utf8(err.c_str()).c_str()); show_error(err); }
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (h->items.empty()) { SetLastError(ERROR_NO_MORE_FILES); return INVALID_HANDLE_VALUE; }
    *FindData = h->items[0];
    h->next = 1;
    return (HANDLE)h.release();
}

BOOL __stdcall FsFindNextW(HANDLE Hdl, WIN32_FIND_DATAW* FindData)
{
    FindHandle* h = (FindHandle*)Hdl;
    if (!h || Hdl == INVALID_HANDLE_VALUE || h->next >= h->items.size()) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    *FindData = h->items[h->next++];
    return TRUE;
}

int __stdcall FsFindClose(HANDLE Hdl)
{
    if (Hdl && Hdl != INVALID_HANDLE_VALUE) delete (FindHandle*)Hdl;
    return 0;
}

static void find_data_to_ansi(const WIN32_FIND_DATAW& w, WIN32_FIND_DATAA* a)
{
    memset(a, 0, sizeof(*a));
    a->dwFileAttributes = w.dwFileAttributes;
    a->ftCreationTime = w.ftCreationTime; a->ftLastAccessTime = w.ftLastAccessTime; a->ftLastWriteTime = w.ftLastWriteTime;
    a->nFileSizeHigh = w.nFileSizeHigh; a->nFileSizeLow = w.nFileSizeLow;
    WideCharToMultiByte(CP_ACP, 0, w.cFileName, -1, a->cFileName, MAX_PATH - 1, nullptr, nullptr);
}

HANDLE __stdcall FsFindFirst(char* Path, WIN32_FIND_DATAA* FindData)
{
    WIN32_FIND_DATAW w;
    std::wstring p = to_wide(Path ? Path : "", CP_ACP);
    HANDLE h = FsFindFirstW((WCHAR*)p.c_str(), &w);
    if (h != INVALID_HANDLE_VALUE) find_data_to_ansi(w, FindData);
    return h;
}

BOOL __stdcall FsFindNext(HANDLE Hdl, WIN32_FIND_DATAA* FindData)
{
    WIN32_FIND_DATAW w;
    if (!FsFindNextW(Hdl, &w)) return FALSE;
    find_data_to_ansi(w, FindData);
    return TRUE;
}

int __stdcall FsGetFileW(WCHAR* RemoteName, WCHAR* LocalName, int CopyFlags, RemoteInfoStruct*)
{
    RPath rp = parse_path(RemoteName);
    if (!is_afc_mode(rp.mode)) return FS_FILE_NOTSUPPORTED;
    return download(RemoteName, LocalName, CopyFlags, rp);
}

int __stdcall FsPutFileW(WCHAR* LocalName, WCHAR* RemoteName, int CopyFlags)
{
    RPath rp = parse_path(RemoteName);
    if (!is_afc_mode(rp.mode) || rp.path == "/") return FS_FILE_NOTSUPPORTED;
    return upload(LocalName, RemoteName, CopyFlags, rp);
}

int __stdcall FsRenMovFileW(WCHAR* OldName, WCHAR* NewName, BOOL Move, BOOL OverWrite, RemoteInfoStruct*)
{
    RPath src = parse_path(OldName), dst = parse_path(NewName);
    if (!is_afc_mode(src.mode) || !is_afc_mode(dst.mode) || src.path == "/" || dst.path == "/") return FS_FILE_NOTSUPPORTED;
    bool same = cache_key(src) == cache_key(dst);

    {
        std::lock_guard<std::recursive_mutex> l(g_opLock);
        St st;
        afc_error_t e = with_afc(dst, nullptr, [&](afc_client_t afc) { return afc_stat_raw(afc, dst.path, st); });
        if (e == AFC_E_SUCCESS) {
            if (!OverWrite) return FS_FILE_EXISTS;
            if (same && src.path == dst.path) return FS_FILE_OK;
        } else if (is_connection_error(e)) return FS_FILE_WRITEERROR;

        if (Move && same) {
            if (progress(OldName, NewName, 0, 1)) return FS_FILE_USERABORT;
            e = with_afc(src, nullptr, [&](afc_client_t afc) {
                if (OverWrite) afc_remove_path(afc, dst.path.c_str());
                return afc_rename_path(afc, src.path.c_str(), dst.path.c_str());
            });
            if (e == AFC_E_OBJECT_NOT_FOUND) return FS_FILE_NOTFOUND;
            if (e != AFC_E_SUCCESS) return FS_FILE_WRITEERROR;
            progress(OldName, NewName, 1, 1);
            return FS_FILE_OK;
        }
    }

    int r = remote_copy(OldName, NewName, src, dst);
    if (r == FS_FILE_OK && Move) {
        std::lock_guard<std::recursive_mutex> l(g_opLock);
        with_afc(src, nullptr, [&](afc_client_t afc) { return afc_remove_path(afc, src.path.c_str()); });
    }
    return r;
}

BOOL __stdcall FsDeleteFileW(WCHAR* RemoteName)
{
    RPath rp = parse_path(RemoteName);
    if (!is_afc_mode(rp.mode) || rp.path == "/") return FALSE;
    std::lock_guard<std::recursive_mutex> l(g_opLock);
    return with_afc(rp, nullptr, [&](afc_client_t afc) { return afc_remove_path(afc, rp.path.c_str()); }) == AFC_E_SUCCESS;
}

BOOL __stdcall FsRemoveDirW(WCHAR* RemoteName)
{
    return FsDeleteFileW(RemoteName);    // AFC removes empty directories with the same call
}

BOOL __stdcall FsMkDirW(WCHAR* Path)
{
    RPath rp = parse_path(Path);
    if (!is_afc_mode(rp.mode) || rp.path == "/") return FALSE;
    std::lock_guard<std::recursive_mutex> l(g_opLock);
    return with_afc(rp, nullptr, [&](afc_client_t afc) { return afc_make_directory(afc, rp.path.c_str()); }) == AFC_E_SUCCESS;
}

BOOL __stdcall FsSetTimeW(WCHAR* RemoteName, FILETIME*, FILETIME*, FILETIME* LastWriteTime)
{
    RPath rp = parse_path(RemoteName);
    if (!is_afc_mode(rp.mode) || !LastWriteTime) return FALSE;
    uint64_t ns = filetime_to_unix_ns(*LastWriteTime);
    std::lock_guard<std::recursive_mutex> l(g_opLock);
    return with_afc(rp, nullptr, [&](afc_client_t afc) { return afc_set_file_time(afc, rp.path.c_str(), ns); }) == AFC_E_SUCCESS;
}

int __stdcall FsExtractCustomIconW(WCHAR* RemoteName, int ExtractFlags, HICON* TheIcon)
{
    if (!RemoteName || !TheIcon) return FS_ICON_USEDEFAULT;
    bool smallIcon = (ExtractFlags & FS_ICONFLAG_SMALL) != 0;
    RPath rp = parse_path(RemoteName);
    int siid = -1;
    switch (rp.mode) {
    case M_ROOT:      if (rp.path == "/") siid = 8; break;                    // SIID_DRIVEFIXED
    case M_USER:      if (rp.path == "/") siid = 97; break;                   // SIID_DEVICECELLPHONE
    case M_PHOTOS:    if (rp.path == "/DCIM") siid = 98; break;               // SIID_DEVICECAMERA
    case M_PROCESSED: if (rp.path == "/DCIM") siid = 72; break;               // SIID_IMAGEFILES
    case M_ICLOUD:    if (rp.path == ICLOUD_DIR) siid = 13; break;            // SIID_WORLD
    case M_APPLIST:   siid = 2; break;                                        // SIID_APPLICATION
    case M_SETTINGS:  siid = 106; break;                                      // SIID_SETTINGS
    case M_APP:
        if (rp.path == "/") {
            if (!icon_cached(rp.app) && !(ExtractFlags & FS_ICONFLAG_BACKGROUND)) return FS_ICON_DELAYED;
            std::vector<unsigned char> png;
            if (app_icon_png(rp.app, png)) {
                HICON h = png_to_icon(png, GetSystemMetrics(smallIcon ? SM_CXSMICON : SM_CXICON));
                if (h) {
                    *TheIcon = h;
                    _snwprintf(RemoteName, MAX_PATH - 1, L"wfx_iOS:app:%hs", rp.app.c_str());
                    RemoteName[MAX_PATH - 1] = 0;
                    return FS_ICON_EXTRACTED_DESTROY;
                }
            }
            siid = 2;
        }
        break;
    default: break;
    }
    if (siid < 0) return FS_ICON_USEDEFAULT;
    HICON h = stock_icon(siid, smallIcon);
    if (!h) return FS_ICON_USEDEFAULT;
    *TheIcon = h;
    _snwprintf(RemoteName, MAX_PATH - 1, L"wfx_iOS:stock:%d", siid);
    RemoteName[MAX_PATH - 1] = 0;
    return FS_ICON_EXTRACTED_DESTROY;
}

int __stdcall FsExecuteFileW(HWND MainWin, WCHAR* RemoteName, WCHAR* Verb)
{
    if (!Verb) return FS_EXEC_ERROR;
    if (!_wcsicmp(Verb, L"open")) {
        if (parse_path(RemoteName).mode == M_SETTINGS) { show_settings(MainWin); return FS_EXEC_OK; }
        return FS_EXEC_YOURSELF;                                 // let TC download and open it
    }
    if (!_wcsnicmp(Verb, L"properties", 10)) {
        RPath rp = parse_path(RemoteName);
        if (rp.mode == M_TOP || rp.mode == M_SETTINGS) show_settings(MainWin);
        else if (is_afc_mode(rp.mode)) show_file_properties(MainWin, RemoteName, rp);
        return FS_EXEC_OK;
    }
    return FS_EXEC_ERROR;
}

} // extern "C"

BOOL APIENTRY DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) g_hInst = inst;
    return TRUE;
}
