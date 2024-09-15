set(TARGET_NAME pa2)
file(GLOB_RECURSE HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/**.h)
file(GLOB_RECURSE SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/**.c)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_executable(${TARGET_NAME} ${SOURCES} ${HEADERS} pa2/process.h)
target_link_libraries(${TARGET_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/pa2/lib64/libruntime.so)

execute_process(COMMAND uname -m COMMAND tr -d '\n' OUTPUT_VARIABLE ARCHITECTURE)
message(STATUS "Architecture: ${ARCHITECTURE}")
