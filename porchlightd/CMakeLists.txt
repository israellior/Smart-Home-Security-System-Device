cmake_minimum_required(VERSION 3.22)
project(porchlightd CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(PORCHLIGHT_SANITIZE "Build with the address and undefined-behaviour sanitizers" OFF)

# Off by default because the development machines are Windows and WSL2, which
# have no GPIO and cannot install libgpiod. Turn it on when building on the Pi.
option(PORCHLIGHT_GPIO "Build the libgpiod backends for the button, the PIR and the LED" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)

# -Wmaybe-uninitialized is speculative, and std::variant at -O2 defeats it: the
# optimizer inlines every alternative's move constructor into the call site and
# then reports the branches it cannot prove unreachable. Moving an Event that
# holds Tick draws a warning about ViewerRequested's std::string.
#
# 22 of them in Release, none in Debug, from identical code, and not one points
# at a file of ours - every report is inside libstdc++. -Wuninitialized, which
# fires only on a definite read, stays on.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options(-Wno-maybe-uninitialized)
endif()

if(PORCHLIGHT_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  message(STATUS "nlohmann_json not installed; fetching it")
  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# Pure: no I/O, no dependencies, and what the tests exercise on their own.
add_library(porchlight_core STATIC
  src/core/alert_queue.cpp
  src/core/core.cpp)
target_include_directories(porchlight_core PUBLIC src)

add_library(porchlight_runtime STATIC
  src/backends.cpp
  src/child_process.cpp
  src/config.cpp
  src/daemon.cpp
  src/io/alsa_chime.cpp
  src/io/bridge_server_link.cpp
  src/io/gst_recorder.cpp
  src/io/script_media.cpp
  src/io/script_uploader.cpp
  src/logging.cpp
  src/random_ids.cpp
  src/reactor.cpp
  src/timer_fd.cpp)
target_include_directories(porchlight_runtime PUBLIC src)
target_link_libraries(porchlight_runtime PUBLIC porchlight_core nlohmann_json::nlohmann_json)

if(PORCHLIGHT_GPIO)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GPIOD IMPORTED_TARGET libgpiod>=2.0)
  if(NOT GPIOD_FOUND)
    # Named explicitly rather than left to pkg-config's own wording, because the
    # likely failure is not "missing" but "found 1.6": Debian 12 and older ship
    # libgpiod 1.x, whose C API is a different one entirely and will not build
    # these files. Debian 13 trixie, which the Pi runs, has 2.x.
    message(FATAL_ERROR
      "PORCHLIGHT_GPIO needs libgpiod 2.x - apt install libgpiod-dev. "
      "libgpiod 1.x will not do: its C API is unrelated.")
  endif()
  target_sources(porchlight_runtime PRIVATE src/io/gpio_input.cpp src/io/gpio_led.cpp)
  target_link_libraries(porchlight_runtime PUBLIC PkgConfig::GPIOD)
  target_compile_definitions(porchlight_runtime PUBLIC PORCHLIGHT_GPIO)
endif()

# Only where the GPIO backends are compiled: the daemon names GpioInput and
# GpioLed directly, with no interface in front of them, so without libgpiod it
# cannot link. The libraries and the tests still build everywhere.
if(PORCHLIGHT_GPIO)
  add_executable(porchlightd src/main.cpp)
  target_link_libraries(porchlightd PRIVATE porchlight_runtime)
else()
  message(STATUS "PORCHLIGHT_GPIO is off: building the tests, not the porchlightd binary")
endif()

# pi/record-clip.py and pi/upload-clip.py in C++: the same arguments, messages
# and exit codes, so recorder.script or server.uploader_path can name either.
# Nothing names these yet. Each is built only where its library's headers are
# (libgstreamer1.0-dev, libcurl4-openssl-dev), so a machine without them builds
# everything else as before.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(GSTREAMER QUIET IMPORTED_TARGET gstreamer-1.0)
  pkg_check_modules(LIBCURL QUIET IMPORTED_TARGET libcurl)
endif()
if(GSTREAMER_FOUND)
  add_executable(record-clip children/record_clip.cpp)
  target_link_libraries(record-clip PRIVATE PkgConfig::GSTREAMER)
else()
  message(STATUS "gstreamer-1.0 not found: not building record-clip")
endif()
if(LIBCURL_FOUND)
  add_executable(upload-clip children/upload_clip.cpp)
  target_link_libraries(upload-clip PRIVATE PkgConfig::LIBCURL nlohmann_json::nlohmann_json)
else()
  message(STATUS "libcurl not found: not building upload-clip")
endif()

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
