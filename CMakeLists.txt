cmake_minimum_required(VERSION 3.10)

project(web C)

set(CMAKE_C_STANDARD 17)

set(CMAKE_C_FLAGS_DEBUG   "-g -O0 -Wall -Wextra -Wpedantic -fsanitize=address,undefined")
set(CMAKE_C_FLAGS_RELEASE "-g -O0 -Wall -Wextra")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB SRC_LIST "src/*.c")

add_executable(web ${SRC_LIST})

target_include_directories(web PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ── 依赖库查找 ──────────────────────────────────
# 需要的系统库：pthread（POSIX 线程）、sqlite3、ssl、crypto
#
# pthread 是 libc 一部分，无需额外操作。
# sqlite3 和 openssl 需要系统中安装对应的开发包：
#   Ubuntu/Debian: sudo apt install libsqlite3-dev libssl-dev
#   Fedora/RHEL:   sudo dnf install sqlite-devel openssl-devel
#   Arch:          sudo pacman -S sqlite openssl
#
# 若 CMake 找不到，请设置 CMAKE_PREFIX_PATH 指向安装位置。

# 尝试 pkg-config（Linux 下最可靠）
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
    pkg_check_modules(OPENSSL REQUIRED openssl)
    target_include_directories(web PRIVATE ${SQLITE3_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS})
    target_link_libraries(web PRIVATE pthread ${SQLITE3_LIBRARIES} ${OPENSSL_LIBRARIES})
else()
    # 回退：手动查找（适用于没有 pkg-config 的环境）
    find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
    find_library(SQLITE3_LIBRARY sqlite3)
    find_path(OPENSSL_INCLUDE_DIR openssl/ssl.h)
    find_library(OPENSSL_SSL_LIBRARY ssl)
    find_library(OPENSSL_CRYPTO_LIBRARY crypto)

    if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
        message(FATAL_ERROR
            "sqlite3 not found.\n"
            "  Install it first:\n"
            "    Ubuntu/Debian: sudo apt install libsqlite3-dev\n"
            "    Fedora/RHEL:   sudo dnf install sqlite-devel\n"
            "  Or set SQLITE3_INCLUDE_DIR / SQLITE3_LIBRARY manually.")
    endif()
    if(NOT OPENSSL_INCLUDE_DIR OR NOT OPENSSL_SSL_LIBRARY OR NOT OPENSSL_CRYPTO_LIBRARY)
        message(FATAL_ERROR
            "openssl not found.\n"
            "  Install it first:\n"
            "    Ubuntu/Debian: sudo apt install libssl-dev\n"
            "    Fedora/RHEL:   sudo dnf install openssl-devel\n"
            "  Or set OPENSSL_INCLUDE_DIR / OPENSSL_{SSL,CRYPTO}_LIBRARY manually.")
    endif()

    target_include_directories(web PRIVATE
        ${SQLITE3_INCLUDE_DIR} ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(web PRIVATE pthread
        ${SQLITE3_LIBRARY} ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY})
endif()