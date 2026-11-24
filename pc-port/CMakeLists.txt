cmake_minimum_required(VERSION 3.20)
project(pcport LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  # determinism: no fast-math, no FMA contraction
  add_compile_options(/fp:precise)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -ffp-contract=off -fno-fast-math)
endif()

find_package(Threads REQUIRED)

add_library(pcport_core STATIC
  src/core/sim.cpp
  src/core/clock.cpp
  src/core/frame_limiter.cpp
  src/core/frame_stats.cpp
  src/core/input.cpp
  src/core/loop.cpp
  src/core/sha256.cpp)
target_include_directories(pcport_core PUBLIC src/core)
target_link_libraries(pcport_core PUBLIC Threads::Threads)
if(WIN32)
  target_compile_definitions(pcport_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(pcport_core PUBLIC winmm)
endif()

if(MINGW)
  # self-contained .exe: no libstdc++/libgcc/winpthread DLLs needed on the target PC
  set(PCPORT_STATIC_RT -static -static-libgcc -static-libstdc++)
endif()

add_executable(pcport_tests tests/test_frame_independence.cpp)
target_link_libraries(pcport_tests PRIVATE pcport_core ${PCPORT_STATIC_RT})

add_executable(pcport_bench src/app/bench_main.cpp)
target_link_libraries(pcport_bench PRIVATE pcport_core ${PCPORT_STATIC_RT})

add_executable(pcport_inventory src/tools/inventory_main.cpp)
target_link_libraries(pcport_inventory PRIVATE pcport_core ${PCPORT_STATIC_RT})

if(WIN32)
  add_executable(pcport_testscene src/platform/win32/main_win32.cpp)
  target_link_libraries(pcport_testscene PRIVATE pcport_core d3d11 dxgi dxguid ${PCPORT_STATIC_RT})
endif()

enable_testing()
add_test(NAME frame_independence_quick COMMAND pcport_tests --quick)
