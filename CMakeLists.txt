cmake_minimum_required(VERSION 3.20)
project(cform LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SIMDJSON REQUIRED IMPORTED_TARGET simdjson)

add_library(cform_core STATIC
  src/json.cpp
  src/schema.cpp
  src/tui.cpp
  src/tree.cpp
  src/walker.cpp
)
target_include_directories(cform_core PUBLIC src)
target_link_libraries(cform_core PUBLIC PkgConfig::SIMDJSON)
target_compile_options(cform_core PRIVATE -Wall -Wextra -O2)

add_executable(cform src/main.cpp)
target_link_libraries(cform PRIVATE cform_core)

enable_testing()
add_executable(cform_test test/walker_test.cpp)
target_link_libraries(cform_test PRIVATE cform_core)
target_compile_options(cform_test PRIVATE -Wall -Wextra -O2)
add_test(NAME walker COMMAND cform_test)
