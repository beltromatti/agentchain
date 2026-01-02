cmake_minimum_required(VERSION 3.16)
project(blockchain C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

find_package(Threads REQUIRED)

add_executable(app
    src/main.c
    src/txpool.c
    src/utils.c
    src/crypto.c
)

target_include_directories(app PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(app PRIVATE
    PkgConfig::SODIUM
    Threads::Threads
)