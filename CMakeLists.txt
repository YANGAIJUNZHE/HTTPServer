cmake_minimum_required(VERSION 3.10)

project(web C)

set(CMAKE_C_STANDARD 17)

# 编译选项
set(CMAKE_C_FLAGS_DEBUG   "-g -O0 -Wall -Wextra -Wpedantic -fsanitize=address,undefined")
set(CMAKE_C_FLAGS_RELEASE "-O0 -DNDEBUG -Wall -Wextra")

# 默认 Release，可通过 cmake -DCMAKE_BUILD_TYPE=Debug 切换
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB SRC_LIST "src/*.c")

add_executable(web ${SRC_LIST})

target_include_directories(web PRIVATE ${CMAKE_SOURCE_DIR}/src)