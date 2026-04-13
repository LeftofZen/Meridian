#pragma once

#include "world/WorldData.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Meridian {

class SparseVoxelOctree final {
public:
    static constexpr std::uint32_t kInvalidChildIndex = UINT32_MAX;

    struct Node {
        std::array<std::uint32_t, 8> children{
            kInvalidChildIndex,
            kInvalidChildIndex,
            kInvalidChildIndex,
            kInvalidChildIndex,
            kInvalidChildIndex,
            kInvalidChildIndex,
            kInvalidChildIndex,
            kInvalidChildIndex};
        float maxDensity{0.0F};
        std::uint8_t childMask{0};
        std::uint8_t materialId{0};
        bool leaf{true};
    };

    struct BuildStats {
        std::size_t nodeCount{0};
        std::size_t leafCount{0};
        std::size_t solidVoxelCount{0};
        std::uint32_t depth{0};
    };

    SparseVoxelOctree() = default;

    [[nodiscard]] static SparseVoxelOctree build(
        std::span<const VoxelSample> voxels,
        std::uint32_t resolution);

    [[nodiscard]] bool empty() const noexcept { return m_nodes.empty(); }
    [[nodiscard]] const std::vector<Node>& nodes() const noexcept { return m_nodes; }
    [[nodiscard]] const BuildStats& stats() const noexcept { return m_stats; }

private:
    [[nodiscard]] static std::uint32_t buildNode(
        SparseVoxelOctree& octree,
        std::span<const VoxelSample> voxels,
        std::uint32_t resolution,
        std::uint32_t baseX,
        std::uint32_t baseY,
        std::uint32_t baseZ,
        std::uint32_t extent,
        std::uint32_t depth);

    std::vector<Node> m_nodes;
    BuildStats m_stats;
};

} // namespace Meridian