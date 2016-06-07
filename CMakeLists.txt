cmake_minimum_required(VERSION 3.3)
project(sik_chat)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -g")

set(SERVER_SOURCE_FILES server.cpp)
add_executable(server ${SERVER_SOURCE_FILES})

set(CLIENT_SOURCE_FILES client.cpp)
add_executable(client ${CLIENT_SOURCE_FILES})