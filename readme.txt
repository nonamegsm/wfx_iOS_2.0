wfx_iOS 2 - iOS file system plugin for Total Commander
=======================================================

An update of the wfx_iOS 1.3 plugin (2017) by Domo, the original author.
The idea, folder layout and features are his work. This version only brings
the plugin onto current libimobiledevice and OpenSSL 3, so that it can pair
with and browse modern iOS / iPadOS devices. It is not an official release
by the original author.

Requirements
  - Apple Mobile Device Support (Apple's driver and USB service); its
    "Apple Mobile Device Service" must be running. The setup program offers to
    download it from Apple. Manual install, from an elevated PowerShell:
      winget install -e --id Apple.AppleMobileDeviceSupport
  - The device connected by cable, unlocked, and "Trust this computer" accepted.

Folders
  Root              whole file system (jailbroken devices with afc2 only; hidden otherwise)
  User              media partition (AFC)
  Photos            /DCIM
  Processed photos  /DCIM, but downloads fetch the edited version of a photo when one exists
  iCloud photos     photos synced from iCloud Photos (shown only when present)
  Photos by month   the whole photo library, one folder per month
  Applications      documents of apps that enable file sharing, by app name
  Home screen       the icons of the device: Dock, Page 01, ... and folders.
                    F6 moves an item to another page or folder, F7 makes a
                    folder. layout.plist is the whole layout: copy it out to
                    save, copy a saved one in to restore. A backup is written
                    to %LOCALAPPDATA%\wfx_iOS before every change.
  Arrange home screen  (Enter) window for arranging the icons: drag them
                    between pages and folders, then "Apply to device"
  Device information  (Enter) serial number, IMEI, addresses, storage, battery
                    and everything else the device reports; copy or save as text

"Settings" at the plugin root (Enter), or Alt+Enter on the root, opens the
settings dialog. Alt+Enter on a file shows its properties.

Settings (section [wfx_iOS] in Total Commander's fsplugin.ini)
  PreferredDevice=<UDID>   use only this device (empty = any connected device)
  ShowAllApps=1            list all user apps and try their full container first
                           (only works for developer-signed apps)
  AppNames=0               list applications by bundle id instead of by name
  Language=en|uk|ru|pl     language (default: same as Windows)
  LogFile=<path>           trace file (default %TEMP%\wfx_iOS.log)
  Log=0                    turn the short plugin log off
  Debug=1                  add the full protocol trace (large, slow)
  AllowWiFi=1              also use devices reachable over Wi-Fi sync

Build
  Run build.bat (Visual Studio C++ + git required). Dependencies come from vcpkg.

Based on these open-source libraries:
  libimobiledevice, libusbmuxd, libplist, libimobiledevice-glue (https://libimobiledevice.org/)
  OpenSSL (https://www.openssl.org/)
  SQLite (https://sqlite.org/)
