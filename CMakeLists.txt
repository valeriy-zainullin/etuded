cmake_minimum_required(VERSION 3.14 FATAL_ERROR)

project(etude-lsp-server)
set(CMAKE_CXX_STANDARD 20)

include(cmake/CPM.cmake)

add_executable(server server.cpp)

CPMAddPackage("gh:kuafuwang/LspCpp#e0b443d42e7d23638d727ac8ef6839b9e527bf0a")
target_link_libraries(server PRIVATE lspcpp)

