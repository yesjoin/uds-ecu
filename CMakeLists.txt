cmake_minimum_required(VERSION 3.16)
project(uds_ecu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

# M0：SocketCAN 收帧 + 收发解耦
add_executable(uds-ecu src/CanBus.cpp src/main.cpp)
target_include_directories(uds-ecu PRIVATE include)
target_link_libraries(uds-ecu PRIVATE Threads::Threads)

# M1a：ISO-TP 重组的纯内存自测（不碰 vcan）
add_executable(isotp-selftest tests/isotp_selftest.cpp src/IsoTp.cpp)
target_include_directories(isotp-selftest PRIVATE include)
target_link_libraries(isotp-selftest PRIVATE Threads::Threads)

# M1b-2：真在 vcan0 上收发（两个进程各拿一个 socket）
add_executable(isotp-echo tools/isotp_echo.cpp src/CanBus.cpp src/IsoTp.cpp)
target_include_directories(isotp-echo PRIVATE include)
target_link_libraries(isotp-echo PRIVATE Threads::Threads)
