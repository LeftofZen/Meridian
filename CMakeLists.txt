cmake_minimum_required(VERSION 4.2.0)

project(Meridian
    VERSION 0.1.0
    DESCRIPTION "A next-gen game engine using SDL3 and Vulkan"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT MSVC)
    message(FATAL_ERROR "Meridian supports Visual Studio 2026 with MSVC, or clang-cl in VS Code using the MSVC-compatible toolchain.")
endif()

if (MSVC_VERSION LESS 1950)
    message(FATAL_ERROR "Meridian requires the Visual Studio 2026 toolset (MSVC 19.50 or newer).")
endif()

find_package(SDL3 CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(unofficial-lmdb CONFIG REQUIRED)
find_package(SQLiteCpp REQUIRED)
find_package(steam-audio CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(unofficial-lua CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(RecastNavigation CONFIG REQUIRED)
find_package(Taskflow CONFIG REQUIRED)

add_executable(Meridian)

target_sources(Meridian PRIVATE
    src/main.cpp
    src/core/Engine.cpp
    src/window/Window.cpp
    src/renderer/VulkanContext.cpp
    src/audio/AudioSystem.cpp
    src/physics/PhysicsSystem.cpp
    src/networking/NetworkSystem.cpp
    src/scripting/ScriptingSystem.cpp
)

target_include_directories(Meridian PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(Meridian PRIVATE
    SDL3::SDL3
    volk::volk
    Jolt::Jolt
    EnTT::EnTT
    GameNetworkingSockets::GameNetworkingSockets
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

if (MSVC)
    target_compile_options(Meridian PRIVATE /W4 /permissive-)
endif()

target_compile_definitions(Meridian PRIVATE
    SOL_ALL_SAFETIES_ON=1
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
