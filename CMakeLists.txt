cmake_minimum_required(VERSION 3.16)
project(am_dmf_two_layer_reproduction LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(am_dmf src/main.cpp)
if(MSVC)
  target_compile_options(am_dmf PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(am_dmf PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME core_self_test COMMAND am_dmf --self-test)
