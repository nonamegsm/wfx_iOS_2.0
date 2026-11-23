# wfx_iOS 2

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
libimobiledevice-glue, [OpenSSL](https://www.openssl.org/) and [SQLite](https://sqlite.org/).

## Requirements

- Total Commander (32- or 64-bit) on Windows 10 or 11
- Apple Mobile Device Support, with the "Apple Mobile Device Service" running. This is
  Apple's own driver and USB service. It is not included here (Apple does not allow
  redistribution); the setup program downloads it from Apple for you, see below.
- The device connected by cable, unlocked, and "Trust this computer" accepted

## Install

### Setup program (recommended)

Download `wfx_iOS_<version>_setup.exe` from [Releases](../../releases), or the latest build from
the [`dist` branch](../../tree/dist). Close Total Commander and run it. It:

1. finds Total Commander and installs both plugin files (32- and 64-bit); an existing
   wfx_iOS, including Domo's 1.3, is updated in place;
2. registers the plugin in `wincmd.ini`;
3. if Apple Mobile Device Support is missing, offers to download and install it from Apple
   through `winget` (`Apple.AppleMobileDeviceSupport`), and starts its service.

The setup program is not code-signed, so Windows SmartScreen may ask for confirmation.

### Manual

Open `wfx_iOS_<version>.zip` in Total Commander and accept the install prompt (no panel may be
inside the plugin at that moment). Install Apple's part yourself from an elevated
PowerShell, or install iTunes from apple.com:

```powershell
winget install -e --id Apple.AppleMobileDeviceSupport
```

Then open *Network Neighborhood* -> *iOS*.

## Folders

| Folder | Content |
|---|---|
| `Root` | whole file system (jailbroken devices with afc2 only; hidden otherwise) |
| `User` | media partition |
| `Photos` | `/DCIM` |
| `Processed photos` | `/DCIM`, but downloads fetch the edited version of a photo when one exists |
| `iCloud photos` | photos synced from iCloud Photos (`/PhotoData/CPLAssets`); shown only when the device has them |
| `Photos by month` | the whole photo library, one folder per month (`2026-11`), with original file names |
| `Applications` | documents of apps that enable file sharing, by app name |
| `Home screen` | the icons of the device: dock, pages and folders; move items to rearrange them |
| `Arrange home screen` | not a folder: Enter opens the window for arranging the icons |
| `Device information` | not a folder: Enter opens a window with everything the device reports |

With several devices connected, the root shows one folder per device (named after the
device) and each contains the folders above. Pick a device in the settings to work with
only that one.

`Settings` at the plugin root (Enter), or Alt+Enter on the root, opens the settings dialog:
device selection with device information, and the options below. Alt+Enter on a file shows
its properties.

## Arrange home screen

Enter on `Arrange home screen` (or Alt+Enter on the `Home screen` folder) opens a window made
for tidying up a device with many applications:

- left: the dock, every page, and the folders on each page;
- right: the icons of the place chosen on the left, in their order on the screen.

| Action | How |
|---|---|
| Move icons to another page or folder | select them (Ctrl / Shift for several), drag them onto it in the tree |
| Change the order | drag within the list; the icons land before the one they are dropped on |
| New folder | select the icons for it, `New folder`, type its name |
| Rename / ungroup a folder | open or select it, `Rename folder` / `Ungroup folder` |
| New page | `New page`, then drag icons onto it |
| Open a folder | double click it in the list |

The window works on a copy. `Apply to device` sends the arrangement in one step, after the
present one was saved to `%LOCALAPPDATA%\wfx_iOS\home-screen-<date>-<time>.plist`; the window
then shows what the device made of it. `Save to file` / `Open file` keep and bring back
arrangements, `Read from device` starts over. Empty folders and pages are not sent.

## Home screen as folders

The same layout is also available in the file panels, for quick moves without the window.
`Home screen` shows where the icons are on the device:

```
Home screen\Dock\Safari
Home screen\Page 01\Mail
Home screen\Page 02\Games\Chess        "Games" is a folder on the device
Home screen\layout.plist                the whole layout as one file
```

| Action | Result on the device |
|---|---|
| F6 an item to another page, the dock or a folder | the icon moves there (it is added at the end) |
| F6 an item to the page after the last one | a new page is made |
| F7 on a page | a new folder; it appears on the device with the first item moved into it |
| F6 / Shift+F6 a folder | moves it to another page / renames it |
| F8 on an empty folder | removes it (a folder disappears by itself when its last item leaves) |
| F5 `layout.plist` to the PC | saves the layout |
| F5 a saved `.plist` into `Home screen` | restores that layout |

Sorting by date shows the items in their order on the screen. Applications cannot be renamed
or deleted here, and folders cannot contain folders - as on the device.

Each change is sent to the device at once. The layout as it was before is saved first, to
`%LOCALAPPDATA%\wfx_iOS\home-screen-<date>-<time>.plist`; copying such a file into
`Home screen` puts everything back. Open `Home screen` again (or re-read with Ctrl+R there)
after moving icons on the device itself, so that the plugin works from the current layout.

Whether a device accepts a layout from a computer depends on the iOS version; when it refuses,
the plugin says so and nothing changes.

## Device information

Enter on `Device information` (or the button in the settings dialog) opens a window with
what the device reports about itself: name, model, system version and build, serial number,
UDID, ECID, IMEI / MEID / ICCID on cellular devices, Wi-Fi, Bluetooth and Ethernet addresses,
storage, battery, language and region, and after these every remaining value under the
device's own key names. A value can be copied, and the whole list copied or saved as text.

The device must be unlocked and trusted; otherwise only a few basic values are available.
Which values exist depends on the model and system version. These identifiers are shown
only in the window; the plugin does not write them to its log (the optional full protocol
trace, `Debug=1`, records all traffic and therefore does contain them).

## Photos by month

Pictures are stored on the device in technical folders (`100APPLE`, `group123`) under generated
names. `Photos by month` reads the Photos database of the device (`/PhotoData/Photos.sqlite`),
and shows every picture and video under the month it was taken, with its original name. The
database is copied to a temporary folder, read, and the copy is deleted at once. It is read
again at most every five minutes.

Only files that are really on the device are listed. With "Optimise Storage" turned on, many
originals exist only in iCloud and cannot be reached over USB. Files inside a month folder can
be copied and deleted like any other file; new files cannot be put there.

## Installing and removing applications

- **Install:** copy an `.ipa` file into `Applications`. The package must be signed for the
  device (developer, ad-hoc or enterprise signing); the device refuses anything else.
- **Uninstall:** delete the application's folder in `Applications`. The plugin asks for
  confirmation for each application. Answering *Yes* removes the application together with
  all its documents and data, and cannot be undone. Answering *No* leaves everything as it
  is, including the documents.

Deleting files or folders *inside* an application works as before and does not uninstall it.

## Languages

Dialogs and messages are available in English, Ukrainian, Russian and Polish. The plugin
follows the Windows display language; the settings dialog can override it. Folder names
(`User`, `Photos`, ...) stay in English, because they are part of the paths Total Commander
remembers. All texts are in `src/lang.c`; `tools/check_lang.py` checks that a translation
uses the same placeholders as the English text.

## Custom columns

The plugin offers extra file details to Total Commander: `Created`, `Type`, `Hard links`,
`Blocks`, `Link target`, `Device path` and, for applications, `Bundle id`. To show them, open
Configuration > Options > Custom columns, create a column set and pick fields of plugin `iOS`.

## Settings

Use the settings dialog, or edit section `[wfx_iOS]` in Total Commander's `fsplugin.ini`:

| Key | Meaning |
|---|---|
| `PreferredDevice=<UDID>` | use only this device (empty = all connected devices) |
| `ShowAllApps=1` | list all user apps and try their full container first (developer-signed apps only) |
| `AppNames=0` | list applications by bundle id instead of by name |
| `Language=en`/`uk`/`ru`/`pl` | language of dialogs and messages (default: same as Windows) |
| `LogFile=<path>` | write the trace to this file instead of `%TEMP%\wfx_iOS.log` |
| `Log=0` | turn off the short plugin log written to `%TEMP%\wfx_iOS.log` |
| `Debug=1` | add the full protocol trace to the log (large and slow; only for bug reports) |
| `AllowWiFi=1` | also use devices reachable over Wi-Fi sync when none is on USB (connecting can be slow) |

## Changes from 1.3

- Current libimobiledevice and OpenSSL 3; works with iOS / iPadOS 27
- Applications are listed by name instead of bundle id, and open on their `Documents` folder
- `iCloud photos` folder for photos stored outside `/DCIM`
- Edited-photo lookup also covers HEIC and PNG
- Resume for downloads and uploads; direct device-to-device copy
- `Root` is shown only when the device offers afc2
- USB only by default; Wi-Fi-synced devices are optional (`AllowWiFi=1`)
- Unplugging the device no longer hangs the plugin
- Error messages say which step failed, and a trace file is written
- Settings dialog with device selection, and a file properties dialog
- Icons: real home-screen icons for applications, and distinct icons for the top-level folders

## Troubleshooting

| Message | Meaning |
|---|---|
| Cannot reach the Apple Mobile Device Service | Apple Mobile Device Support is not installed or its service is stopped |
| The service is running but reports no connected device | cable, driver, or the device is locked |
| Tap "Trust" on the device | pairing has not been accepted yet |
| An application shows an error instead of `Documents` | the app does not share files; iOS grants no access to it |

Photos missing from `Photos`: only pictures taken on the device are in `/DCIM`. Pictures that
arrived through iCloud Photos are under `iCloud photos`, and with "Optimise Storage" turned on
many originals are not on the device at all.

For anything else, set `Debug=1`, reproduce the problem, and attach `%TEMP%\wfx_iOS.log` to an issue. The log contains device names,
identifiers and file names, so check it before posting.

## Build

Run `build.bat` (Visual Studio with C++ and git required); dependencies come from vcpkg.
The `ports/` folder holds a patched copy of vcpkg's libimobiledevice port and is required:
the stock port builds an AFC implementation that does not work under MSVC (wrong byte order)
and can spin forever on a closed connection.
Every push is built by GitHub Actions for x86 and x64, including the setup program
(`installer/wfx_iOS.iss`, Inno Setup); the result is published to the `dist` branch.

The version number is the single line in `version.txt`; see [CHANGELOG.md](CHANGELOG.md).

## Source layout

The plugin is plain C (C99 style), one module per job:

| File | Job |
|---|---|
| `src/plugin.c` | the functions Total Commander calls (`Fs...`) |
| `src/device.c` | device handle, lockdown, service start, AFC connections |
| `src/paths.c` | Total Commander path to device path, application names |
| `src/listing.c` | directory listings |
| `src/transfer.c` | download, upload, copy on the device |
| `src/apps.c` | installing and removing applications |
| `src/photos.c` | `Photos by month`, reads the device's photo database |
| `src/icons.c` | folder and application icons |
| `src/columns.c` | extra file details for custom columns |
| `src/dialogs.c` | settings dialog, file properties |
| `src/info.c` | device information window |
| `src/home.c` | home screen layout: reading, writing, backup, and as folders |
| `src/arrange.c` | the "Arrange home screen" window |
| `src/util.c` | strings, time, ini file, log file, error box |
| `src/lang.c` | all user-visible texts in four languages |
| `src/wfx_ios.h` | shared declarations and an overview |

## Status

Beta. Verified so far on one device (iPad, iPadOS 27.2, Windows, USB): browsing `User` and
`Photos`. Please report results with the iOS version and device model.

## License

LGPL-2.1-or-later, the same license as libimobiledevice. See [LICENSE](LICENSE).
