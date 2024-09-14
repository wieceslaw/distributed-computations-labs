cmake_minimum_required(VERSION 3.28)
project(program C)
set(CMAKE_C_STANDARD 99)

set(TARGET_NAME program)
file(GLOB_RECURSE HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/**.h)
file(GLOB_RECURSE SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/**.c)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_executable(${TARGET_NAME} ${SOURCES} ${HEADERS})

add_compile_options(
        -std=c99
        -Wall
        -Wpedantic
)
