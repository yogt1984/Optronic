add_library(optronic_core INTERFACE)
add_library(optronic::core ALIAS optronic_core)
target_include_directories(optronic_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
target_link_libraries(optronic_core INTERFACE optronic::options)
