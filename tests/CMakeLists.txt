# tests/
#
# The suite prefers Catch2 v3 when available and falls back to GoogleTest, then
# to a self-contained micro-harness so the project always builds and runs.
#
#   cmake -DFIBERECS_BUILD_TESTS=ON ...

option(FIBERECS_USE_CATCH2 "Use Catch2 for the test suite if found" ON)
option(FIBERECS_USE_GTEST "Use GoogleTest for the test suite if found" ON)

if(NOT TARGET fiberecs_tests)
  if(FIBERECS_USE_CATCH2)
    find_package(Catch2 3 QUIET)
    if(Catch2_FOUND)
      list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
      include(Catch)
      catch_discover_tests(fiberecs_tests)
    endif()
  endif()

  if(NOT Catch2_FOUND AND FIBERECS_USE_GTEST)
    find_package(GTest QUIET)
    if(GTest_FOUND)
      include(GoogleTest)
    endif()
  endif()

  if(Catch2_FOUND)
    set(FIBERECS_TEST_SOURCES
      TestMain.cpp
      test_arenas.cpp
      test_fiber.cpp
      test_work_stealing.cpp
      test_scheduler.cpp
      test_ecs.cpp)

    add_executable(fiberecs_tests ${FIBERECS_TEST_SOURCES})
    target_link_libraries(fiberecs_tests PRIVATE
      fiberecs_job fiberecs_ecs Catch2::Catch2WithMain)
    target_compile_definitions(fiberecs_tests PRIVATE FIBERECS_TEST_CATCH2=1)
    add_test(NAME fiberecs_tests COMMAND fiberecs_tests)
  elseif(GTest_FOUND)
    set(FIBERECS_TEST_SOURCES
      TestMain.cpp
      test_arenas.cpp
      test_fiber.cpp
      test_work_stealing.cpp
      test_scheduler.cpp
      test_ecs.cpp)

    add_executable(fiberecs_tests ${FIBERECS_TEST_SOURCES})
    target_link_libraries(fiberecs_tests PRIVATE fiberecs_job fiberecs_ecs GTest::gtest_main)
    target_compile_definitions(fiberecs_tests PRIVATE FIBERECS_TEST_GTEST=1)
    include(GoogleTest)
    gtest_discover_tests(fiberecs_tests)
  else()
    message(STATUS "  tests ............ no Catch2/GoogleTest found, using built-in harness")
    set(FIBERECS_TEST_SOURCES
      TestMain.cpp
      test_arenas.cpp
      test_fiber.cpp
      test_work_stealing.cpp
      test_scheduler.cpp
      test_ecs.cpp)

    add_executable(fiberecs_tests ${FIBERECS_TEST_SOURCES})
    target_link_libraries(fiberecs_tests PRIVATE fiberecs_job fiberecs_ecs)
    target_compile_definitions(fiberecs_tests PRIVATE FIBERECS_TEST_BUILTIN=1)
    add_test(NAME fiberecs_tests COMMAND fiberecs_tests)
  endif()
endif()

# Archetypal ECS throughput benchmark: 100k entities, three systems per frame
# dispatched as parallel chunk jobs. Registers with CTest so a regression in
# correctness (or an unexpected heap allocation in a steady-state frame) shows
# up as a failing test, not just a slower number.
add_executable(ecs_archetype_bench ecs_archetype_bench.cpp)
target_link_libraries(ecs_archetype_bench PRIVATE fiberecs_job fiberecs_ecs fiberecs_options)
add_test(NAME ecs_archetype_bench COMMAND ecs_archetype_bench)
set_tests_properties(ecs_archetype_bench PROPERTIES TIMEOUT 300)

# The shipped verification harness is also registered with CTest.
add_test(NAME headless_smoke COMMAND fiberecs --headless-test)
set_tests_properties(headless_smoke PROPERTIES TIMEOUT 600)