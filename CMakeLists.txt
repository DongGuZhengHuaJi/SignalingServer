cmake_minimum_required(VERSION 3.10)
project(SignalingServer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(Boost REQUIRED COMPONENTS system thread)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)


include_directories(${Boost_INCLUDE_DIRS}
                    ${nlohmann_json_INCLUDE_DIRS}
)

add_executable(signaling_server                 
                signaling_server.cpp
                thread_pool.hpp
)

target_link_libraries(signaling_server 
                    ${Boost_LIBRARIES}
                    ${nlohmann_json_LIBRARIES}
                    Threads::Threads
)
