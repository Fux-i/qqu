cmake_minimum_required(VERSION 3.20)

set(PROJECT_NAME "qqu")

project(${PROJECT_NAME} VERSION 1.0 LANGUAGES CXX)

# Enforce the use of C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Disable compiler-specific extensions like __attribute__((...))
set(CMAKE_CXX_EXTENSIONS OFF)
# libc++ only when compiling with Clang (GCC rejects -stdlib=libc++)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-stdlib=libc++)
  add_link_options(-stdlib=libc++)
endif()
# build cache for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS True)

# Sanitizers: separate build dirs (build-asan / build-tsan). Never mix asan+tsan.
set(QQU_SANITIZER "" CACHE STRING "Sanitizer: asan | tsan | ubsan | none")
set_property(CACHE QQU_SANITIZER PROPERTY STRINGS "" asan tsan ubsan none)
if(QQU_SANITIZER STREQUAL "asan")
  add_compile_options(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
elseif(QQU_SANITIZER STREQUAL "tsan")
  add_compile_options(-O1 -g -fno-omit-frame-pointer -fsanitize=thread)
  add_link_options(-fsanitize=thread)
elseif(QQU_SANITIZER STREQUAL "ubsan")
  add_compile_options(-O1 -g -fno-omit-frame-pointer -fsanitize=undefined)
  add_link_options(-fsanitize=undefined)
elseif(NOT QQU_SANITIZER STREQUAL "" AND NOT QQU_SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unknown QQU_SANITIZER=${QQU_SANITIZER} (use asan|tsan|ubsan|none)")
endif()

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

add_executable(qqu_test_spsc src/tests/spsc.cc)
target_link_libraries(qqu_test_spsc PRIVATE
  GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(qqu_test_spsc)
