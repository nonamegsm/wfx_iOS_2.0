cmake_minimum_required(VERSION 3.21)

# The version lives in version.txt ("major.minor"); bump it there only.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" WFX_VERSION LIMIT_COUNT 1)
string(STRIP "${WFX_VERSION}" WFX_VERSION)
string(REPLACE "." ";" WFX_VERSION_PARTS "${WFX_VERSION}")
list(GET WFX_VERSION_PARTS 0 WFX_VERSION_MAJOR)
list(GET WFX_VERSION_PARTS 1 WFX_VERSION_MINOR)

project(wfx_iOS VERSION ${WFX_VERSION} LANGUAGES C RC)

# Static CRT so the plugin is one self-contained file (matches the *-windows-static triplets).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(unofficial-libimobiledevice CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)       # reads the device's photo database

add_library(wfx_iOS SHARED
    src/plugin.c
    src/device.c
    src/paths.c
    src/listing.c
    src/transfer.c
    src/photos.c
    src/icons.c
    src/columns.c
    src/dialogs.c
    src/util.c
    src/wfx_ios.h
    src/fsplugin.h
    src/resource.h
    src/wfx_ios.rc
    wfx_ios.def)

target_compile_definitions(wfx_iOS PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
    WFX_VERSION_STR="${WFX_VERSION}"
    WFX_VERSION_MAJOR=${WFX_VERSION_MAJOR}
    WFX_VERSION_MINOR=${WFX_VERSION_MINOR})

if(MSVC)
    target_compile_options(wfx_iOS PRIVATE /W3)
endif()

target_link_libraries(wfx_iOS PRIVATE
    unofficial::libimobiledevice::libimobiledevice
    unofficial::sqlite3::sqlite3
    shlwapi shell32 ole32 windowscodecs uuid ws2_32 crypt32 iphlpapi)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(wfx_iOS PROPERTIES SUFFIX ".wfx64")
else()
    set_target_properties(wfx_iOS PROPERTIES SUFFIX ".wfx")
endif()
set_target_properties(wfx_iOS PROPERTIES PREFIX "" OUTPUT_NAME "wfx_iOS")
