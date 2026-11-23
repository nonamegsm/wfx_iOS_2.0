cmake_minimum_required(VERSION 3.21)
project(wfx_iOS VERSION 2.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the plugin is one self-contained file (matches the *-windows-static triplets).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(unofficial-libimobiledevice CONFIG REQUIRED)

add_library(wfx_iOS SHARED src/wfx_ios.cpp src/fsplugin.h src/resource.h src/wfx_ios.rc wfx_ios.def)
target_compile_definitions(wfx_iOS PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(wfx_iOS PRIVATE unofficial::libimobiledevice::libimobiledevice shlwapi shell32 gdiplus ole32 ws2_32 crypt32 iphlpapi)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(wfx_iOS PROPERTIES SUFFIX ".wfx64")
else()
    set_target_properties(wfx_iOS PROPERTIES SUFFIX ".wfx")
endif()
set_target_properties(wfx_iOS PROPERTIES PREFIX "" OUTPUT_NAME "wfx_iOS")
