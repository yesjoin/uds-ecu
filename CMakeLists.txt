cmake_minimum_required(VERSION 3.16)
project(uds_ecu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

add_executable(uds-ecu src/CanBus.cpp src/main.cpp)
target_include_directories(uds-ecu PRIVATE include)
target_link_libraries(uds-ecu PRIVATE Threads::Threads)
