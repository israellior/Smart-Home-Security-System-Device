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

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)

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

add_library(porchlight_runtime STATIC
  src/config.cpp
  src/logging.cpp
  src/reactor.cpp)
target_include_directories(porchlight_runtime PUBLIC src)
target_link_libraries(porchlight_runtime PUBLIC nlohmann_json::nlohmann_json)

add_executable(porchlightd src/main.cpp)
target_link_libraries(porchlightd PRIVATE porchlight_runtime)
