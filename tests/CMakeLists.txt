# ThreadSanitizer maps its shadow memory at fixed addresses, and the kernel's
# high-entropy ASLR can land a library on top of them: the binary then aborts
# with "unexpected memory mapping" before running a single case - including the
# run gtest_discover_tests uses to enumerate them, which turns into a permanent
# <target>_NOT_BUILT failure. Disabling ASLR for the test binaries is the
# documented workaround, and the emulator hook is the only one that reaches
# both the discovery run and the tests themselves.
if(OPTRONIC_SANITIZER MATCHES "thread" AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  find_program(OPTRONIC_SETARCH setarch)
  if(OPTRONIC_SETARCH)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${OPTRONIC_SETARCH} -R)
  else()
    message(WARNING "setarch not found - ThreadSanitizer tests may abort on ASLR")
  endif()
endif()

# System GoogleTest when the image provides it, pinned source otherwise, so a
# bare host and the CI containers run the same tests.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

add_executable(test_core core/test_error.cpp)
# optronic::sanitizers has to be named here as well: the libraries link it
# PRIVATE, which for a static library propagates the link options but not the
# compile options - so without this the test code is linked against the
# sanitizer runtime while not being instrumented by it.
target_link_libraries(test_core PRIVATE
  optronic::core optronic::warnings optronic::sanitizers GTest::gtest_main)
# PRE_TEST everywhere: the default POST_BUILD mode runs the test binary during
# the build to enumerate cases, which cannot work when cross-compiling for
# aarch64 on an x86 host - it breaks the cross stage outright. Deferring the
# enumeration to ctest time means the host runs it natively and the target runs
# it under QEMU, from the same build.
gtest_discover_tests(test_core DISCOVERY_MODE PRE_TEST)

# Guarded the same way as the module: no GStreamer on the host means the video
# tests are absent, not failing.
#
# Excluded under ThreadSanitizer on purpose. GLib, GObject and libx264 are not
# built with TSan, so the tool cannot see their locks and atomics: a run
# reports races inside g_queue_pop_tail, g_type_class_ref and libx264's own
# worker pool, none of them in our code. Silencing that needs a suppression
# list covering five third-party libraries, which would hide a real race in
# the module just as effectively. TSan therefore covers the framework, where
# it is meaningful, and the video module is covered by ASan/UBSan instead
# (both clean, including leak detection).
if(TARGET optronic::video AND NOT OPTRONIC_SANITIZER MATCHES "thread")
  add_executable(test_video
    video/test_factory.cpp
    video/test_pipeline.cpp)
  target_link_libraries(test_video PRIVATE
    optronic::video optronic::warnings optronic::sanitizers GTest::gtest_main)
  gtest_discover_tests(test_video DISCOVERY_MODE PRE_TEST)
endif()

add_executable(test_app app/test_app.cpp)
target_link_libraries(test_app PRIVATE
  optronic::app optronic::warnings optronic::sanitizers GTest::gtest_main)
gtest_discover_tests(test_app DISCOVERY_MODE PRE_TEST)

add_executable(test_log log/test_ring.cpp log/test_logger.cpp)
target_link_libraries(test_log PRIVATE
  optronic::log optronic::warnings optronic::sanitizers GTest::gtest_main)
gtest_discover_tests(test_log DISCOVERY_MODE PRE_TEST)

# ctest runs each discovered case in its own process, which hides anything that
# only goes wrong when objects are created and destroyed in sequence - a stale
# per-thread cache pointing into a freed logger, for one. This entry runs the
# whole binary once, in a single process, on purpose.
add_test(NAME log_single_process COMMAND test_log)

add_executable(test_hal hal/test_hal.cpp)
target_link_libraries(test_hal PRIVATE
  optronic::hal optronic::warnings optronic::sanitizers GTest::gtest_main)
gtest_discover_tests(test_hal DISCOVERY_MODE PRE_TEST)

if(TARGET optronic::telemetry)
  add_executable(test_telemetry telemetry/test_publisher.cpp)
  target_link_libraries(test_telemetry PRIVATE
    optronic::telemetry optronic::warnings optronic::sanitizers GTest::gtest_main)
  gtest_discover_tests(test_telemetry DISCOVERY_MODE PRE_TEST)
endif()

add_executable(test_sensor sensor/test_nuc.cpp)
target_link_libraries(test_sensor PRIVATE
  optronic::sensor optronic::warnings optronic::sanitizers GTest::gtest_main)
gtest_discover_tests(test_sensor DISCOVERY_MODE PRE_TEST)

if(TARGET optronic::detect)
  add_executable(test_detect detect/test_yolo.cpp)
  target_link_libraries(test_detect PRIVATE
    optronic::detect optronic::warnings optronic::sanitizers GTest::gtest_main)
  # The detector reads its model and reference scene by relative path, so the
  # tests run from the source tree rather than the build directory.
  gtest_discover_tests(test_detect DISCOVERY_MODE PRE_TEST
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
