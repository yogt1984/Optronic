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

add_library(optronic_log log/src/logger.cpp)
add_library(optronic::log ALIAS optronic_log)
target_include_directories(optronic_log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/log/include)
target_link_libraries(optronic_log
  PUBLIC optronic::core optronic::options
  PRIVATE optronic::warnings optronic::sanitizers)

add_library(optronic_hal hal/src/isp_model.cpp hal/src/uio.cpp)
add_library(optronic::hal ALIAS optronic_hal)
target_include_directories(optronic_hal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/hal/include)
target_link_libraries(optronic_hal
  PUBLIC optronic::core optronic::options
  PRIVATE optronic::warnings optronic::sanitizers)
