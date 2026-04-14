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

    octree.m_stats = {};
    accumulateBuildStats(octree, rootIndex, resolution, 0U, octree.m_stats);
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
        return nodeIndex;
    }

    Node node{};
    node.leaf = false;
    const std::size_t subtreeStartIndex = octree.m_nodes.size();

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

    if (node.childMask == 0xFFU) {
        const Node& firstChild = octree.m_nodes[node.children[0]];
        if (firstChild.leaf) {
            const std::uint8_t collapsedMaterialId = firstChild.materialId;
            bool canCollapse = collapsedMaterialId != 0U;
            for (std::uint32_t childIndex = 1U; childIndex < 8U && canCollapse; ++childIndex) {
                const Node& childNode = octree.m_nodes[node.children[childIndex]];
                canCollapse =
                    childNode.leaf &&
                    childNode.materialId == collapsedMaterialId;
            }

            if (canCollapse) {
                octree.m_nodes.resize(subtreeStartIndex);

                Node leafNode{};
                leafNode.maxDensity = node.maxDensity;
                leafNode.materialId = collapsedMaterialId;
                leafNode.leaf = true;

                const std::uint32_t nodeIndex = static_cast<std::uint32_t>(octree.m_nodes.size());
                octree.m_nodes.push_back(leafNode);
                return nodeIndex;
            }
        }
    }

    const std::uint32_t nodeIndex = static_cast<std::uint32_t>(octree.m_nodes.size());
    octree.m_nodes.push_back(node);
    return nodeIndex;
}

void SparseVoxelOctree::accumulateBuildStats(
    const SparseVoxelOctree& octree,
    std::uint32_t nodeIndex,
    std::uint32_t extent,
    std::uint32_t depth,
    BuildStats& stats) noexcept
{
    const Node& node = octree.m_nodes[nodeIndex];
    ++stats.nodeCount;
    stats.depth = std::max(stats.depth, depth + 1U);

    if (node.leaf) {
        ++stats.leafCount;
        stats.solidVoxelCount +=
            static_cast<std::size_t>(extent) *
            static_cast<std::size_t>(extent) *
            static_cast<std::size_t>(extent);
        return;
    }

    const std::uint32_t childExtent = extent / 2U;
    for (std::uint32_t childIndex = 0U; childIndex < 8U; ++childIndex) {
        if ((node.childMask & (1U << childIndex)) == 0U) {
            continue;
        }

        const std::uint32_t childNodeIndex = node.children[childIndex];
        if (childNodeIndex == kInvalidChildIndex) {
            continue;
        }

        accumulateBuildStats(octree, childNodeIndex, childExtent, depth + 1U, stats);
    }
}

} // namespace Meridian