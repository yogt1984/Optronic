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
target_link_libraries(test_core PRIVATE
  optronic::core optronic::warnings GTest::gtest_main)
gtest_discover_tests(test_core)
