cmake_minimum_required (VERSION 3.8)

aux_source_directory(. ALL_SRCS)

add_library(coroutine ${ALL_SRCS})