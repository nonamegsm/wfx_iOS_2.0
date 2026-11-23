# wfx_iOS 2.0

Total Commander file-system plugin for browsing iPhone / iPad files.

## Credits

**This is an update of wfx_iOS 1.3 by Domo, the original author.** The idea, the
folder layout and the feature set are his work, and all credit for the plugin
belongs to him. Version 1.3 (2017) no longer works with current iOS because the
protocol libraries it was built with are out of date; this project only brings
the plugin back onto a current stack.

It is an unofficial continuation, not a release by the original author. No code
or binaries from the original plugin are included: the behaviour of 1.3 was
studied and written again from scratch (see [docs/RE-NOTES.md](docs/RE-NOTES.md)).
Domo, if you would like anything here changed, credited differently or taken
down, please open an issue.

Built on [libimobiledevice](https://libimobiledevice.org/), libusbmuxd, libplist,
libimobiledevice-glue and [OpenSSL](https://www.openssl.org/).

## Requirements

- Total Commander (32- or 64-bit)
- Apple Mobile Device Support (comes with iTunes or the "Apple Devices" app), with the
  "Apple Mobile Device Service" running
- The device unlocked and "Trust this computer" accepted

## Install

Download the zip from [Releases](../../releases) and open it in Total Commander; it
offers to install the plugin. Then open *Network Neighborhood* -> *iOS*.

## Folders

| Folder | Content |
|---|---|
| `Root` | whole file system (jailbroken devices with afc2 only; hidden otherwise) |
| `User` | media partition |
| `Photos` | `/DCIM` |
| `Processed photos` | `/DCIM`, but downloads fetch the edited version of a photo when one exists |
| `iCloud photos` | photos synced from iCloud Photos (`/PhotoData/CPLAssets`); shown only when the device has them |
| `Applications` | documents of apps that enable file sharing, by app name |

Alt+Enter on the plugin root shows device information and switches the preferred
device when several are connected. Alt+Enter on a file shows its properties.

## Settings

Section `[wfx_iOS]` in Total Commander's `fsplugin.ini`:

| Key | Meaning |
|---|---|
| `PreferredDevice=<UDID>` | use only this device (empty = any connected device) |
| `ShowAllApps=1` | list all user apps and try their full container first (developer-signed apps only) |
| `AppNames=0` | list applications by bundle id instead of by name |
| `LogFile=<path>` | write the trace to this file instead of `%TEMP%\wfx_iOS.log` |
| `Debug=0` | turn off the protocol trace written to `%TEMP%\wfx_iOS.log` (on by default during the beta) |

## Changes from 1.3

- Current libimobiledevice and OpenSSL 3
- Edited-photo lookup also covers HEIC and PNG
- Resume for downloads and uploads; direct device-to-device copy
- `Root` is shown only when the device offers afc2
- Wi-Fi-synced devices are accepted when no USB device is connected
- The two dialogs of 1.3 are plain message boxes for now

## Build

Run `build.bat` (Visual Studio with C++ and git required); dependencies come from
vcpkg. Every push is also built by GitHub Actions for x86 and x64.

## Status

New code. Please report problems with the iOS version and device model.

## License

LGPL-2.1-or-later, the same license as libimobiledevice. See [LICENSE](LICENSE).
