# cr-srt — built with MinGW-w64 g++ (C++20) and Ninja; build.bat sets the toolchain up. Two static libraries from
# vendor/ and one DLL linked fully static, so the game needs nothing beside it.
cmake_minimum_required(VERSION 3.25)
project(cr-srt LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
enable_testing()
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

# What both DLLs share: the log, memory checks, the exe's sections, the cache and the solve, the ECS walk and the
# runner hook, which is where either one reads the game.
add_library(crsrt-core STATIC
  src/log.cpp
  src/memory.cpp
  src/guarded.cpp
  src/source.cpp
  src/snapfile.cpp
  src/pe.cpp
  src/cache.cpp
  src/solve.cpp
  src/boot.cpp
  src/dict.cpp
  src/ecs.cpp
  src/envwatch.cpp
  src/motion.cpp
  src/player.cpp
  src/enemies.cpp
  src/whereabouts.cpp
  src/hooks/registry.cpp
  src/hooks/runner.cpp)
target_include_directories(crsrt-core PUBLIC src)
target_compile_options(crsrt-core PRIVATE -Wall -Wextra)
target_link_libraries(crsrt-core PUBLIC minhook)

add_library(cr-srt SHARED
  src/dllmain.cpp
  src/unload.cpp
  src/hooks/damage.cpp
  src/hooks/present.cpp
  src/hooks/script.cpp
  src/hooks/sonar.cpp
  src/hooks/spawn.cpp
  src/hooks/transit.cpp
  src/hooks/uistate.cpp
  src/facts.cpp
  src/overlay/swap.cpp
  src/overlay/renderer.cpp
  src/overlay/ui.cpp)
set_target_properties(cr-srt PROPERTIES PREFIX "")  # cr-srt.dll, not libcr-srt.dll
target_compile_options(cr-srt PRIVATE -Wall -Wextra)
target_link_libraries(cr-srt PRIVATE crsrt-core imgui dxgi d3dcompiler)
target_link_options(cr-srt PRIVATE -static -s)

# The probe: the runner hook and a snapshot of the ECS on F9, nothing drawn (src/probe/snapshot.h).
add_library(cr-probe SHARED
  src/probe/dllmain.cpp
  src/probe/unload.cpp
  src/probe/snapshot.cpp)
set_target_properties(cr-probe PROPERTIES PREFIX "")
target_compile_options(cr-probe PRIVATE -Wall -Wextra)
target_link_libraries(cr-probe PRIVATE crsrt-core)
target_link_options(cr-probe PRIVATE -static -s)

# The tests: an exe over crsrt-core that reads fixtures instead of the game (tests/check.h). ctest runs it from tests/,
# where fixtures/ is.
add_executable(crsrt-tests
  tests/main.cpp
  tests/clock_test.cpp
  tests/dict_test.cpp
  tests/ecs_test.cpp
  tests/enemies_test.cpp
  tests/fixtures_test.cpp
  tests/guarded_test.cpp
  tests/memory_test.cpp
  tests/player_test.cpp
  tests/registry_test.cpp
  tests/whereabouts_test.cpp
  tests/snapfile_test.cpp)
target_include_directories(crsrt-tests PRIVATE tests)
target_compile_options(crsrt-tests PRIVATE -Wall -Wextra)
target_link_libraries(crsrt-tests PRIVATE crsrt-core)
target_link_options(crsrt-tests PRIVATE -static)
add_test(NAME crsrt-tests COMMAND crsrt-tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests)
