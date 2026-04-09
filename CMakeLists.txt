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

# World persistence: voxel chunks
find_package(unofficial-lmdb CONFIG REQUIRED)

# World persistence: entity/game state (pulls in sqlite3)
find_package(SQLiteCpp REQUIRED)

# Spatial audio — Steam Audio (HRTF, physics-based occlusion/reverb via ray casting)
find_package(steam-audio CONFIG REQUIRED)

# Math — GLM (GLSL-compatible, header-only)
find_package(glm CONFIG REQUIRED)

# Logging — spdlog (async, multi-sink)
find_package(spdlog CONFIG REQUIRED)

# Serialization — FlatBuffers (zero-copy, schema-driven)
find_package(flatbuffers CONFIG REQUIRED)

# Compression — zstd (chunk streaming and on-disk storage)
find_package(zstd CONFIG REQUIRED)

# Debug UI — Dear ImGui (Vulkan + SDL3 backends)
find_package(imgui CONFIG REQUIRED)

# Scripting — Lua + sol2 bindings
find_package(unofficial-lua CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)

# AI / Navigation — Recast & Detour (navmesh generation and pathfinding)
find_package(RecastNavigation CONFIG REQUIRED)

# Task scheduling — Taskflow (C++17 work-stealing task graph)
find_package(Taskflow CONFIG REQUIRED)

add_executable(Meridian src/main.cpp)

target_link_libraries(Meridian PRIVATE
    SDL3::SDL3
    Jolt::Jolt
    EnTT::EnTT
    GameNetworkingSockets::GameNetworkingSockets
    OpenVDB::openvdb
    unofficial::lmdb::lmdb
    SQLiteCpp
    unofficial::steam-audio::phonon
    glm::glm
    spdlog::spdlog
    flatbuffers::flatbuffers
    zstd::libzstd
    imgui::imgui
    unofficial::lua::lua
    sol2::sol2
    RecastNavigation::Recast
    RecastNavigation::Detour
    Taskflow::Taskflow
)
