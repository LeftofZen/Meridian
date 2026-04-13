#include "world/WorldChunkManager.hpp"

#include "core/Logger.hpp"
#include "world/SparseVoxelOctree.hpp"
#include "world/WorldChunkStorage.hpp"

#include "tasks/TaskSystem.hpp"

#include <algorithm>
#include <chrono>

namespace Meridian {

namespace {

[[nodiscard]] VoxelMaterialId defaultMaterialForChunk(ChunkCoord coord) noexcept
{
    if (coord.y < 0) {
        return VoxelMaterialId::Stone;
    }

    if (coord.y == 0) {
        return VoxelMaterialId::Grass;
    }

    return VoxelMaterialId::Air;
}

[[nodiscard]] const char* voxelMaterialName(VoxelMaterialId material) noexcept
{
    switch (material) {
    case VoxelMaterialId::Stone:
        return "stone";
    case VoxelMaterialId::Grass:
        return "grass";
    case VoxelMaterialId::Air:
        return "air";
    default:
        return "unknown";
    }
}

} // namespace

WorldChunkManager::WorldChunkManager(TaskSystem& taskSystem) noexcept : m_tasks(taskSystem) {}

bool WorldChunkManager::init()
{
    if (!m_tasks.isInitialised()) {
        MRD_ERROR("WorldChunkManager init failed: TaskSystem is not initialised");
        return false;
    }

    m_initialised = true;
    MRD_INFO("WorldChunkManager initialized (chunk resolution: {}^3 voxels)",
        kWorldChunkResolution);
    return true;
}

void WorldChunkManager::shutdown()
{
    if (!m_initialised) {
        return;
    }

    m_pendingRequests.clear();
    m_inFlightJobs.clear();
    m_chunkRecords.clear();
    m_residentChunks.clear();
    m_bootstrapRequested = false;
    m_initialised = false;
    MRD_INFO("WorldChunkManager shutting down");
}

void WorldChunkManager::update(float /*deltaTimeSeconds*/)
{
    if (!m_initialised) {
        return;
    }

    if (!m_bootstrapRequested) {
        bootstrapChunkRequests();
    }

    dispatchChunkJobs();
    collectCompletedJobs();
}

std::size_t WorldChunkManager::getResidentChunkCount() const noexcept
{
    return m_residentChunks.size();
}

std::size_t WorldChunkManager::getInFlightChunkCount() const noexcept
{
    return m_inFlightJobs.size();
}

std::size_t WorldChunkManager::getPendingChunkCount() const noexcept
{
    return m_pendingRequests.size();
}

void WorldChunkManager::bootstrapChunkRequests()
{
    for (int y = -kBootstrapRadius; y <= kBootstrapRadius; ++y) {
        for (int z = -kBootstrapRadius; z <= kBootstrapRadius; ++z) {
            for (int x = -kBootstrapRadius; x <= kBootstrapRadius; ++x) {
                requestChunk(ChunkCoord{.x = x, .y = y, .z = z});
            }
        }
    }

    m_bootstrapRequested = true;
    MRD_INFO("World chunk manager queued {} bootstrap chunks", m_pendingRequests.size());
}

void WorldChunkManager::requestChunk(ChunkCoord coord)
{
    const ChunkKey key = makeChunkKey(coord);
    if (m_chunkRecords.contains(key)) {
        return;
    }

    m_chunkRecords.emplace(key, ChunkRecord{
        .coord = coord,
        .key = key,
        .status = ChunkStatus::Requested,
    });
    m_pendingRequests.push_back(coord);
}

void WorldChunkManager::dispatchChunkJobs()
{
    while (!m_pendingRequests.empty() && m_inFlightJobs.size() < kMaxConcurrentJobs) {
        const ChunkCoord coord = m_pendingRequests.front();
        m_pendingRequests.pop_front();

        const ChunkKey key = makeChunkKey(coord);
        auto chunkIt = m_chunkRecords.find(key);
        if (chunkIt == m_chunkRecords.end() || chunkIt->second.status != ChunkStatus::Requested) {
            continue;
        }

        chunkIt->second.status = ChunkStatus::Generating;

        m_inFlightJobs.push_back(ChunkJob{
            .coord = coord,
            .key = key,
            .future = m_tasks.async([coord]() {
                return generateChunk(coord);
            }),
        });
    }
}

void WorldChunkManager::collectCompletedJobs()
{
    using namespace std::chrono_literals;

    auto nextJob = std::remove_if(
        m_inFlightJobs.begin(),
        m_inFlightJobs.end(),
        [this](ChunkJob& job) {
            if (job.future.wait_for(0ms) != std::future_status::ready) {
                return false;
            }

            GeneratedChunk generatedChunk = job.future.get();
            auto chunkIt = m_chunkRecords.find(job.key);
            if (chunkIt == m_chunkRecords.end()) {
                return true;
            }

            ChunkRecord& record = chunkIt->second;
            record.status = ChunkStatus::Resident;
            m_residentChunks.upsert(std::move(generatedChunk.chunkStorage));

            const WorldChunkStorage* residentChunk = m_residentChunks.find(job.key);
            if (residentChunk == nullptr) {
                return true;
            }

            MRD_INFO(
                "World chunk [{}, {}, {}] ready (material {}, {}^3 voxels, solid {}, svo nodes {}, resident: {})",
                residentChunk->coord().x,
                residentChunk->coord().y,
                residentChunk->coord().z,
                voxelMaterialName(defaultMaterialForChunk(residentChunk->coord())),
                residentChunk->voxelResolution(),
                residentChunk->solidVoxelCount(),
                residentChunk->octree().stats().nodeCount,
                getResidentChunkCount());
            return true;
        });

    m_inFlightJobs.erase(nextJob, m_inFlightJobs.end());
}

WorldChunkManager::GeneratedChunk WorldChunkManager::generateChunk(
    ChunkCoord coord)
{
    const ChunkKey key = makeChunkKey(coord);
    std::vector<VoxelSample> voxels = generateDefaultChunkVoxels(coord);
    SparseVoxelOctree octree =
        SparseVoxelOctree::build(voxels, kWorldChunkResolution);

    return GeneratedChunk{
        .chunkStorage = WorldChunkStorage(
            coord,
            key,
            kWorldChunkResolution,
            std::move(voxels),
            std::move(octree)),
    };
}

std::vector<VoxelSample> WorldChunkManager::generateDefaultChunkVoxels(ChunkCoord coord)
{
    const VoxelMaterialId material = defaultMaterialForChunk(coord);
    std::vector<VoxelSample> voxels(
        static_cast<std::size_t>(kWorldChunkResolution) *
        kWorldChunkResolution *
        kWorldChunkResolution);

    if (material == VoxelMaterialId::Air) {
        return voxels;
    }

    const float density = 1.0F;
    const std::uint8_t materialId = static_cast<std::uint8_t>(material);
    for (VoxelSample& voxel : voxels) {
        voxel.density = density;
        voxel.materialId = materialId;
    }

    return voxels;
}

} // namespace Meridian