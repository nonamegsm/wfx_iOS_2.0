# Changelog

## 2.9

- `Home screen`: the icon layout of the device as folders - `Dock`, `Page 01`, `Page 02`, ...
  with the device's folders inside. Move applications between pages and folders with F6,
  make a folder with F7, rename a folder, move a whole folder to another page.
- `Home screen\layout.plist` is the complete layout as a file: copy it to the PC to keep it,
  copy a saved one back to restore it. The layout is also saved automatically before
  every change (`%LOCALAPPDATA%\wfx_iOS`).

## 2.8

- `Device information` window: serial number, IMEI, ECID, UDID, Wi-Fi / Bluetooth addresses,
  model, system version, storage, battery and every other value the device reports.
  Copy one value, copy everything, or save it to a text file. Opened with Enter on
  `Device information` in a device's folder, or from the settings dialog.

## 2.7

- Translations: Ukrainian, Russian and Polish, next to English. The language follows
  Windows and can be chosen in the settings dialog.

## 2.6

- Install an application by copying an `.ipa` file into `Applications`.
- Uninstall an application by deleting its folder in `Applications`; always asks first,
  before any of the application's documents are touched.

## 2.5

- `Photos by month`: the whole photo library grouped by the month a picture was taken,
  with the original file names. Read from the device's photo database (SQLite).

## 2.4

- Custom columns: creation time, type, hard links, blocks, link target, device path
  and (for applications) the bundle id can be shown as Total Commander columns.

## 2.3

- Several devices at once: with more than one device connected (and no preferred
  device selected in the settings) the plugin root shows one folder per device.
  Files can be copied directly from one device to another.

## 2.2

- File properties (Alt+Enter) are a real dialog; the values can be selected and copied.

## 2.1

- Source rewritten as plain, commented C (C99 style), split into modules.
  No change in behaviour is intended.
- Application icons are scaled with the Windows Imaging Component.
- The version number now comes from `version.txt`.

## 2.0

- First update of wfx_iOS 1.3 by Domo for current iOS / iPadOS:
  current libimobiledevice and OpenSSL 3, application names and icons,
  `iCloud photos` folder, settings dialog, setup program, resume for transfers,
  no hang when the device is unplugged.
