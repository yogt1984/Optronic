cmake_minimum_required(VERSION 3.25)

project(Optronic
  VERSION 0.1.0
  DESCRIPTION "Sensor node application"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(OPTRONIC_WERROR "Treat warnings as errors" OFF)
option(OPTRONIC_BUILD_TESTS "Build unit tests" ON)

find_package(Threads REQUIRED)

# Warning policy shared by every target. Linked PRIVATE, never exported.
add_library(optronic_warnings INTERFACE)
add_library(optronic::warnings ALIAS optronic_warnings)
target_compile_options(optronic_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  $<$<BOOL:${OPTRONIC_WERROR}>:-Werror>)

add_executable(optronic main.cpp)
target_include_directories(optronic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(optronic PRIVATE optronic::warnings Threads::Threads)

install(TARGETS optronic RUNTIME DESTINATION bin)
