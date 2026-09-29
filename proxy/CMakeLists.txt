cmake_minimum_required(VERSION 3.22)
project(bearite_proxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

add_library(main SHARED src/main.cpp)
target_link_libraries(main PRIVATE log dl)
target_compile_options(main PRIVATE -fvisibility=hidden)
target_link_options(main PRIVATE -Wl,-z,max-page-size=16384)
