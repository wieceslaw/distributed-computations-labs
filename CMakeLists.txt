cmake_minimum_required(VERSION 3.16)
project(program C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_GENERATOR_PLATFORM x64)

add_compile_options(
        -std=c99
        -Wall
        -Wpedantic
)

add_subdirectory(1)
add_subdirectory(2)
add_subdirectory(3)
add_subdirectory(4)
