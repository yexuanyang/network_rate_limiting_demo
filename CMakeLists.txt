cmake_minimum_required(VERSION 3.10)
project(network_rate_limiting_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 服务器
add_executable(server src/server.cpp)
target_include_directories(server PRIVATE include)

# 客户端
add_executable(client src/client.cpp)

# 安装目标
install(TARGETS server client DESTINATION bin)
