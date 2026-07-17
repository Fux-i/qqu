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
