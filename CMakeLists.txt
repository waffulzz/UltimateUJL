cmake_minimum_required(VERSION 3.24)
project(UltimateUJL C CXX)

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
target_compile_options(runtime PRIVATE -Wall -Wno-unused-function -O2)

# Headless boot probe
add_executable(ujl_probe src/probe_main.c)
target_include_directories(ujl_probe PRIVATE src)
target_link_libraries(ujl_probe PRIVATE runtime)

# Dear ImGui (vendored)
set(IMGUI_DIR third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

# The game
find_package(OpenGL REQUIRED)
set(UJL_ICON_RC "")
if(WIN32)
  enable_language(RC)
  set(UJL_ICON_RC assets/icon/ujl.rc)   # exe icon (tools/make_icon.py builds ujl.ico from the user's art)
endif()
add_executable(ujl src/frontend/main.cpp src/frontend/discs.cpp src/frontend/gl_funcs.cpp src/frontend/hw_renderer.cpp ${UJL_ICON_RC})
target_link_libraries(ujl PRIVATE runtime imgui OpenGL::GL)
if(APPLE)
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
if(EXISTS "${RECOMP_DIR_US}/recomp_files.cmake")
  set(_saved_sources ${RECOMP_SOURCES})
  include("${RECOMP_DIR_US}/recomp_files.cmake")
  add_library(recomp_us STATIC ${RECOMP_SOURCES})
  set(RECOMP_SOURCES ${_saved_sources})
  target_include_directories(recomp_us PUBLIC src/runtime "${RECOMP_DIR_US}")
  target_compile_options(recomp_us PRIVATE -w -O2 -fno-strict-aliasing)
  add_library(runtime_us STATIC ${RUNTIME_SOURCES})
  target_include_directories(runtime_us PUBLIC src/runtime "${RECOMP_DIR_US}")
  target_link_libraries(runtime_us PUBLIC recomp_us SDL3::SDL3-static)
  target_compile_options(runtime_us PRIVATE -w -O2)
  add_executable(ujl_probe_us src/probe_main.c)
  target_include_directories(ujl_probe_us PRIVATE src)
  target_link_libraries(ujl_probe_us PRIVATE runtime_us)
  if(MINGW)
    target_link_options(ujl_probe_us PRIVATE -static)
  endif()
endif()

# Controller diagnostics (console)
add_executable(ujl_padtest src/tools/padtest.c)
target_link_libraries(ujl_padtest PRIVATE SDL3::SDL3-static)
if(MINGW)
  target_link_options(ujl_padtest PRIVATE -static)
endif()
