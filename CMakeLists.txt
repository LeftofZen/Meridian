cmake_minimum_required(VERSION 4.2.0)

project(Meridian
    VERSION 0.1.0
    DESCRIPTION "A next-gen game engine using SDL3 and Vulkan"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

if (NOT MSVC)
    message(FATAL_ERROR "Meridian supports Visual Studio 2026 with MSVC, or clang-cl in VS Code using the MSVC-compatible toolchain.")
endif()

if (MSVC_VERSION LESS 1950 AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
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

set(MERIDIAN_VCPKG_ROOT "")
if (DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/buildtrees/imgui/src")
    set(MERIDIAN_VCPKG_ROOT "$ENV{VCPKG_ROOT}")
elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/buildtrees/imgui/src")
    set(MERIDIAN_VCPKG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg")
endif()

file(GLOB IMGUI_SOURCE_ROOTS "${MERIDIAN_VCPKG_ROOT}/buildtrees/imgui/src/v*.clean")
list(SORT IMGUI_SOURCE_ROOTS COMPARE NATURAL ORDER DESCENDING)
list(LENGTH IMGUI_SOURCE_ROOTS IMGUI_SOURCE_ROOT_COUNT)
if (IMGUI_SOURCE_ROOT_COUNT GREATER 0)
    list(GET IMGUI_SOURCE_ROOTS 0 IMGUI_SOURCE_ROOT)
    set(MERIDIAN_IMGUI_SOURCES
        "${IMGUI_SOURCE_ROOT}/imgui.cpp"
        "${IMGUI_SOURCE_ROOT}/imgui_draw.cpp"
        "${IMGUI_SOURCE_ROOT}/imgui_tables.cpp"
        "${IMGUI_SOURCE_ROOT}/imgui_widgets.cpp"
        "${IMGUI_SOURCE_ROOT}/backends/imgui_impl_sdl3.cpp"
        "${IMGUI_SOURCE_ROOT}/backends/imgui_impl_vulkan.cpp"
    )
endif()

target_sources(Meridian PRIVATE
    src/main.cpp
    src/core/Engine.cpp
    src/window/Window.cpp
    src/renderer/VulkanContext.cpp
    src/audio/AudioSystem.cpp
    src/physics/PhysicsSystem.cpp
    src/networking/NetworkSystem.cpp
    src/scripting/ScriptingSystem.cpp
    src/world/World.cpp
    ${MERIDIAN_IMGUI_SOURCES}
)

target_include_directories(Meridian PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (IMGUI_SOURCE_ROOT_COUNT GREATER 0)
    target_include_directories(Meridian PRIVATE
        "${IMGUI_SOURCE_ROOT}"
        "${IMGUI_SOURCE_ROOT}/backends"
    )
endif()

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
    unofficial::lua::lua
    sol2::sol2
    RecastNavigation::Recast
    RecastNavigation::Detour
    Taskflow::Taskflow
)

if (IMGUI_SOURCE_ROOT_COUNT EQUAL 0)
    target_link_libraries(Meridian PRIVATE imgui::imgui)
endif()

if (MSVC)
    target_compile_options(Meridian PRIVATE /W4 /permissive-)
endif()

target_compile_definitions(Meridian PRIVATE
    IMGUI_IMPL_VULKAN_USE_VOLK=1
    SOL_ALL_SAFETIES_ON=1
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
