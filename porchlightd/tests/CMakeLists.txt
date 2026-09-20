find_package(GTest QUIET)
if(NOT GTest_FOUND)
  message(STATUS "GTest not installed; fetching it")
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(core_tests
  support.cpp
  alert_test.cpp
  button_test.cpp
  call_test.cpp
  led_test.cpp
  motion_test.cpp
  recording_test.cpp
  shutdown_test.cpp)

target_include_directories(core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(core_tests PRIVATE porchlight_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(core_tests)
