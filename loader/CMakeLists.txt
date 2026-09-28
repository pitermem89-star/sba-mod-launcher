cmake_minimum_required(VERSION 3.22)
project(bearite LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
# NOTE: Dobby has no stable tags, pin a commit after the first good build.
FetchContent_Declare(dobby
  GIT_REPOSITORY https://github.com/jmpews/Dobby.git
  GIT_TAG master
  GIT_SHALLOW TRUE)

set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json dobby)

add_library(bearite SHARED src/main.cpp)
target_include_directories(bearite PRIVATE ${dobby_SOURCE_DIR}/include)
target_link_libraries(bearite PRIVATE nlohmann_json::nlohmann_json dobby log dl)
target_compile_options(bearite PRIVATE -Wall -Wextra -fvisibility=hidden)
# Android 15 devices may use 16 KB memory pages
target_link_options(bearite PRIVATE -Wl,-z,max-page-size=16384)
