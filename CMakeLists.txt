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
add_executable(qqu_test_mpsc src/tests/mpsc.cc)
target_link_libraries(qqu_test_mpsc PRIVATE
  GTest::gtest_main
)
add_executable(qqu_test_mpmc src/tests/mpmc.cc)
target_link_libraries(qqu_test_mpmc PRIVATE
  GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(qqu_test_spsc)
gtest_discover_tests(qqu_test_mpsc)
gtest_discover_tests(qqu_test_mpmc)

# --- benchmarks ---
FetchContent_Declare(
  SPSCQueue
  GIT_REPOSITORY https://github.com/rigtorp/SPSCQueue.git
  GIT_TAG v1.1
)
FetchContent_MakeAvailable(SPSCQueue)

FetchContent_Declare(
  atomic_queue
  GIT_REPOSITORY https://github.com/max0x7ba/atomic_queue.git
  GIT_TAG v1.9.2
)
FetchContent_MakeAvailable(atomic_queue)

FetchContent_Declare(
  MPMCQueue
  GIT_REPOSITORY https://github.com/rigtorp/MPMCQueue.git
  GIT_TAG b74a9f06469ec268b82eca3b5fe28dd58f312d06
)
FetchContent_MakeAvailable(MPMCQueue)

find_package(Threads REQUIRED)

add_executable(qqu_bench_spsc src/benchmarks/spsc_bench.cc)
add_executable(qqu_bench_mpsc src/benchmarks/mpsc_bench.cc)
add_executable(qqu_bench_mpmc src/benchmarks/mpmc_bench.cc)
target_link_libraries(qqu_bench_spsc PRIVATE SPSCQueue::SPSCQueue)
target_link_libraries(qqu_bench_mpsc PRIVATE MPMCQueue::MPMCQueue)
target_link_libraries(qqu_bench_mpmc PRIVATE MPMCQueue::MPMCQueue)
foreach(suite IN ITEMS mpmc mpsc spsc)
  target_link_libraries(qqu_bench_${suite} PRIVATE
    Threads::Threads
    atomic_queue::atomic_queue
  )
  if(NOT QQU_SANITIZER OR QQU_SANITIZER STREQUAL "none")
    target_compile_options(qqu_bench_${suite} PRIVATE -march=native)
  endif()
endforeach()
