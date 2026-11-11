include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
    URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.63.tar.gz
    URL_HASH SHA256=5b8dac0608f49498fe8e95f562fe6bcf0bc781e561f137348a8c251d06a88b6f)
FetchContent_MakeAvailable(openxr)
