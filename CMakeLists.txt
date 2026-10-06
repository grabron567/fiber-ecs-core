cmake_minimum_required(VERSION 3.20)

project(fiberecs
  VERSION 0.2.0
  DESCRIPTION "Fiber-based job system and archetypal ECS foundation"
  LANGUAGES CXX)

# The x86_64 fiber backend needs a hand-written context switch.
include(CheckCXXSourceCompiles)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT MSVC)
  enable_language(ASM)
  set(FIBERECS_HAS_ASM_FIBER ON)
else()
  set(FIBERECS_HAS_ASM_FIBER OFF)
endif()

# ---------------------------------------------------------------------------
# Language level
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Warnings: max level, errors on
# ---------------------------------------------------------------------------
add_library(fiberecs_warnings INTERFACE)
if(MSVC)
  target_compile_options(fiberecs_warnings INTERFACE
    /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /MP /utf-8)
  target_compile_definitions(fiberecs_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(fiberecs_warnings INTERFACE
    -Wall -Wextra -Werror
    -Wshadow -Wnon-virtual-dtor -Wcast-align -Wunused -Woverloaded-virtual
    -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
    -Wmissing-declarations -Wold-style-cast -Wredundant-decls
    -Wnull-dereference -Wconversion -Wsign-conversion)
endif()

# ---------------------------------------------------------------------------
# Sanitizers: Debug enables ASan + UBSan; TSan is a separate opt-in because it
# cannot be combined with ASan in a single build.
# ---------------------------------------------------------------------------
option(FIBERECS_SANITIZE_ADDRESS "Debug builds use AddressSanitizer + UBSan" ON)
option(FIBERECS_SANITIZE_THREAD "Debug builds use ThreadSanitizer (mutually exclusive with ASan)" OFF)
option(FIBERECS_NATIVE_ARCH "Tune for the host CPU (-march=native)" OFF)

# ---------------------------------------------------------------------------
# Sanitizer availability. Some toolchains ship the compiler driver without the
# sanitizer runtimes (MinGW-w64 has no libasan/libtsan), so probe with a real
# compile+link instead of assuming.
# ---------------------------------------------------------------------------
unset(FIBERECS_ASAN_AVAILABLE CACHE)
unset(FIBERECS_TSAN_AVAILABLE CACHE)
set(FIBERECS_ASAN_AVAILABLE OFF)
set(FIBERECS_TSAN_AVAILABLE OFF)

if(MSVC)
  set(CMAKE_REQUIRED_LINK_OPTIONS "/fsanitize=address")
  check_cxx_source_compiles("int main(){return 0;}" FIBERECS_ASAN_AVAILABLE)
  set(CMAKE_REQUIRED_LINK_OPTIONS "")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
  check_cxx_source_compiles("int main(){return 0;}" FIBERECS_ASAN_AVAILABLE)
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=thread")
  check_cxx_source_compiles("int main(){return 0;}" FIBERECS_TSAN_AVAILABLE)
  set(CMAKE_REQUIRED_LINK_OPTIONS "")
endif()

add_library(fiberecs_sanitizers INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  if(FIBERECS_SANITIZE_THREAD)
    if(NOT FIBERECS_TSAN_AVAILABLE)
      message(WARNING
        "FIBERECS_SANITIZE_THREAD=ON but this toolchain has no ThreadSanitizer "
        "runtime. Continuing without -fsanitize=thread; data-race verification "
        "must be run on a toolchain that ships libtsan (clang on Linux, or MSVC "
        "/fsanitize=thread which is not used here).")
      set(FIBERECS_SANITIZE_THREAD OFF)
    else()
      target_compile_options(fiberecs_sanitizers INTERFACE
        -fsanitize=thread -fno-omit-frame-pointer -g)
      target_link_options(fiberecs_sanitizers INTERFACE -fsanitize=thread)
      target_compile_definitions(fiberecs_sanitizers INTERFACE FIBERECS_TSAN=1)
    endif()
  elseif(FIBERECS_SANITIZE_ADDRESS)
    if(NOT FIBERECS_ASAN_AVAILABLE)
      message(WARNING
        "FIBERECS_SANITIZE_ADDRESS=ON but this toolchain has no "
        "AddressSanitizer/UndefinedBehaviorSanitizer runtime. Continuing without "
        "sanitizers; configure with -DFIBERECS_SANITIZE_ADDRESS=OFF to silence.")
      set(FIBERECS_SANITIZE_ADDRESS OFF)
    else()
      target_compile_options(fiberecs_sanitizers INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
      target_link_options(fiberecs_sanitizers INTERFACE -fsanitize=address,undefined)
      target_compile_definitions(fiberecs_sanitizers INTERFACE
        FIBERECS_ASAN=1 FIBERECS_UBSAN=1)
    endif()
  endif()

  target_compile_options(fiberecs_sanitizers INTERFACE
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>)
  if(FIBERECS_NATIVE_ARCH AND NOT CMAKE_CROSSCOMPILING)
    target_compile_options(fiberecs_sanitizers INTERFACE
      $<$<NOT:$<CONFIG:Debug>>:-march=native>)
  endif()
endif()

# MSVC: ASan is available, TSan is not.
if(MSVC AND FIBERECS_SANITIZE_ADDRESS)
  if(FIBERECS_ASAN_AVAILABLE)
    target_compile_options(fiberecs_sanitizers INTERFACE
      $<$<CONFIG:Debug>:/fsanitize=address>)
    target_link_options(fiberecs_sanitizers INTERFACE
      $<$<CONFIG:Debug>:/fsanitize=address>)
    target_compile_definitions(fiberecs_sanitizers INTERFACE FIBERECS_ASAN=1)
  else()
    message(WARNING "FIBERECS_SANITIZE_ADDRESS=ON but MSVC ASan link test failed; disabling.")
    set(FIBERECS_SANITIZE_ADDRESS OFF)
  endif()
endif()

add_library(fiberecs_options INTERFACE)
target_link_libraries(fiberecs_options INTERFACE fiberecs_warnings fiberecs_sanitizers)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(core)
add_subdirectory(job)
add_subdirectory(include)

add_executable(fiberecs main.cpp)
target_link_libraries(fiberecs PRIVATE fiberecs_job fiberecs_options)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)
enable_testing()
add_subdirectory(tests)

message(STATUS "fiberecs ${PROJECT_VERSION}")
message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  ASan+UBSan ......... ${FIBERECS_SANITIZE_ADDRESS} (runtime available: ${FIBERECS_ASAN_AVAILABLE})")
message(STATUS "  ThreadSanitizer ... ${FIBERECS_SANITIZE_THREAD} (runtime available: ${FIBERECS_TSAN_AVAILABLE})")
message(STATUS "  warnings ........... -Werror equivalent")