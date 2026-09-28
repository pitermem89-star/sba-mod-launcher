cmake_minimum_required(VERSION 3.22)
project(hello_mod LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(hello SHARED src/hello.cpp)
target_include_directories(hello PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../api)
target_link_libraries(hello PRIVATE log)
target_compile_options(hello PRIVATE -Wall -Wextra -fvisibility=hidden)
target_link_options(hello PRIVATE -Wl,-z,max-page-size=16384)
