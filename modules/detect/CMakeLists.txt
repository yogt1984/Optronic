find_package(OpenCV QUIET COMPONENTS core imgproc dnn)

# Same rule as video and telemetry: a missing dependency removes the module,
# it does not break the build. The detector is the optional stage.
if(NOT OpenCV_FOUND)
  message(STATUS "OpenCV not found - skipping modules/detect")
  return()
endif()

add_library(optronic_detect src/yolo.cpp)
add_library(optronic::detect ALIAS optronic_detect)

target_include_directories(optronic_detect PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# OpenCV is PRIVATE: it reaches this library and stops.
target_link_libraries(optronic_detect
  PUBLIC optronic::core optronic::video optronic::options
  PRIVATE ${OpenCV_LIBS} optronic::warnings optronic::sanitizers)
target_include_directories(optronic_detect SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
