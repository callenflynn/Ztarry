cmake_minimum_required(VERSION 3.20)
project(Ztarry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Ztarry
    src/main.cpp
    src/curses_compat.cpp
)

target_include_directories(Ztarry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(Ztarry PRIVATE pdh psapi iphlpapi)

if(MSVC)
    target_compile_options(Ztarry PRIVATE /W4 /permissive-)
else()
    target_compile_options(Ztarry PRIVATE -Wall -Wextra -Wpedantic)
endif()
