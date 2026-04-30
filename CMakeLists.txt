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

if(NOT MSVC)
    message(FATAL_ERROR "Meridian supports Visual Studio 2026 with MSVC, or clang-cl in VS Code using the MSVC-compatible toolchain.")
endif()

if(MSVC_VERSION LESS 1950 AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Meridian requires the Visual Studio 2026 toolset (MSVC 19.50 or newer).")
endif()

find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_image CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(unofficial-lmdb CONFIG REQUIRED)
find_package(SQLiteCpp CONFIG REQUIRED)
find_package(steam-audio CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(Tracy CONFIG REQUIRED)
find_package(unofficial-lua CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(RecastNavigation CONFIG REQUIRED)
find_package(Taskflow CONFIG REQUIRED)

find_program(GLSLANG_VALIDATOR NAMES glslangValidator
    HINTS ENV VULKAN_SDK
    PATH_SUFFIXES Bin
)
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator was not found. Install the Vulkan SDK or add glslangValidator to PATH.")
endif()

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.7
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(imgui PUBLIC SDL3::SDL3 volk::volk)
add_library(imgui::imgui ALIAS imgui)

set(MERIDIAN_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")

set(MERIDIAN_SHADER_SOURCES
    shaders/fullscreen_triangle.vert
    shaders/basic_pathtracer.frag
    shaders/pathtracer_denoise.frag
    shaders/pathtracer_svgf_temporal.frag
    shaders/pathtracer_svgf_atrous.frag
    shaders/terrain_heightmap.comp
)

set(MERIDIAN_SHADER_OUTPUTS)
foreach(MERIDIAN_SHADER_SOURCE IN LISTS MERIDIAN_SHADER_SOURCES)
    get_filename_component(MERIDIAN_SHADER_NAME "${MERIDIAN_SHADER_SOURCE}" NAME)
    set(MERIDIAN_SHADER_OUTPUT "${MERIDIAN_SHADER_OUTPUT_DIR}/${MERIDIAN_SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT "${MERIDIAN_SHADER_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MERIDIAN_SHADER_OUTPUT_DIR}"
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.3 -g
                -o "${MERIDIAN_SHADER_OUTPUT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/${MERIDIAN_SHADER_SOURCE}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${MERIDIAN_SHADER_SOURCE}"
        VERBATIM
    )
    list(APPEND MERIDIAN_SHADER_OUTPUTS "${MERIDIAN_SHADER_OUTPUT}")
endforeach()

add_custom_target(MeridianShaders ALL DEPENDS ${MERIDIAN_SHADER_OUTPUTS})

add_executable(Meridian
    src/main.cpp
    src/core/Engine.cpp
    src/input/InputManager.cpp
    src/window/Window.cpp
    src/renderer/DebugOverlayRenderer.cpp
    src/renderer/FreeCameraController.cpp
    src/renderer/PathTracerRenderer.cpp
    src/renderer/RenderFramePipeline.cpp
    src/renderer/ShaderLibrary.cpp
    src/renderer/VulkanContext.cpp
    src/renderer/WorldSceneRenderer.cpp
    src/audio/AudioSystem.cpp
    src/physics/PhysicsSystem.cpp
    src/networking/NetworkSystem.cpp
    src/scripting/ScriptingSystem.cpp
    src/world/ChunkGenerationVisibility.cpp
    src/world/WorldChunkManager.cpp
    src/world/WorldChunkDatabase.cpp
    src/world/SparseVoxelOctree.cpp
    src/world/TerrainHeightmapGenerator.cpp
    src/world/WorldChunkStorage.cpp
    src/world/WorldSpatialHashGrid.cpp
    src/world/World.cpp
)

add_dependencies(Meridian MeridianShaders)

target_include_directories(Meridian PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(Meridian PRIVATE
    MERIDIAN_SHADER_OUTPUT_DIR="${MERIDIAN_SHADER_OUTPUT_DIR}"
    SOL_ALL_SAFETIES_ON=1
    TRACY_ENABLE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(Meridian PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->
)

target_link_libraries(Meridian PRIVATE
    SDL3::SDL3
    SDL3_image::SDL3_image
    imgui::imgui
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
    Tracy::TracyClient
    unofficial::lua::lua
    sol2::sol2
    RecastNavigation::Recast
    RecastNavigation::Detour
    Taskflow::Taskflow
)

if(WIN32)
    add_custom_command(TARGET Meridian POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:Meridian>
            $<TARGET_FILE_DIR:Meridian>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/meridian-icon.png"
            $<TARGET_FILE_DIR:Meridian>/meridian-icon.png
        COMMAND_EXPAND_LISTS
    )
endif()
