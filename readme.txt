wfx_iOS 2.0 - iOS file system plugin for Total Commander
=========================================================

An update of the wfx_iOS 1.3 plugin (2017) by Domo, the original author.
The idea, folder layout and features are his work. This version only brings
the plugin onto current libimobiledevice and OpenSSL 3, so that it can pair
with and browse modern iOS / iPadOS devices. It is not an official release
by the original author.

Requirements
  - Apple Mobile Device Support (installed with iTunes or the "Apple Devices" app);
    its "Apple Mobile Device Service" must be running.
  - The device must be unlocked and "Trust this computer" accepted.

Folders
  Root              whole file system (jailbroken devices with afc2 only; hidden otherwise)
  User              media partition (AFC)
  Photos            /DCIM
  Processed photos  /DCIM, but downloads fetch the edited version of a photo when one exists
  Applications      documents of apps that enable file sharing, by bundle id

Alt+Enter on the plugin root shows device information and lets you switch the
preferred device when several are connected. Alt+Enter on a file shows its properties.

Settings (section [wfx_iOS] in Total Commander's fsplugin.ini)
  PreferredDevice=<UDID>   use only this device (empty = any connected device)
  ShowAllApps=1            list all user apps and try their full container first
                           (only works for developer-signed apps)

Build
  Run build.bat (Visual Studio C++ + git required). Dependencies come from vcpkg.

Based on these open-source libraries:
  libimobiledevice, libusbmuxd, libplist, libimobiledevice-glue (https://libimobiledevice.org/)
  OpenSSL (https://www.openssl.org/)
