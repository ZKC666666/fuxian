cmake_minimum_required(VERSION 3.16)
project(am_dmf_two_layer_reproduction LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(dmf_core src/protocol.cpp src/dynamic_dag.cpp src/placement.cpp
                     src/upper_layer.cpp src/router.cpp src/execution_loop.cpp)
target_include_directories(dmf_core PUBLIC include)

add_executable(am_dmf src/main.cpp)
target_link_libraries(am_dmf PRIVATE dmf_core)

add_executable(dmf_custom_route src/custom_route.cpp src/custom_route_case.cpp)
target_link_libraries(dmf_custom_route PRIVATE dmf_core)
target_compile_definitions(dmf_custom_route PRIVATE
  DMF_DEFAULT_CASE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/route_case.txt")

add_executable(dmf_runtime_tests tests/runtime_tests.cpp)
target_link_libraries(dmf_runtime_tests PRIVATE dmf_core)

add_executable(dmf_placement_tests tests/placement_tests.cpp)
target_link_libraries(dmf_placement_tests PRIVATE dmf_core)

add_executable(dmf_router_tests tests/router_tests.cpp)
target_link_libraries(dmf_router_tests PRIVATE dmf_core)

add_executable(dmf_execution_tests tests/execution_tests.cpp)
target_link_libraries(dmf_execution_tests PRIVATE dmf_core)

foreach(target dmf_core am_dmf dmf_custom_route dmf_runtime_tests
               dmf_placement_tests dmf_router_tests dmf_execution_tests)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()

enable_testing()
add_test(NAME runtime_tests COMMAND dmf_runtime_tests)
add_test(NAME placement_tests COMMAND dmf_placement_tests)
add_test(NAME router_tests COMMAND dmf_router_tests)
add_test(NAME execution_tests COMMAND dmf_execution_tests)
add_test(NAME placement_demo COMMAND am_dmf placement)
add_test(NAME router_demo COMMAND am_dmf router)
add_test(NAME timed_demo COMMAND am_dmf timed)
add_test(NAME detour_demo COMMAND am_dmf)
add_test(NAME custom_route_demo COMMAND dmf_custom_route)
add_test(NAME custom_route_invalid
         COMMAND dmf_custom_route "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/route_case_invalid.txt")
add_test(NAME custom_route_timeout
         COMMAND dmf_custom_route "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/route_case_timeout.txt")
add_test(NAME custom_route_close_starts
         COMMAND dmf_custom_route "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/route_case_close_starts.txt")
set_tests_properties(custom_route_invalid custom_route_timeout custom_route_close_starts
                     PROPERTIES WILL_FAIL TRUE)
