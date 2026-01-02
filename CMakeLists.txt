cmake_minimum_required(VERSION 3.16)
project(blockchain C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

find_package(Threads REQUIRED)

add_executable(blockchain
    src/main.c
    src/txpool.c
    src/utils.c
    src/crypto.c
    src/blockchain.c
    src/network.c
    src/consensus.c
    src/control.c
    src/tx_builder.c
    src/log.c
)

target_include_directories(blockchain PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(blockchain PRIVATE
    PkgConfig::SODIUM
    Threads::Threads
)
