cmake_minimum_required(VERSION 3.20)
project(KakutoChojin-recomp LANGUAGES C CXX)

# Two builds from the same sources:
#   32-bit x86 (Windows, MSVC): kt_loader + kt_host.dll run the XBE's own
#               machine code (mixed mode with optional lifted code in kt_lifted.dll).
#   64-bit (Windows/MSVC, or other systems with GCC/Clang): kt_run (KakutoChojin.exe) runs the
#               title only as lifted C (KT_LIFTED_DIR is required) in a 4 GB
#               guest address space.

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Lifted title code (tools/lift output; generated per title, never committed):
#   cmake -DKT_LIFTED_DIR=<dir with lifted_*.c> ...
set(KT_LIFTED_DIR "" CACHE PATH "Directory with tools/lift output")
# Each generated file needs a lot of compiler memory and time; compiling
# them all at once (one per core) makes the machine unusable.
set(KT_LIFTED_JOBS 2 CACHE STRING "Parallel compile jobs for the lifted code")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS kt_lifted_pool=${KT_LIFTED_JOBS})

# SDL3 (zlib license) for the window, input and audio. Point SDL3_DIR at an
# SDL3 CMake package (or install SDL3 system-wide), or on Windows unpack the
# SDL3 VC development release into third_party/ (tools/fetch_sdl3.ps1 does this).
file(GLOB KT_SDL3_CANDIDATES ${CMAKE_SOURCE_DIR}/third_party/SDL3-*/cmake)
# tools/setup.ps1 puts the MinGW package here.
list(APPEND KT_SDL3_CANDIDATES ${CMAKE_SOURCE_DIR}/deps/SDL3/x86_64-w64-mingw32/lib/cmake/SDL3)
find_package(SDL3 REQUIRED CONFIG PATHS ${KT_SDL3_CANDIDATES})

# Runtime that replaces the Xbox kernel and hardware libraries.
set(KT_HOST_SOURCES
  src/host/manifest.cpp
  src/host/kpcr.cpp
  src/host/kernel.cpp
  src/host/kernel_io.cpp
  src/host/kernel_timer.cpp
  src/host/ob.cpp
  src/host/os.cpp
  src/host/hostfs.cpp
  src/host/hle.cpp
  src/host/xapi/input.cpp
  src/host/platform/platform_sdl.cpp
  src/host/platform/launcher.cpp
  src/host/audio/mixer.cpp
  src/host/gpu/gpu.cpp
  src/host/gpu/gpu_gl.cpp
  src/host/dsound/dsound.cpp
  src/host/dsound/adpcm.cpp
  src/host/d3d/device.cpp
  src/host/d3d/resource.cpp
  src/host/d3d/state.cpp
  src/host/d3d/shader.cpp
  src/host/d3d/ffgen.cpp
  src/host/d3d/overlay.cpp
  src/host/d3d/vsh.cpp
  src/host/d3d/psh.cpp
  src/host/crash.cpp
  src/host/log.cpp
  src/host/guest_mem.cpp
)
set(KT_HOST_DEFINES KT_GPU_GL)
set(KT_HOST_LIBS SDL3::SDL3)
if(WIN32)
  list(APPEND KT_HOST_SOURCES src/host/gpu/gpu_d3d11.cpp)
  list(APPEND KT_HOST_DEFINES UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  list(APPEND KT_HOST_LIBS shell32 d3d11 dxgi d3dcompiler ntdll winmm)
else()
  find_package(Threads REQUIRED)
  list(APPEND KT_HOST_LIBS Threads::Threads ${CMAKE_DL_LIBS})
endif()

# Copies SDL3.dll next to a runtime binary (Windows; elsewhere SDL3 is a system library).
function(kt_copy_sdl3 target)
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:${target}>)
  endif()
endfunction()

if(MSVC)
  # Generated code: unreferenced labels/locals are expected.
  set(KT_LIFTED_OPTIONS /O2 /W1 /wd4102 /wd4101 /wd4189 /bigobj)
  set(KT_HOST_OPTIONS /W3 /utf-8)
  set(KT_MAP_OPTION /MAP)
else()
  # Lifted code reinterprets guest memory and relies on wrapping arithmetic.
  set(KT_LIFTED_OPTIONS -O2 -fno-strict-aliasing -fwrapv -w)
  set(KT_HOST_OPTIONS -Wall -Wno-unused-function -Wno-unused-variable -fno-strict-aliasing)
  set(KT_MAP_OPTION)
  if(MINGW)
    # One self-contained exe (plus SDL3.dll): no libc++ / libunwind / pthread DLLs.
    set(KT_MAP_OPTION -static)
  endif()
endif()

if(KT_LIFTED_DIR)
  file(GLOB KT_LIFTED_SRC ${KT_LIFTED_DIR}/*.c)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  if(NOT KT_LIFTED_DIR)
    message(FATAL_ERROR "the 64-bit build runs only lifted code: set -DKT_LIFTED_DIR=<tools/lift output>")
  endif()
  add_library(kt_lifted_obj OBJECT ${KT_LIFTED_SRC})
  set_target_properties(kt_lifted_obj PROPERTIES JOB_POOL_COMPILE kt_lifted_pool)
  target_include_directories(kt_lifted_obj PRIVATE src/lift ${KT_LIFTED_DIR})
  target_compile_options(kt_lifted_obj PRIVATE ${KT_LIFTED_OPTIONS})

  add_executable(kt_run ${KT_HOST_SOURCES} src/host/runtime64.cpp $<TARGET_OBJECTS:kt_lifted_obj>)
  target_compile_features(kt_run PRIVATE cxx_std_17)
  target_compile_definitions(kt_run PRIVATE ${KT_HOST_DEFINES})
  target_compile_options(kt_run PRIVATE ${KT_HOST_OPTIONS})
  target_link_libraries(kt_run PRIVATE ${KT_HOST_LIBS})
  target_link_options(kt_run PRIVATE ${KT_MAP_OPTION})
  set_target_properties(kt_run PROPERTIES OUTPUT_NAME KakutoChojin)
  kt_copy_sdl3(kt_run)
  return()
endif()

if(NOT (WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4))
  message(FATAL_ERROR "unsupported target: the 32-bit build needs the x86 MSVC toolchain; other systems build 64-bit")
endif()

add_library(kt_host SHARED ${KT_HOST_SOURCES} src/host/main.cpp src/host/lift.cpp src/host/guest_call.cpp)
target_compile_features(kt_host PRIVATE cxx_std_17)
target_compile_definitions(kt_host PRIVATE ${KT_HOST_DEFINES})
target_compile_options(kt_host PRIVATE ${KT_HOST_OPTIONS})
target_link_libraries(kt_host PRIVATE ${KT_HOST_LIBS})
# Title SEH handlers live in the XBE image; keep host modules SafeSEH-agnostic.
target_link_options(kt_host PRIVATE /SAFESEH:NO /MAP)
kt_copy_sdl3(kt_host)

if(KT_LIFTED_DIR)
  add_library(kt_lifted SHARED ${KT_LIFTED_SRC})
  set_target_properties(kt_lifted PROPERTIES JOB_POOL_COMPILE kt_lifted_pool)
  target_include_directories(kt_lifted PRIVATE src/lift ${KT_LIFTED_DIR})
  target_link_libraries(kt_lifted PRIVATE kt_host)
  target_compile_definitions(kt_lifted PRIVATE KT_VERIFY)
  target_compile_options(kt_lifted PRIVATE ${KT_LIFTED_OPTIONS})
  target_link_options(kt_lifted PRIVATE /SAFESEH:NO /MAP)
endif()

# Placeholder exe pinned at the XBE base address (see loader_main.c).
add_executable(kt_loader src/loader/loader_main.c)
target_compile_options(kt_loader PRIVATE /GS-)
target_link_libraries(kt_loader PRIVATE kt_host kernel32)
target_link_options(kt_loader PRIVATE
  /BASE:0x10000 /FIXED /DYNAMICBASE:NO /SAFESEH:NO /NXCOMPAT:NO
  /NODEFAULTLIB /ENTRY:LoaderEntry /SUBSYSTEM:CONSOLE)
