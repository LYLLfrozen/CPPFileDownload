cmake_minimum_required(VERSION 3.16)
project(FileDownload LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(file_server
    src/server.cpp
    src/common.cpp
)

target_include_directories(file_server PRIVATE src)
target_compile_definitions(file_server PRIVATE FD_WEB_ROOT="${CMAKE_SOURCE_DIR}/web")

if(WIN32)
    target_link_libraries(file_server PRIVATE ws2_32)
endif()
