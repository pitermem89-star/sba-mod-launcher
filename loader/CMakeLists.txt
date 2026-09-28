cmake_minimum_required(VERSION 3.22)
project(bearite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

add_library(bearite SHARED src/main.cpp)
target_link_libraries(bearite PRIVATE nlohmann_json::nlohmann_json log dl)
target_compile_options(bearite PRIVATE -Wall -Wextra -fvisibility=hidden)
# Android 15 devices may use 16 KB memory pages
target_link_options(bearite PRIVATE -Wl,-z,max-page-size=16384)
