cmake_minimum_required(VERSION 3.20)
project(Ztarry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Ztarry
    src/main.cpp
    src/curses_compat.cpp
)

add_executable(installer WIN32
    src/installer/main.cpp
)

set_target_properties(installer PROPERTIES OUTPUT_NAME "installer")

target_include_directories(Ztarry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(Ztarry PRIVATE pdh psapi iphlpapi)
target_link_libraries(installer PRIVATE urlmon shell32 advapi32)

if(MSVC)
    target_compile_options(Ztarry PRIVATE /W4 /permissive-)
    target_compile_options(installer PRIVATE /W4 /permissive-)
else()
    target_compile_options(Ztarry PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(installer PRIVATE -Wall -Wextra -Wpedantic)
endif()
