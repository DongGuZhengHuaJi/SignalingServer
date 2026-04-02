cmake_minimum_required(VERSION 3.10)
project(SignalingServer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 1. 查找依赖
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

# 2. 创建目标
add_executable(signaling_server                 
    signaling_server.cpp
    thread_pool.hpp
)

# 3. 使用 Target 方式链接（不再需要 include_directories）
# 这种写法会自动处理包含路径和依赖关系，不会死循环
target_link_libraries(signaling_server
    PRIVATE
    Boost::system
    Boost::thread
    nlohmann_json::nlohmann_json
    Threads::Threads
)