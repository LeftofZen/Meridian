#pragma once

#include <bit>
#include <cstdint>

namespace Meridian {

struct ChunkCoord {
    int x{0};
    int y{0};
    int z{0};

    [[nodiscard]] bool operator==(const ChunkCoord&) const noexcept = default;
};

using ChunkKey = std::uint64_t;

struct VoxelSample {
    float density{0.0F};
    std::uint8_t materialId{0};
};

enum class VoxelMaterialId : std::uint8_t {
    Air = 0,
    Grass = 1,
    Stone = 2,
    Dirt = 3,
    Sand = 4,
    Snow = 5,
    Forest = 6,
};

inline constexpr std::uint32_t kWorldChunkResolution = 32;
static_assert(std::has_single_bit(kWorldChunkResolution));

inline constexpr int kWorldChunkSize = static_cast<int>(kWorldChunkResolution);

[[nodiscard]] inline constexpr ChunkKey makeChunkKey(ChunkCoord coord) noexcept
{
    constexpr std::uint64_t axisMask = (1ULL << 21U) - 1ULL;
    constexpr int axisBias = 1 << 20;

    const auto packAxis = [](int value) -> std::uint64_t {
        return static_cast<std::uint64_t>(value + axisBias) & axisMask;
    };

    return (packAxis(coord.x) << 42U) |
           (packAxis(coord.y) << 21U) |
           packAxis(coord.z);
}

} // namespace Meridian