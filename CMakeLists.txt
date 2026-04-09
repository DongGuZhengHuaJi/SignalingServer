cmake_minimum_required(VERSION 3.10)
project(SignalingServer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)



include_directories(
    "/usr/local/include"            # redis++ 的位置
    "/usr/include/hiredis"          # hiredis 的位置
    "/usr/include/mysqlconn"        # MySQL 的位置
    "${CMAKE_CURRENT_SOURCE_DIR}"   # 你自己的项目根目录
)

# ==========================================
# 2. 直接指定库文件的位置 (链接器找 .so)
# ==========================================
# 这里直接把库的绝对路径存到变量里
set(REDIS_PLUS_LIB "/usr/local/lib/libredis++.so")
set(HIREDIS_LIB     "/usr/lib/x86_64-linux-gnu/libhiredis.so")
set(MYSQL_LIB       "/usr/lib/x86_64-linux-gnu/libmysqlcppconn.so")


# 1. 查找依赖
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

# 2. 创建目标
add_executable(signaling_server                 
    signaling_server.cpp
    thread_pool.hpp
    redis_mgr.h
)



# 3. 使用 Target 方式链接（不再需要 include_directories）
# 这种写法会自动处理包含路径和依赖关系，不会死循环
target_link_libraries(signaling_server
    PRIVATE
    Boost::system
    Boost::thread
    nlohmann_json::nlohmann_json
    Threads::Threads
    ${REDIS_PLUS_LIB}
    ${HIREDIS_LIB}
    ${MYSQL_LIB}
)