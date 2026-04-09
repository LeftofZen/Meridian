cmake_minimum_required(VERSION 3.20)

project(Meridian
    VERSION 0.1.0
    DESCRIPTION "A next-gen game engine using SDL3 and Vulkan"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SDL3 REQUIRED CONFIG)

add_executable(Meridian src/main.cpp)

target_link_libraries(Meridian PRIVATE SDL3::SDL3)
