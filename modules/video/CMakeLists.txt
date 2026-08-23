find_package(PkgConfig REQUIRED)
pkg_check_modules(GST IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0)

# A host without the GStreamer headers still configures and builds everything
# else. The container images carry them, so CI always covers this module - a
# missing laptop dependency must not look like a broken repository.
if(NOT GST_FOUND)
  message(STATUS "GStreamer development files not found - skipping modules/video")
  return()
endif()

add_library(optronic_video
  src/factory.cpp
  src/pipeline.cpp)
add_library(optronic::video ALIAS optronic_video)

target_include_directories(optronic_video PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# GStreamer is PRIVATE: it reaches this library and stops. A component that
# tries to include <gst/gst.h> gets a missing header, not a working build -
# the module boundary of SPEC-19 checked by the build rather than in review.
target_link_libraries(optronic_video
  PUBLIC optronic::core optronic::options
  PRIVATE PkgConfig::GST optronic::warnings optronic::sanitizers)
