# wfx_iOS 1.3 - reverse-engineering notes

Target: `wfx_iOS.wfx64` (PE32+, MSVC 12 / VS2013, linked 2017-02-01). Static disassembly only
(capstone over the plugin's own code at RVA 0x13A000-0x13E800); nothing was executed.
IDA's decompiler was not reachable from this session, so every item below is read from
disassembly plus string/import references.

## Observed

| What | Evidence (RVA) |
|---|---|
| Statically links libimobiledevice, libusbmuxd, libplist, **OpenSSL 1.1.0b (Sep 2016)**, libxml2 | strings, readme |
| 18 exports; `FsFindFirst`/`FsFindNext`/`FsInit` share the address of their W versions | export table |
| `FsGetDefRootName` -> "iOS"; `FsGetBackgroundFlags` -> 3 (download + upload) | 13BCB0, 13BCF0 |
| `FsSetDefaultParams` keeps `DefaultIniName`; reads `[wfx_iOS] PreferredDevice` (UDID) | 13BD00 |
| Path parser: first component selects a mode - 1 top, 2 `Root`, 3 `User`, 4 `Photos`, 5 `Processed photos`, 6 `Applications`, 7 inside one app. Modes 4/5 prefix `/DCIM`; `\`->`/`, UTF-16->UTF-8 | 13A8E0 |
| Service per mode: modes <=2 `com.apple.afc2`, others `com.apple.afc`, mode 7 `com.apple.mobile.house_arrest` | 13AE61-13AF15 |
| house_arrest command: `VendContainer` if iOS major < 8, else `VendDocuments`; result checked for `Error` | 13AF5F-13AFC3 |
| iOS major comes from lockdown `ProductVersion`; `DeviceName` also read | 13ADB8-13AE3C |
| App list: installation_proxy browse, `ApplicationType=User`, attributes `CFBundleIdentifier` + `UIFileSharingEnabled`; on iOS >= 8 only file-sharing apps are listed; entries are bundle ids | 13B5E3-13B890 |
| Directory entries built from AFC file info: `st_size`, `st_ifmt`, `st_mtime`, `st_birthtime`, `st_nlink`, `st_blocks`, `LinkTarget` | 13B0C5-13B3D9 |
| `FsGetFileW`: returns 7 (exists/resume allowed) if the local file exists and overwrite is not set; sets local file time | 13BFFF-13C411 |
| Mode 5 download: path matching `/DCIM/???APPLE/IMG_????.JPG` is replaced by `/PhotoData/Mutations<path without ext>/Adjustments/FullSizeRender.jpg` when that exists | 13C041-13C12F |
| `FsRenMovFileW` copies through a temp file (`GetTempFileNameW`, prefix `TCMD`) | 13CDA1 |
| `FsExecuteFileW`: `open` -> -1 (TC handles it); `properties` on `\` -> device dialog (resource 0x67: device combo with `<Any connected device>`, writes `PreferredDevice`); `properties` elsewhere -> file dialog (0x66); other verbs -> 1 | 13DDE0 |
| Device dialog shows `DeviceName`, AFC `Model`, `FSTotalBytes`/`FSFreeBytes` as "%.1f Gb of %.1f Gb (user partition)" | 13D850-13D9DB |

## Inferred (not proven by execution)

- The plugin fails on current iOS because of its 2016/2017 protocol stack (old OpenSSL and
  pairing/lockdown code), not because of anything in the plugin logic itself. The services it
  uses (afc, house_arrest, installation_proxy over lockdownd) still exist.
- Whether `Root` was hidden when afc2 is missing was not determined.

## Not examined

- The 32-bit build (assumed to be the same source).
- Dialog resources beyond the control strings referenced from code.

## Design choices in 2.0 that differ from 1.3

- Dialogs are message boxes (no .rc); device switching cycles through connected devices.
- `Root` is listed only when afc2 actually starts.
- Rendition lookup also covers HEIC/PNG and any file name, not only `IMG_????.JPG`.
- Device-to-device copy streams directly instead of going through a temp file; download/upload resume is supported.
- `ShowAllApps` ini option; Wi-Fi-synced devices are accepted when no USB device is present.
- Proper ANSI `FsFindFirst`/`FsFindNext` wrappers.
