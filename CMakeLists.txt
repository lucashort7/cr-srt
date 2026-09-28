# cr-srt — built with MinGW-w64 g++ (C++20) and Ninja; build.bat sets the toolchain up. Two static libraries from
# vendor/ and one DLL linked fully static, so the game needs nothing beside it.
cmake_minimum_required(VERSION 3.25)
project(cr-srt LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# Dear ImGui: the core and the DX12 backend (vendor/imgui/VENDORED.md). IMGUI_USER_CONFIG reaches every file that
# includes imgui.h, ours and ImGui's, through the PUBLIC definition.
add_library(imgui STATIC
  vendor/imgui/imgui.cpp
  vendor/imgui/imgui_draw.cpp
  vendor/imgui/imgui_tables.cpp
  vendor/imgui/imgui_widgets.cpp
  vendor/imgui/backends/imgui_impl_dx12.cpp)
target_include_directories(imgui PUBLIC vendor/imgui vendor/imgui/backends src)
target_compile_definitions(imgui PUBLIC IMGUI_USER_CONFIG="imgui_user_config.h")

# MinHook, x64 only (vendor/minhook/VENDORED.md).
add_library(minhook STATIC
  vendor/minhook/buffer.c
  vendor/minhook/hook.c
  vendor/minhook/trampoline.c
  vendor/minhook/hde/hde64.c)
target_include_directories(minhook PUBLIC vendor/minhook/include)

add_library(cr-srt SHARED
  src/dllmain.cpp
  src/log.cpp
  src/memory.cpp
  src/pe.cpp
  src/cache.cpp
  src/solve.cpp
  src/ecs.cpp
  src/player.cpp
  src/unload.cpp
  src/hooks/runner.cpp
  src/hooks/present.cpp
  src/overlay/swap.cpp
  src/overlay/renderer.cpp
  src/overlay/ui.cpp)
set_target_properties(cr-srt PROPERTIES PREFIX "")  # cr-srt.dll, not libcr-srt.dll
target_include_directories(cr-srt PRIVATE src)
target_compile_options(cr-srt PRIVATE -Wall -Wextra)
target_link_libraries(cr-srt PRIVATE imgui minhook dxgi d3dcompiler)
target_link_options(cr-srt PRIVATE -static -s)
