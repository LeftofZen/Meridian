cmake_minimum_required(VERSION 3.20)

project(Meridian
    VERSION 0.1.0
    DESCRIPTION "A next-gen game engine using SDL3 and Vulkan"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Core windowing / rendering
find_package(SDL3 REQUIRED CONFIG)

# Physics — Jolt Physics (multi-threaded, deterministic, C++20-friendly)
find_package(Jolt REQUIRED CONFIG)

# Entity Component System — EnTT (header-only, sparse-set, C++20)
find_package(EnTT REQUIRED CONFIG)

# Networking — GameNetworkingSockets (reliable UDP, NAT traversal, P2P+relay)
find_package(GameNetworkingSockets REQUIRED CONFIG)

# Sparse volumetric data — OpenVDB / NanoVDB (GPU-side fluid & particle storage)
find_package(OpenVDB REQUIRED)

add_executable(Meridian src/main.cpp)

target_link_libraries(Meridian PRIVATE
    SDL3::SDL3
    Jolt::Jolt
    EnTT::EnTT
    GameNetworkingSockets::GameNetworkingSockets
    OpenVDB::openvdb
)
