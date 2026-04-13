#include "world/WorldChunkManager.hpp"

#include "core/Logger.hpp"
#include "world/TerrainHeightmapGenerator.hpp"
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

[[nodiscard]] ChunkKey heightmapTileKey(ChunkCoord coord) noexcept
{
    return makeChunkKey(ChunkCoord{.x = coord.x, .y = 0, .z = coord.z});
}

} // namespace

WorldChunkManager::WorldChunkManager(
    TaskSystem& taskSystem,
    TerrainHeightmapGenerator* heightmapGenerator) noexcept
    : m_tasks(taskSystem), m_heightmapGenerator(heightmapGenerator) {}

bool WorldChunkManager::init()
{
    if (!m_tasks.isInitialised()) {
        MRD_ERROR("WorldChunkManager init failed: TaskSystem is not initialised");
        return false;
    }

    m_initialised = true;
    bootstrapChunkRequests();
    warmHeightmapTiles();
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
    m_heightmapTiles.clear();
    m_residentChunks.clear();
    m_bootstrapRequested = false;
    ++m_renderRevision;
    m_initialised = false;
    MRD_INFO("WorldChunkManager shutting down");
}

void WorldChunkManager::update(float /*deltaTimeSeconds*/)
{
    if (!m_initialised) {
        return;
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

std::vector<WorldChunkRenderData> WorldChunkManager::buildRenderData() const
{
    std::vector<WorldChunkRenderData> renderData;
    renderData.reserve(m_residentChunks.size());

    for (const auto& [key, chunkStorage] : m_residentChunks.chunks()) {
        (void)key;

        WorldChunkRenderData chunkData{
            .coord = chunkStorage.coord(),
            .voxelResolution = chunkStorage.voxelResolution(),
        };
        chunkData.materialIds.reserve(chunkStorage.voxels().size());

        for (const VoxelSample& voxel : chunkStorage.voxels()) {
            chunkData.materialIds.push_back(static_cast<std::uint32_t>(voxel.materialId));
        }

        renderData.push_back(std::move(chunkData));
    }

    std::sort(
        renderData.begin(),
        renderData.end(),
        [](const WorldChunkRenderData& left, const WorldChunkRenderData& right) {
            if (left.coord.y != right.coord.y) {
                return left.coord.y < right.coord.y;
            }
            if (left.coord.z != right.coord.z) {
                return left.coord.z < right.coord.z;
            }
            return left.coord.x < right.coord.x;
        });

    return renderData;
}

void WorldChunkManager::bootstrapChunkRequests()
{
    if (m_bootstrapRequested) {
        return;
    }

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

void WorldChunkManager::warmHeightmapTiles()
{
    if (m_heightmapGenerator == nullptr) {
        return;
    }

    for (const ChunkCoord& coord : m_pendingRequests) {
        const ChunkKey key = heightmapTileKey(coord);
        if (m_heightmapTiles.contains(key)) {
            continue;
        }

        m_heightmapTiles.emplace(key, m_heightmapGenerator->generateTile(coord));
    }
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

void WorldChunkManager::rebuildActiveTerrain()
{
    m_pendingRequests.clear();
    m_inFlightJobs.clear();
    m_chunkRecords.clear();
    m_heightmapTiles.clear();
    m_residentChunks.clear();
    m_bootstrapRequested = false;
    ++m_renderRevision;

    bootstrapChunkRequests();
    warmHeightmapTiles();

    MRD_INFO("World chunk manager rebuilding active terrain");
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

        TerrainHeightmapTile heightmapTile;
        TerrainHeightmapSettings heightmapSettings;
        if (m_heightmapGenerator != nullptr) {
            const ChunkKey tileKey = heightmapTileKey(coord);
            auto tileIt = m_heightmapTiles.find(tileKey);
            if (tileIt == m_heightmapTiles.end()) {
                tileIt = m_heightmapTiles.emplace(
                    tileKey,
                    m_heightmapGenerator->generateTile(coord)).first;
            }

            heightmapTile = tileIt->second;
            heightmapSettings = m_heightmapGenerator->settings();
        }

        m_inFlightJobs.push_back(ChunkJob{
            .coord = coord,
            .key = key,
            .future = m_tasks.async([coord, heightmapTile, heightmapSettings]() {
                return WorldChunkManager::generateChunk(coord, heightmapTile, heightmapSettings);
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
            ++m_renderRevision;

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
    ChunkCoord coord,
    TerrainHeightmapTile heightmapTile,
    TerrainHeightmapSettings heightmapSettings)
{
    const ChunkKey key = makeChunkKey(coord);
    std::vector<VoxelSample> voxels = !heightmapTile.grayscale.empty()
        ? generateHeightmapChunkVoxels(coord, heightmapTile, heightmapSettings)
        : generateDefaultChunkVoxels(coord);
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

std::vector<VoxelSample> WorldChunkManager::generateHeightmapChunkVoxels(
    ChunkCoord coord,
    const TerrainHeightmapTile& tile,
    const TerrainHeightmapSettings& settings)
{
    std::vector<VoxelSample> voxels(
        static_cast<std::size_t>(kWorldChunkResolution) *
        kWorldChunkResolution *
        kWorldChunkResolution);

    const int chunkBaseY = coord.y * kWorldChunkSize;
    for (std::uint32_t localZ = 0; localZ < kWorldChunkResolution; ++localZ) {
        for (std::uint32_t localX = 0; localX < kWorldChunkResolution; ++localX) {
            const std::size_t heightIndex =
                static_cast<std::size_t>(localX) +
                static_cast<std::size_t>(localZ) * tile.resolution;
            const float grayscale = heightIndex < tile.grayscale.size() ? tile.grayscale[heightIndex] : 0.5F;
            const float ridgeMap = heightIndex < tile.ridgeMap.size() ? tile.ridgeMap[heightIndex] : 0.5F;
            const float surfaceHeight =
                settings.minWorldHeight + grayscale * settings.heightRange();

            for (std::uint32_t localY = 0; localY < kWorldChunkResolution; ++localY) {
                const float voxelCenterY = static_cast<float>(chunkBaseY + static_cast<int>(localY)) + 0.5F;
                if (voxelCenterY > surfaceHeight) {
                    continue;
                }

                const std::size_t voxelIndex =
                    static_cast<std::size_t>(localX) +
                    static_cast<std::size_t>(localY) * kWorldChunkResolution +
                    static_cast<std::size_t>(localZ) * kWorldChunkResolution * kWorldChunkResolution;
                VoxelSample& voxel = voxels[voxelIndex];
                voxel.density = 1.0F;
                const float surfaceDepth = surfaceHeight - voxelCenterY;
                const float ridgeSigned = ridgeMap * 2.0F - 1.0F;
                const bool surfaceCrease = ridgeSigned < -0.35F && surfaceDepth <= 2.0F;
                voxel.materialId = static_cast<std::uint8_t>(
                    surfaceDepth <= 1.5F && !surfaceCrease
                        ? VoxelMaterialId::Grass
                        : VoxelMaterialId::Stone);
            }
        }
    }

    return voxels;
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