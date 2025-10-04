cmake_minimum_required(VERSION 4.0)
project(tlog C)

set(CMAKE_C_STANDARD 11)

include_directories(cpp)
include_directories(c)

add_executable(tlog c/tlog.c
        main.c)
