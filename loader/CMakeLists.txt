cmake_minimum_required(VERSION 3.22)
project(bearite LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_Declare(and64
  GIT_REPOSITORY https://github.com/Rprop/And64InlineHook.git
  GIT_TAG master
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(json and64)

# Hook library: single .cpp, built separately with warnings off
file(GLOB_RECURSE AND64_SRC "${and64_SOURCE_DIR}/And64InlineHook.cpp")
if(NOT AND64_SRC)
  message(FATAL_ERROR "And64InlineHook.cpp not found in ${and64_SOURCE_DIR}")
endif()
list(GET AND64_SRC 0 AND64_SRC)
add_library(and64hook STATIC ${AND64_SRC})
set_target_properties(and64hook PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(and64hook PRIVATE -w)

add_library(bearite SHARED src/main.cpp)
target_link_libraries(bearite PRIVATE nlohmann_json::nlohmann_json and64hook log dl)
target_compile_options(bearite PRIVATE -Wall -Wextra -fvisibility=hidden)
# Android 15 devices may use 16 KB memory pages
target_link_options(bearite PRIVATE -Wl,-z,max-page-size=16384)
