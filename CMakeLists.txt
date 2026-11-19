cmake_minimum_required(VERSION 3.20)
project(fasthtmlparser VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20 CACHE STRING "C++ standard (20 or 23)")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Link-time optimization in Release: about 5-10% on the parser (see README).
include(CheckIPOSupported)
check_ipo_supported(RESULT fh_ipo_supported OUTPUT fh_ipo_error)
if(fh_ipo_supported)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

function(fh_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
endfunction()

add_library(fh STATIC
  src/tables.cpp
  src/decode.cpp
  src/text.cpp
  src/parse.cpp
  src/document.cpp
  src/uri.cpp
  src/css.cpp
  src/xpath.cpp
  src/generated_entities.cpp
)

target_include_directories(fh PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
fh_warnings(fh)

add_executable(fh_bench bench/main.cpp)
target_link_libraries(fh_bench PRIVATE fh)
fh_warnings(fh_bench)

add_executable(fh_tests tests/main.cpp)
target_link_libraries(fh_tests PRIVATE fh)
fh_warnings(fh_tests)

enable_testing()
add_test(NAME fh_tests COMMAND fh_tests ${CMAKE_CURRENT_SOURCE_DIR}/fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)
