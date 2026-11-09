# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Matteo Scalia

cmake_minimum_required(VERSION 3.20)
project(cform VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(EMSCRIPTEN)
  # --- WebAssembly module consumed by wasm/index.js -------------------------
  add_compile_options(-fexceptions -Wall -Wextra)
  add_link_options(-fexceptions -sDISABLE_EXCEPTION_CATCHING=0)

  add_library(cform_core STATIC
    src/artwork.cpp
    src/json.cpp
    src/json_parse.cpp
    src/schema.cpp
    src/tui.cpp
    src/tree.cpp
    src/walker.cpp
    src/terminal_wasm.cpp
  )
  target_include_directories(cform_core PUBLIC src)

  add_executable(cform_wasm wasm/bindings.cpp)
  target_link_libraries(cform_wasm PRIVATE cform_core)
  target_link_options(cform_wasm PRIVATE
    -lembind
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createCformModule"
    "-sENVIRONMENT=node"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sSINGLE_FILE=1"
    "-sSTACK_SIZE=1048576"
    "-sEXPORTED_RUNTIME_METHODS=HEAPU8"
    "--emit-tsd=${CMAKE_CURRENT_SOURCE_DIR}/wasm/build/cform.d.ts"
  )
  set_target_properties(cform_wasm PROPERTIES
    OUTPUT_NAME cform
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/wasm/build"
  )
else()
  # --- native CLI and tests -------------------------------------------------
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SIMDJSON REQUIRED IMPORTED_TARGET simdjson)

  add_library(cform_core STATIC
    src/artwork.cpp
    src/json.cpp
    src/schema.cpp
    src/tui.cpp
    src/tree.cpp
    src/walker.cpp
    src/terminal_posix.cpp
  )
  target_include_directories(cform_core PUBLIC src)
  target_compile_definitions(cform_core PUBLIC CFORM_USE_SIMDJSON=1)
  target_link_libraries(cform_core PUBLIC PkgConfig::SIMDJSON)
  target_compile_options(cform_core PRIVATE -Wall -Wextra)

  add_executable(cform src/main.cpp)
  target_link_libraries(cform PRIVATE cform_core)
  target_compile_options(cform PRIVATE -Wall -Wextra)

  target_compile_definitions(cform PRIVATE CFORM_VERSION="${PROJECT_VERSION}")

  # Optional: libchafa renders the right-side artwork from an image file.
  pkg_check_modules(CHAFA chafa>=1.6 IMPORTED_TARGET)
  if(CHAFA_FOUND)
    include(FetchContent)
    FetchContent_Declare(
      stb
      GIT_REPOSITORY https://github.com/nothings/stb.git
      GIT_TAG d14d46f6b44456eb88beac58cba833a4135d4c89
    )
    FetchContent_MakeAvailable(stb)

    target_sources(cform PRIVATE src/artwork_chafa.cpp)
    target_compile_definitions(cform PRIVATE CFORM_HAVE_CHAFA=1)
    target_include_directories(cform PRIVATE ${stb_SOURCE_DIR})
    target_link_libraries(cform PRIVATE PkgConfig::CHAFA m)
  endif()

  enable_testing()
  add_executable(cform_test test/walker_test.cpp)
  target_link_libraries(cform_test PRIVATE cform_core)
  target_compile_options(cform_test PRIVATE -Wall -Wextra)
  add_test(NAME walker COMMAND cform_test)

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(CHAFA_FOUND AND Python3_Interpreter_FOUND)
    add_test(
      NAME resize
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/resize_test.py
              $<TARGET_FILE:cform>
    )
  endif()
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME resume
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/resume_test.py
              $<TARGET_FILE:cform>
    )
    add_test(
      NAME popup
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/test/popup_test.py
              $<TARGET_FILE:cform>
    )
  endif()

  # Ship the license next to the installed binary so distributed builds carry
  # the required notice.
  include(GNUInstallDirs)
  install(TARGETS cform RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DOCDIR}")
endif()
