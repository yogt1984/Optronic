find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQ IMPORTED_TARGET libmosquitto)

# Same rule as the video module: no mosquitto, no telemetry, everything else
# still builds. The container images carry it, so CI always covers this.
if(NOT MOSQ_FOUND)
  message(STATUS "libmosquitto not found - skipping modules/telemetry")
  return()
endif()

add_library(optronic_telemetry src/publisher.cpp)
add_library(optronic::telemetry ALIAS optronic_telemetry)

target_include_directories(optronic_telemetry PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# mosquitto is PRIVATE: it reaches this library and stops.
target_link_libraries(optronic_telemetry
  PUBLIC optronic::core optronic::log optronic::options
  PRIVATE PkgConfig::MOSQ optronic::warnings optronic::sanitizers)
