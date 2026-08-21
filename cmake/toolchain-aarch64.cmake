# aarch64 Linux cross toolchain.
#
# Two worlds, one file:
#  * Yocto / PetaLinux SDK: source the SDK's environment-setup script first.
#    OECORE_TARGET_SYSROOT, CC and CXX are then taken from the environment.
#  * Plain Debian/Ubuntu cross packages (crossbuild-essential-arm64):
#    aarch64-linux-gnu-g++ is used; OPTRONIC_SYSROOT may point at a sysroot.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(DEFINED ENV{OECORE_TARGET_SYSROOT})
  # $CXX from the SDK looks like "aarch64-xilinx-linux-g++ -mcpu=... --sysroot=...".
  separate_arguments(_cxx_words UNIX_COMMAND "$ENV{CXX}")
  separate_arguments(_cc_words UNIX_COMMAND "$ENV{CC}")
  list(POP_FRONT _cxx_words _cxx_exe)
  list(POP_FRONT _cc_words _cc_exe)
  set(CMAKE_CXX_COMPILER ${_cxx_exe})
  set(CMAKE_C_COMPILER ${_cc_exe})
  string(JOIN " " CMAKE_CXX_FLAGS_INIT ${_cxx_words})
  string(JOIN " " CMAKE_C_FLAGS_INIT ${_cc_words})
  set(CMAKE_SYSROOT $ENV{OECORE_TARGET_SYSROOT})
  set(OPTRONIC_TOOLCHAIN_FLAVOUR "yocto-sdk" CACHE STRING "" FORCE)
else()
  set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
  set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
  if(DEFINED ENV{OPTRONIC_SYSROOT})
    set(CMAKE_SYSROOT $ENV{OPTRONIC_SYSROOT})
  endif()
  set(CMAKE_CXX_FLAGS_INIT "-march=armv8-a+crc")
  set(CMAKE_C_FLAGS_INIT "-march=armv8-a+crc")
  set(OPTRONIC_TOOLCHAIN_FLAVOUR "debian-cross" CACHE STRING "" FORCE)
endif()

# Programs from the host, libraries/headers/packages only from the target.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config must look into the target sysroot, not the host.
if(CMAKE_SYSROOT)
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
  set(ENV{PKG_CONFIG_LIBDIR}
      "${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig:${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig")
endif()
