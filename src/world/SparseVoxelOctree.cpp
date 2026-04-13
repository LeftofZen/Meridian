#include "world/SparseVoxelOctree.hpp"

#include <algorithm>
#include <bit>

namespace Meridian {

SparseVoxelOctree SparseVoxelOctree::build(
    std::span<const VoxelSample> voxels,
    std::uint32_t resolution)
{
    SparseVoxelOctree octree;
    if (voxels.empty() || resolution == 0 || !std::has_single_bit(resolution)) {
        return octree;
    }

    octree.m_stats.depth = std::bit_width(resolution);
    const std::uint32_t rootIndex = buildNode(
        octree,
        voxels,
        resolution,
        0,
        0,
        0,
        resolution,
        0);

    if (rootIndex == kInvalidChildIndex) {
        octree.m_nodes.clear();
        octree.m_stats = {};
        return octree;
    }

    octree.m_stats.nodeCount = octree.m_nodes.size();
    return octree;
}

std::uint32_t SparseVoxelOctree::buildNode(
    SparseVoxelOctree& octree,
    std::span<const VoxelSample> voxels,
    std::uint32_t resolution,
    std::uint32_t baseX,
    std::uint32_t baseY,
    std::uint32_t baseZ,
    std::uint32_t extent,
    std::uint32_t depth)
{
    const auto voxelIndex = [resolution](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return (static_cast<std::size_t>(z) * resolution * resolution) +
               (static_cast<std::size_t>(y) * resolution) + x;
    };

    if (extent == 1) {
        const VoxelSample& voxel = voxels[voxelIndex(baseX, baseY, baseZ)];
        if (voxel.density <= 0.0F || voxel.materialId == 0U) {
            return kInvalidChildIndex;
        }

        Node leafNode{};
        leafNode.maxDensity = voxel.density;
        leafNode.materialId = voxel.materialId;
        leafNode.leaf = true;

        const std::uint32_t nodeIndex = static_cast<std::uint32_t>(octree.m_nodes.size());
        octree.m_nodes.push_back(leafNode);
        ++octree.m_stats.leafCount;
        ++octree.m_stats.solidVoxelCount;
        octree.m_stats.depth = std::max(octree.m_stats.depth, depth + 1U);
        return nodeIndex;
    }

    Node node{};
    node.leaf = false;

    const std::uint32_t childExtent = extent / 2U;
    for (std::uint32_t childIndex = 0; childIndex < 8; ++childIndex) {
        const std::uint32_t offsetX = (childIndex & 1U) != 0U ? childExtent : 0U;
        const std::uint32_t offsetY = (childIndex & 2U) != 0U ? childExtent : 0U;
        const std::uint32_t offsetZ = (childIndex & 4U) != 0U ? childExtent : 0U;

        const std::uint32_t builtChildIndex = buildNode(
            octree,
            voxels,
            resolution,
            baseX + offsetX,
            baseY + offsetY,
            baseZ + offsetZ,
            childExtent,
            depth + 1U);

        if (builtChildIndex == kInvalidChildIndex) {
            continue;
        }

        node.children[childIndex] = builtChildIndex;
        node.childMask = static_cast<std::uint8_t>(node.childMask | (1U << childIndex));
        node.maxDensity = std::max(node.maxDensity, octree.m_nodes[builtChildIndex].maxDensity);
    }

    if (node.childMask == 0U) {
        return kInvalidChildIndex;
    }

    const std::uint32_t nodeIndex = static_cast<std::uint32_t>(octree.m_nodes.size());
    octree.m_nodes.push_back(node);
    octree.m_stats.depth = std::max(octree.m_stats.depth, depth + 1U);
    return nodeIndex;
}

} // namespace Meridian