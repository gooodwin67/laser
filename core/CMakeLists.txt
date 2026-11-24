cmake_minimum_required(VERSION 3.20)
project(LaserCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV CONFIG REQUIRED)

add_executable(laser-core main.cpp)
target_include_directories(laser-core PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(laser-core PRIVATE ${OpenCV_LIBS} ws2_32 crypt32 advapi32)

if(MSVC)
    target_compile_options(laser-core PRIVATE /W4 /utf-8)
endif()
