cmake_minimum_required(VERSION 3.16)
project(HTTPSERVER VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)#C17不降级，直接报错

set(SOURCES src/main.c src/server.c src/http.c src/file.c src/config.c)

add_executable(HTTPSERVER ${SOURCES})#编译成可执行文件HTTPSERVER

target_include_directories(HTTPSERVER PRIVATE src)#头文件在src

#编译选项
target_compile_options(HTTPSERVER PRIVATE $<$<CONFIG:Release>:-O2> -Wall -Wextra -pedantic)
#定义_GNU_SOURCE 宏，用于暴漏Linux的api
target_compile_definitions(HTTPSERVER PRIVATE _GNU_SOURCE)




