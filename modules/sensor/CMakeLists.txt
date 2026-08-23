# Header-only: the sequence is a template over the MmioBackend concept, so it
# compiles against the fake on a laptop and against UioRegion on the target
# without a virtual call in between.
add_library(optronic_sensor INTERFACE)
add_library(optronic::sensor ALIAS optronic_sensor)
target_include_directories(optronic_sensor INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(optronic_sensor INTERFACE optronic::hal optronic::core optronic::options)
