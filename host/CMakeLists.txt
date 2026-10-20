cmake_minimum_required(VERSION 3.25)
# One version for the whole package: the plugin's (project() in ../CMakeLists.txt).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../CMakeLists.txt" RFGPHYS_PROJECT_LINE REGEX "^project\\(rfgphys VERSION ")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" RFGPHYS_VERSION "${RFGPHYS_PROJECT_LINE}")
project(rfgphysx_host VERSION ${RFGPHYS_VERSION} LANGUAGES CXX)

# The 64-bit side of the PhysX experiment (research/docs/06): the PhysX host process the plugin starts, and the
# standalone pile benchmark.
# PhysX comes from tools/build_physx.cmd (static libraries with the static CRT, plus PhysXGpu.dll).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "PhysX GPU is 64-bit only: configure with -A x64")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # matches PhysX's /MT libraries
set(PHYSX_INSTALL "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/physx-build/release-gpu/install")
if(NOT EXISTS "${PHYSX_INSTALL}/lib/PhysX_static_64.lib")
    message(FATAL_ERROR "PhysX not built: run tools/build_physx.cmd (through the shared queue)")
endif()

add_library(physx_sdk INTERFACE)
target_include_directories(physx_sdk INTERFACE "${PHYSX_INSTALL}/include")
target_compile_definitions(physx_sdk INTERFACE PX_PHYSX_STATIC_LIB)  # PxConfig.h is not always included first
target_link_libraries(physx_sdk INTERFACE
    "${PHYSX_INSTALL}/lib/PhysXExtensions_static_64.lib"
    "${PHYSX_INSTALL}/lib/PhysX_static_64.lib"
    "${PHYSX_INSTALL}/lib/PhysXPvdSDK_static_64.lib"
    "${PHYSX_INSTALL}/lib/PhysXCommon_static_64.lib"
    "${PHYSX_INSTALL}/lib/PhysXFoundation_static_64.lib")

add_executable(rfgphysx_bench bench/rfgphysx_bench.cpp)
set_target_properties(rfgphysx_bench PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(rfgphysx_bench PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(rfgphysx_bench PRIVATE /W3 /utf-8 /Zi)
target_link_options(rfgphysx_bench PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
target_link_libraries(rfgphysx_bench PRIVATE physx_sdk)
# PhysX loads its GPU module at run time from the executable's folder, as PhysXGpu_64.dll (the build names it
# PhysXGpu.dll). Both executables are built into the same folder; one target copies it there.
add_custom_target(physx_gpu_dll ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/$<CONFIG>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PHYSX_INSTALL}/bin/PhysXGpu.dll" "${CMAKE_BINARY_DIR}/$<CONFIG>/PhysXGpu_64.dll")
add_dependencies(rfgphysx_bench physx_gpu_dll)

# The host process (research/docs/06 §3, §13): rfgphys.asi starts it and talks to it through shared memory
# (src/shared/rfgphysx_ipc.h, compiled by both sides).
add_executable(rfgphysx_host src/host_main.cpp)
set_target_properties(rfgphysx_host PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(rfgphysx_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/shared")
target_compile_definitions(rfgphysx_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(rfgphysx_host PRIVATE /W4 /permissive- /utf-8 /Zi)
target_link_options(rfgphysx_host PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
target_link_libraries(rfgphysx_host PRIVATE physx_sdk dbghelp)
add_dependencies(rfgphysx_host physx_gpu_dll)
# Version resource (../src/shared/version.rc.in).
set(RC_DESCRIPTION "rfgphys PhysX host - debris physics for Red Faction Guerrilla Re-Mars-tered")
set(RC_INTERNAL_NAME "rfgphysx_host")
set(RC_FILENAME "rfgphysx_host.exe")
set(RC_FILE_TYPE "VFT_APP")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/version.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/rfgphysx_host_version.rc"
               @ONLY)
target_sources(rfgphysx_host PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/rfgphysx_host_version.rc")
