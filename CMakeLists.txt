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

# M2：UDS 服务层的纯内存自测
add_executable(uds-selftest tests/uds_selftest.cpp src/Uds.cpp)
target_include_directories(uds-selftest PRIVATE include)
target_link_libraries(uds-selftest PRIVATE Threads::Threads)

# M2：虚拟 ECU + 测试仪，真在 vcan0 上对话（完整栈：CAN → ISO-TP → UDS）
add_executable(uds-demo tools/uds_demo.cpp src/CanBus.cpp src/IsoTp.cpp src/Uds.cpp)
target_include_directories(uds-demo PRIVATE include)
target_link_libraries(uds-demo PRIVATE Threads::Threads)

# M5：200 条测试矩阵 + 报告导出（csv / markdown）
add_executable(uds-matrix tests/uds_matrix.cpp src/Uds.cpp)
target_include_directories(uds-matrix PRIVATE include)
target_link_libraries(uds-matrix PRIVATE Threads::Threads)
