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

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
  message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} too old - need >= 13 for <expected>")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
  message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} too old - need >= 17 for <expected>")
endif()

add_subdirectory(framework)
add_subdirectory(modules)

add_executable(optronic main.cpp)
target_include_directories(optronic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(optronic PRIVATE
  optronic::options optronic::warnings optronic::sanitizers Threads::Threads)

install(TARGETS optronic RUNTIME DESTINATION bin)

if(OPTRONIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
