cmake_minimum_required(VERSION 3.16)
project(am_dmf_two_layer_reproduction LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(dmf_core src/protocol.cpp src/dynamic_dag.cpp)
target_include_directories(dmf_core PUBLIC include)

add_executable(am_dmf src/main.cpp)
target_link_libraries(am_dmf PRIVATE dmf_core)

add_executable(dmf_runtime_tests tests/runtime_tests.cpp)
target_link_libraries(dmf_runtime_tests PRIVATE dmf_core)

foreach(target dmf_core am_dmf dmf_runtime_tests)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()

enable_testing()
add_test(NAME runtime_tests COMMAND dmf_runtime_tests)
