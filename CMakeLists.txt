cmake_minimum_required(VERSION 3.10)
project(network_rate_limiting_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 服务器
add_executable(server src/server.cpp)
target_include_directories(server PRIVATE include)

# 客户端
add_executable(client src/client.cpp)

# 测试
add_executable(test_server tests/test_server.cpp)
target_include_directories(test_server PRIVATE include)
target_link_libraries(test_server pthread)

# ctest 集成：通过 ctest 或 make test 运行
enable_testing()
add_test(NAME server_tests
         COMMAND test_server
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# 安装目标
install(TARGETS server client DESTINATION bin)
