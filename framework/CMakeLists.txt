add_library(optronic_core INTERFACE)
add_library(optronic::core ALIAS optronic_core)
target_include_directories(optronic_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
target_link_libraries(optronic_core INTERFACE optronic::options)

add_library(optronic_app app/src/app.cpp)
add_library(optronic::app ALIAS optronic_app)
target_include_directories(optronic_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/app/include)
target_link_libraries(optronic_app
  PUBLIC optronic::core optronic::options
  PRIVATE optronic::warnings optronic::sanitizers)
