# Changelog

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
