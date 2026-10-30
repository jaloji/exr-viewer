cmake_minimum_required(VERSION 3.24)
project(exr-viewer VERSION 1.0.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "exr-viewer builds a Windows WIC codec and needs Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL is loaded into Explorer, so it must not need a VC++ redistributable.
# The policy default makes the dependencies (older cmake_minimum_required) follow it too.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# All binaries land in one folder next to each other.
foreach(kind RUNTIME LIBRARY ARCHIVE)
    set(CMAKE_${kind}_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${config} CONFIG)
        set(CMAKE_${kind}_OUTPUT_DIRECTORY_${CONFIG} ${CMAKE_BINARY_DIR}/bin)
    endforeach()
endforeach()

# Some dependencies put __FILE__ in their messages: keep the local build path out of the binaries.
if(MSVC)
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/" source_prefix)
    add_compile_options("/d1trimfile:${source_prefix}")
endif()

# OpenEXR (Academy Software Foundation, BSD-3-Clause) with Imath (BSD-3-Clause), built as static
# libraries. libdeflate, zstd and OpenJPH come vendored inside the OpenEXR sources.
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_PKG_CONFIG OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_ENABLE_THREADING OFF CACHE BOOL "" FORCE)  # no worker threads inside Explorer
set(OPENEXR_FORCE_INTERNAL_IMATH ON CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_DEFLATE ON CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_OPENJPH ON CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_ZSTD ON CACHE BOOL "" FORCE)
set(OPENEXR_IMATH_TAG 4188e70c79ad47794765089c427cc36ceadf318b CACHE STRING "" FORCE)  # Imath v3.2.3
FetchContent_Declare(OpenEXR
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openexr.git
    GIT_TAG 69b2604fc76e370615438bdc8d2cd95b9349c12e  # v3.5.2
    GIT_SHALLOW FALSE
    EXCLUDE_FROM_ALL)  # keeps its headers and .lib out of the release package
FetchContent_MakeAvailable(OpenEXR)
FetchContent_GetProperties(Imath SOURCE_DIR imath_SOURCE_DIR)  # fetched by OpenEXR's own scope

function(exr_warnings target)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    endif()
endfunction()

# OpenEXR reader and tone mapping shared by every component.
add_library(exr_core STATIC src/core/exr.cpp src/core/exr.h)
target_include_directories(exr_core PUBLIC src/core)
target_link_libraries(exr_core PRIVATE OpenEXR::OpenEXR)
exr_warnings(exr_core)

# Windows Imaging Component codec: Explorer thumbnails, preview pane, photo viewers.
add_library(ExrWic SHARED src/wic/exrwic.cpp src/wic/exrwic.def)
target_link_libraries(ExrWic PRIVATE exr_core windowscodecs ole32 advapi32 shell32)
exr_warnings(ExrWic)

# Command-line tools.
add_executable(exr2png tools/exr2png.cpp)
target_link_libraries(exr2png PRIVATE exr_core)
exr_warnings(exr2png)

# Test helper (not packaged): writes OpenEXR variants (merged deep passes, other compressions, multipart).
add_executable(exrtest tools/exrtest.cpp)
target_link_libraries(exrtest PRIVATE OpenEXR::OpenEXR)
exr_warnings(exrtest)

add_executable(wictest tools/wictest.cpp)
target_link_libraries(wictest PRIVATE windowscodecs ole32)
exr_warnings(wictest)

# Release package (cmake --install build --prefix dist).
install(TARGETS ExrWic exr2png wictest RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES scripts/install.bat scripts/uninstall.bat scripts/clear-thumbnails.bat README.md LICENSE
              THIRD_PARTY_NOTICES.md DESTINATION .)
# The libraries are linked statically: their notices ship with the binaries.
install(FILES ${openexr_SOURCE_DIR}/LICENSE.md DESTINATION licenses RENAME OpenEXR-LICENSE.md)
install(FILES ${imath_SOURCE_DIR}/LICENSE.md DESTINATION licenses RENAME Imath-LICENSE.md)
install(FILES ${openexr_SOURCE_DIR}/external/deflate/COPYING DESTINATION licenses RENAME libdeflate-LICENSE.txt)
install(FILES ${openexr_SOURCE_DIR}/external/zstd/LICENSE DESTINATION licenses RENAME zstd-LICENSE.txt)
install(FILES ${openexr_SOURCE_DIR}/external/OpenJPH/LICENSE DESTINATION licenses RENAME OpenJPH-LICENSE.txt)
