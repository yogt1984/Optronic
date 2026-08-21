# optronic::sanitizers - INTERFACE target carrying sanitizer flags.
# Selected with -DOPTRONIC_SANITIZER=address|thread|undefined|none (or "address,undefined").

set(OPTRONIC_SANITIZER "none" CACHE STRING "Sanitizer set: none, address, thread, undefined (comma-separated)")

add_library(optronic_sanitizers INTERFACE)
add_library(optronic::sanitizers ALIAS optronic_sanitizers)

if(NOT OPTRONIC_SANITIZER STREQUAL "none")
  string(REPLACE "," ";" _san_list "${OPTRONIC_SANITIZER}")
  foreach(_san IN LISTS _san_list)
    if(NOT _san MATCHES "^(address|thread|undefined|leak)$")
      message(FATAL_ERROR "OPTRONIC_SANITIZER: unknown sanitizer '${_san}'")
    endif()
  endforeach()
  if("address" IN_LIST _san_list AND "thread" IN_LIST _san_list)
    message(FATAL_ERROR "OPTRONIC_SANITIZER: address and thread sanitizers cannot be combined")
  endif()
  string(JOIN "," _san_csv ${_san_list})
  target_compile_options(optronic_sanitizers INTERFACE
    -fsanitize=${_san_csv} -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  target_link_options(optronic_sanitizers INTERFACE -fsanitize=${_san_csv})
  message(STATUS "Sanitizers: ${_san_csv}")
endif()
