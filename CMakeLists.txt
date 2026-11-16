cmake_minimum_required(VERSION 3.24)
project(UltimateUJL C CXX)

# iOS / iPadOS: configure with -DCMAKE_SYSTEM_NAME=iOS (see docs/ARCHITECTURE.md, "iOS"). Only the game itself is
# built there (an .app bundle); the probe and pad test are desktop console tools.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(UJL_IOS ON)
  enable_language(OBJCXX)
else()
  set(UJL_IOS OFF)
endif()

# Native replay dialogs are available on desktop; other targets retain card replays.
if(UJL_IOS)
  set(_replay_files_default OFF)
else()
  set(_replay_files_default ON)
endif()
option(UJL_REPLAY_FILES "Enable host replay files and native file dialogs" ${_replay_files_default})
if(UJL_IOS AND UJL_REPLAY_FILES)
  message(FATAL_ERROR "UJL_REPLAY_FILES needs a save-file picker; leave it OFF on iOS")
endif()

# Without a build type CMake adds no optimization flags: the frontend (hardware renderer replay), ImGui and SDL
# (audio resampling, input) were built at -O0. recomp/runtime set -O2 themselves; default everything else to Release.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (default Release)" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Clang is the supported compiler on every platform (llvm-mingw on Windows, Apple clang on macOS, clang on
# Linux): the generated code is tens of MB of C with huge functions and is only tested with clang.
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(WARNING "Compiler ${CMAKE_C_COMPILER_ID} is untested; configure with -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++")
endif()

# Generated recompiled code (produced from the user's own disc by tools/prepare.sh)
set(RECOMP_DIR "${CMAKE_SOURCE_DIR}/build/gen_jp" CACHE PATH "Directory with generated recomp_*.c")
if(NOT EXISTS "${RECOMP_DIR}/recomp_files.cmake")
  message(FATAL_ERROR "No generated code in ${RECOMP_DIR}. Run tools/prepare.sh first.")
endif()
include("${RECOMP_DIR}/recomp_files.cmake")

# SDL3 (vendored)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)

# Recompiled game code: big, machine-generated. Keep warnings quiet.
add_library(recomp STATIC ${RECOMP_SOURCES})
target_include_directories(recomp PUBLIC src/runtime "${RECOMP_DIR}")
target_compile_options(recomp PRIVATE -w -O2 -fno-strict-aliasing)

set(RUNTIME_SOURCES
  src/runtime/mem.c
  src/runtime/cpu.c
  src/runtime/bios.c
  src/runtime/gte.c
  src/runtime/gpu.c
  src/runtime/gpu_soft.c
  src/runtime/gpu_rec.c
  src/runtime/cdrom.c
  src/runtime/disc.c
  src/runtime/patch.c
  src/runtime/censor.c
  src/runtime/autoplay.c
  src/runtime/replay_files.c
  src/runtime/score_delta.c
  src/runtime/language.c
  src/runtime/title_logo.c src/runtime/button_swap.c
  src/runtime/spu.c
  src/runtime/fbstream.c
  src/runtime/mdec.c
  src/runtime/sio.c
  src/runtime/memcard.c
  src/runtime/unlock.c
  src/runtime/boot.c
)
add_library(runtime STATIC ${RUNTIME_SOURCES})
target_include_directories(runtime PUBLIC src/runtime "${RECOMP_DIR}")
target_link_libraries(runtime PUBLIC recomp SDL3::SDL3-static)
# Generated functions call native hooks in otherwise unreferenced runtime objects. Declare the real static
# library cycle so CMake repeats both archives on ELF linkers instead of losing those hook definitions.
target_link_libraries(recomp PRIVATE runtime)
target_compile_definitions(runtime PUBLIC UJL_REPLAY_FILES=$<BOOL:${UJL_REPLAY_FILES}>)
target_compile_options(runtime PRIVATE -Wall -Wno-unused-function -O2)

# Headless boot probe
if(NOT UJL_IOS)
  add_executable(ujl_probe src/probe_main.c)
  target_include_directories(ujl_probe PRIVATE src)
  target_link_libraries(ujl_probe PRIVATE runtime)
  # Focused native HUD/trace checks; synthetic RAM only, no disc assets.
  add_executable(ujl_score_test EXCLUDE_FROM_ALL tests/score_delta_test.c src/runtime/score_delta.c)
  target_include_directories(ujl_score_test PRIVATE src/runtime)
  target_link_libraries(ujl_score_test PRIVATE SDL3::SDL3-static)
  target_compile_options(ujl_score_test PRIVATE -UNDEBUG)
endif()

# Dear ImGui (vendored)
set(IMGUI_DIR third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

# The game
set(UJL_FRONTEND_SOURCES src/frontend/main.cpp src/frontend/discs.cpp src/frontend/gl_funcs.cpp src/frontend/hw_renderer.cpp
  src/frontend/touch_pad.cpp)
if(UJL_IOS)
  # OpenGL ES 3.0 (UIKit EAGL via SDL), the same renderer with GLSL ES shaders (hw_renderer.cpp picks them at run time)
  set(UJL_IOS_BUNDLE_ID "io.github.waffulzz.UltimateUJL" CACHE STRING "iOS bundle identifier")
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(UJL_IOS_PLATFORM iPhoneSimulator)   # Info.plist CFBundleSupportedPlatforms
  else()
    set(UJL_IOS_PLATFORM iPhoneOS)
  endif()
  # home screen icon from the project's own icon art, at the names Info.plist lists
  set(UJL_IOS_ICONS "${CMAKE_BINARY_DIR}/ios_res/AppIcon60x60@2x.png" "${CMAKE_BINARY_DIR}/ios_res/AppIcon60x60@3x.png")
  foreach(icon ${UJL_IOS_ICONS})
    configure_file(assets/icon/ujl_256.png "${icon}" COPYONLY)
  endforeach()
  add_executable(ujl MACOSX_BUNDLE ${UJL_FRONTEND_SOURCES} src/frontend/platform_ios.mm ${UJL_IOS_ICONS})
  set_source_files_properties(${UJL_IOS_ICONS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
  target_compile_definitions(ujl PRIVATE GLES_SILENCE_DEPRECATION)
  target_compile_definitions(imgui PRIVATE GLES_SILENCE_DEPRECATION)
  target_compile_options(ujl PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  target_link_libraries(ujl PRIVATE runtime imgui "-framework OpenGLES" "-framework UIKit" "-framework Foundation"
    "-framework UniformTypeIdentifiers" "-framework QuartzCore")
  set_target_properties(ujl PROPERTIES
    OUTPUT_NAME "UltimateUJL"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/assets/ios/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${UJL_IOS_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "Ultimate UJL"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${UJL_IOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
  if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
    # Ninja/Makefiles: the linker signs arm64 binaries ad hoc, but a bundle needs its own signature to install.
    # Ad hoc is enough for the simulator; a device needs a real identity (UJL_IOS_SIGN_IDENTITY) or the Xcode generator.
    # Empty = leave it unsigned (an IPA for sideloading tools that sign it themselves: tools/build_ios.sh --ipa).
    set(UJL_IOS_SIGN_IDENTITY "-" CACHE STRING "codesign identity for the .app (- = ad hoc, simulator only; empty = unsigned)")
    if(NOT UJL_IOS_SIGN_IDENTITY STREQUAL "")
      add_custom_command(TARGET ujl POST_BUILD
        COMMAND codesign --force --sign "${UJL_IOS_SIGN_IDENTITY}" --timestamp=none "$<TARGET_BUNDLE_DIR:ujl>"
        VERBATIM)
    endif()
  endif()
else()
  find_package(OpenGL REQUIRED)
  set(UJL_ICON_RC "")
  if(WIN32)
    enable_language(RC)
    set(UJL_ICON_RC assets/icon/ujl.rc)   # exe icon (tools/make_icon.py builds ujl.ico from the user's art)
  endif()
  add_executable(ujl ${UJL_FRONTEND_SOURCES} ${UJL_ICON_RC})
  target_link_libraries(ujl PRIVATE runtime imgui OpenGL::GL)
endif()
if(APPLE AND NOT UJL_IOS)
  # macOS ships OpenGL 4.1 core, deprecated but working; keep the build log quiet about it
  target_compile_definitions(ujl PRIVATE GL_SILENCE_DEPRECATION)
  target_compile_definitions(imgui PRIVATE GL_SILENCE_DEPRECATION)
endif()
if(MINGW)
  # self-contained exe: no libc++/libunwind DLLs next to it
  target_link_options(ujl PRIVATE -static)
  target_link_options(ujl_probe PRIVATE -static)
endif()

# USA analysis probe (internal tool, not shipped): build/gen_us from tools/prepare_us.sh
set(RECOMP_DIR_US "${CMAKE_SOURCE_DIR}/build/gen_us")
if(EXISTS "${RECOMP_DIR_US}/recomp_files.cmake" AND NOT UJL_IOS)
  set(_saved_sources ${RECOMP_SOURCES})
  include("${RECOMP_DIR_US}/recomp_files.cmake")
  add_library(recomp_us STATIC ${RECOMP_SOURCES})
  set(RECOMP_SOURCES ${_saved_sources})
  target_include_directories(recomp_us PUBLIC src/runtime "${RECOMP_DIR_US}")
  target_compile_options(recomp_us PRIVATE -w -O2 -fno-strict-aliasing)
  add_library(runtime_us STATIC ${RUNTIME_SOURCES})
  target_include_directories(runtime_us PUBLIC src/runtime "${RECOMP_DIR_US}")
  target_link_libraries(runtime_us PUBLIC recomp_us SDL3::SDL3-static)
  target_link_libraries(recomp_us PRIVATE runtime_us)
  target_compile_options(runtime_us PRIVATE -w -O2)
  add_executable(ujl_probe_us src/probe_main.c)
  target_include_directories(ujl_probe_us PRIVATE src)
  target_link_libraries(ujl_probe_us PRIVATE runtime_us)
  if(MINGW)
    target_link_options(ujl_probe_us PRIVATE -static)
  endif()
endif()

# Controller diagnostics (console)
if(NOT UJL_IOS)
  add_executable(ujl_padtest src/tools/padtest.c)
  target_link_libraries(ujl_padtest PRIVATE SDL3::SDL3-static)
  if(MINGW)
    target_link_options(ujl_padtest PRIVATE -static)
  endif()
endif()
