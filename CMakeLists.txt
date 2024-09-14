cmake_minimum_required(VERSION 3.28)
project(program C)
set(CMAKE_C_STANDARD 99)

add_compile_options(
        -std=c99
        -Wall
        -Wpedantic
)

add_subdirectory(1)
