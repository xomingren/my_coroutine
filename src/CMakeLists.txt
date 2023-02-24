cmake_minimum_required (VERSION 3.8)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)

project(my_coroutine)

add_subdirectory(coroutine)
add_subdirectory(tests)


