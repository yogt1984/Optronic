cmake_minimum_required(VERSION 3.25)

project(Optronic
  VERSION 0.1.0
  DESCRIPTION "Sensor node application"
  LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(OPTRONIC_WERROR "Treat warnings as errors" OFF)
option(OPTRONIC_BUILD_TESTS "Build unit tests" ON)
option(OPTRONIC_LTO "Enable interprocedural optimisation for release builds" OFF)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Sanitizers)

if(OPTRONIC_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
  if(_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  else()
    message(WARNING "LTO requested but not supported: ${_ipo_msg}")
  endif()
endif()

find_package(Threads REQUIRED)

# Warning policy shared by every target. Linked PRIVATE, never exported.
add_library(optronic_warnings INTERFACE)
add_library(optronic::warnings ALIAS optronic_warnings)
target_compile_options(optronic_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  $<$<BOOL:${OPTRONIC_WERROR}>:-Werror>)

# Language level belongs to the targets, not the directory: only what links
# optronic::options gets it. C++23 for exactly one reason - std::expected is
# C++23, not C++20; everything else stays inside the C++20 subset of SPEC-11.
add_library(optronic_options INTERFACE)
add_library(optronic::options ALIAS optronic_options)
target_compile_features(optronic_options INTERFACE cxx_std_23)

# Feature detection rather than a version number. A version check would have
# claimed Clang 17 was fine: it is not, and not because of the compiler.
# libstdc++ 13 guards <expected> on __cpp_concepts >= 202002L, and Clang 17 and
# 18 both report 201907L because they do not implement P2113 - so the header
# hides itself and the failure surfaces as "no template named expected in
# namespace std", hundreds of lines from the cause. Clang works against libc++
# (-stdlib=libc++), which then also needs its own GoogleTest.
include(CheckCXXSourceCompiles)
# CMAKE_CXX_STANDARD, not CMAKE_REQUIRED_FLAGS: try_compile appends its own
# -std after anything in the flags, and the last one wins.
set(CMAKE_CXX_STANDARD 23)
check_cxx_source_compiles([[
  #include <expected>
  #include <format>
  int main() { std::expected<int, int> e{1}; return *e - 1; }
]] OPTRONIC_HAS_EXPECTED)
unset(CMAKE_CXX_STANDARD)

if(NOT OPTRONIC_HAS_EXPECTED)
  message(FATAL_ERROR
    "This toolchain cannot compile <expected>.\n"
    "  compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
    "Known good: GCC >= 13 with libstdc++, or Clang with -stdlib=libc++.\n"
    "Clang with libstdc++ 13 does not work at any Clang version currently "
    "packaged: libstdc++ gates <expected> on __cpp_concepts >= 202002L and "
    "Clang reports 201907L.")
endif()

add_subdirectory(framework)
add_subdirectory(modules)

add_executable(optronic main.cpp)
target_link_libraries(optronic PRIVATE
  optronic::options optronic::warnings optronic::sanitizers
  optronic::app optronic::log optronic::hal optronic::sensor Threads::Threads)

# The service builds and runs without GStreamer; it just has no pipeline. That
# keeps a bare host useful instead of turning a missing laptop dependency into
# a broken program.
if(TARGET optronic::video)
  target_link_libraries(optronic PRIVATE optronic::video)
  target_compile_definitions(optronic PRIVATE OPTRONIC_WITH_VIDEO=1)
else()
  target_compile_definitions(optronic PRIVATE OPTRONIC_WITH_VIDEO=0)
endif()

if(TARGET optronic::detect)
  target_link_libraries(optronic PRIVATE optronic::detect)
  target_compile_definitions(optronic PRIVATE OPTRONIC_WITH_DETECT=1)
else()
  target_compile_definitions(optronic PRIVATE OPTRONIC_WITH_DETECT=0)
endif()

if(TARGET optronic::telemetry)
  target_link_libraries(optronic PRIVATE optronic::telemetry)
  target_compile_definitions(optronic PRIVATE OPTRONIC_WITH_TELEMETRY=1)
else()
  target_compile_definitions(optronic PRIVATE OPTRONIC_WITH_TELEMETRY=0)
endif()

install(TARGETS optronic RUNTIME DESTINATION bin)

if(OPTRONIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
