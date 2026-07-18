cmake_minimum_required(VERSION 3.20)

set(PROJECT_NAME "qqu")

project(${PROJECT_NAME} VERSION 1.0 LANGUAGES CXX)

# Enforce the use of C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Disable compiler-specific extensions like __attribute__((...))
set(CMAKE_CXX_EXTENSIONS OFF)
# Use libc++ standard (with clang)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
# build cache for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS True)

include_directories(include)

include(FetchContent)
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/deps" CACHE PATH "External deps root" FORCE)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Whether to skip dep update checks" FORCE)

# --- tests ---
enable_testing()
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(googletest)

add_executable(qqu_test_spsc tests/spsc.cc)
target_link_libraries(qqu_test_spsc PRIVATE
  GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(qqu_test_spsc)
