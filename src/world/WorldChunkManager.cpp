#include "world/WorldChunkManager.hpp"

#include "core/Logger.hpp"
#include "world/TerrainHeightmapGenerator.hpp"
#include "world/SparseVoxelOctree.hpp"
#include "world/WorldChunkStorage.hpp"

#include "tasks/TaskSystem.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

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
    case VoxelMaterialId::Dirt:
        return "dirt";
    case VoxelMaterialId::Sand:
        return "sand";
    case VoxelMaterialId::Snow:
        return "snow";
    case VoxelMaterialId::Forest:
        return "forest";
    case VoxelMaterialId::Air:
        return "air";
    default:
        return "unknown";
    }
}

struct TerrainMaterialSample {
    float heightNormalized{0.0F};
    float ridgeSigned{0.0F};
    float erosionSigned{0.0F};
    float treeAmount{0.0F};
    bool isBeach{false};
    bool isSnow{false};
    bool isCliff{false};
    bool isForest{false};
};

[[nodiscard]] float saturate(float value) noexcept
{
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] TerrainMaterialSample sampleTerrainMaterial(
    float surfaceHeight,
    float ridgeMap,
    float erosion,
    float treeCoverage,
    const TerrainHeightmapSettings& settings) noexcept
{
    const float heightNormalized =
        saturate((surfaceHeight - settings.minWorldHeight) / std::max(settings.heightRange(), 1.0F));
    const float ridgeSigned = ridgeMap * 2.0F - 1.0F;
    const float erosionSigned = erosion * 2.0F - 1.0F;
    const float treeAmount = saturate((treeCoverage - 0.45F) * 1.8F);
    const float beachHeight = settings.minWorldHeight + settings.heightRange() * 0.395F;

    TerrainMaterialSample sample{};
    sample.heightNormalized = heightNormalized;
    sample.ridgeSigned = ridgeSigned;
    sample.erosionSigned = erosionSigned;
    sample.treeAmount = treeAmount;
    sample.isBeach = surfaceHeight <= beachHeight;
    sample.isSnow = heightNormalized >= 0.84F && ridgeSigned >= 0.18F;
    sample.isCliff = ridgeSigned <= -0.60F ||
                     erosionSigned >= 0.70F ||
                     (ridgeSigned <= -0.40F && erosionSigned >= 0.35F);
    sample.isForest =
        treeAmount >= 0.50F &&
        ridgeSigned >= -0.20F &&
        erosionSigned <= 0.35F &&
        !sample.isBeach &&
        !sample.isSnow;
    return sample;
}

[[nodiscard]] VoxelMaterialId chooseSurfaceMaterial(const TerrainMaterialSample& sample) noexcept
{
    if (sample.isSnow) {
        return VoxelMaterialId::Snow;
    }

    if (sample.isBeach) {
        return VoxelMaterialId::Sand;
    }

    if (sample.isForest) {
        return VoxelMaterialId::Forest;
    }

    if (sample.isCliff) {
        return VoxelMaterialId::Stone;
    }

    if (sample.erosionSigned >= 0.12F || sample.treeAmount >= 0.20F) {
        return VoxelMaterialId::Dirt;
    }

    return VoxelMaterialId::Grass;
}

[[nodiscard]] VoxelMaterialId chooseSubsurfaceMaterial(
    const TerrainMaterialSample& sample,
    VoxelMaterialId surfaceMaterial,
    float surfaceDepth) noexcept
{
    if (surfaceDepth <= 1.25F) {
        return surfaceMaterial;
    }

    if (surfaceMaterial == VoxelMaterialId::Sand && surfaceDepth <= 3.0F) {
        return VoxelMaterialId::Sand;
    }

    if (surfaceDepth <= 4.0F) {
        if (surfaceMaterial == VoxelMaterialId::Snow || sample.isCliff) {
            return VoxelMaterialId::Stone;
        }

        return VoxelMaterialId::Dirt;
    }

    return VoxelMaterialId::Stone;
}

[[nodiscard]] ChunkKey heightmapTileKey(ChunkCoord coord) noexcept
{
    return makeChunkKey(ChunkCoord{.x = coord.x, .y = 0, .z = coord.z});
}

[[nodiscard]] int worldToChunkCoord(float worldPosition) noexcept
{
    return static_cast<int>(std::floor(worldPosition / static_cast<float>(kWorldChunkSize)));
}

[[nodiscard]] std::array<float, 3> normalizedLookAhead(const CameraRenderState& cameraState) noexcept
{
    const float lookX = cameraState.forward[0];
    const float lookZ = cameraState.forward[2];
    const float lengthSquared = lookX * lookX + lookZ * lookZ;
    if (lengthSquared <= 1e-6F) {
        return {0.0F, 0.0F, 0.0F};
    }

    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    return {lookX * inverseLength, 0.0F, lookZ * inverseLength};
}

[[nodiscard]] VoxelMaterialId representativeSurfaceMaterial(const WorldChunkStorage& chunk) noexcept
{
    std::array<std::size_t, 7> counts{};
    const std::uint32_t resolution = chunk.voxelResolution();
    const auto& voxels = chunk.voxels();

    for (std::uint32_t localZ = 0; localZ < resolution; ++localZ) {
        for (std::uint32_t localX = 0; localX < resolution; ++localX) {
            for (std::int32_t localY = static_cast<std::int32_t>(resolution) - 1; localY >= 0; --localY) {
                const std::size_t voxelIndex =
                    static_cast<std::size_t>(localX) +
                    static_cast<std::size_t>(localY) * resolution +
                    static_cast<std::size_t>(localZ) * resolution * resolution;
                const VoxelSample& voxel = voxels[voxelIndex];
                if (voxel.density <= 0.0F || voxel.materialId == 0U) {
                    continue;
                }

                if (voxel.materialId < counts.size()) {
                    ++counts[voxel.materialId];
                }
                break;
            }
        }
    }

    const auto dominant = std::max_element(counts.begin() + 1, counts.end());
    if (dominant == counts.end() || *dominant == 0U) {
        return VoxelMaterialId::Air;
    }

    return static_cast<VoxelMaterialId>(std::distance(counts.begin(), dominant));
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
    ++m_renderRevision;
    m_initialised = false;
    MRD_INFO("WorldChunkManager shutting down");
}

void WorldChunkManager::update(float /*deltaTimeSeconds*/)
{
    if (!m_initialised) {
        return;
    }

    if (m_hasStreamingCamera) {
        pruneChunksOutsideRetention();
        queueChunksAroundFocus();
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

void WorldChunkManager::setStreamingCamera(const CameraRenderState& cameraState) noexcept
{
    const std::array<float, 3> lookAhead = normalizedLookAhead(cameraState);
    const float focusX = cameraState.position[0] + lookAhead[0] * kStreamingLookAheadDistance;
    const float focusZ = cameraState.position[2] + lookAhead[2] * kStreamingLookAheadDistance;

    m_streamingFocusChunk = ChunkCoord{
        .x = worldToChunkCoord(focusX),
        .y = worldToChunkCoord(cameraState.position[1]),
        .z = worldToChunkCoord(focusZ),
    };
    m_hasStreamingCamera = true;
}

void WorldChunkManager::queueChunksAroundFocus()
{
    if (!m_hasStreamingCamera) {
        return;
    }

    for (int y = m_streamingFocusChunk.y - kStreamChunksBelowFocus;
         y <= m_streamingFocusChunk.y + kStreamChunksAboveFocus;
         ++y) {
        for (int z = m_streamingFocusChunk.z - kStreamRadiusXZ;
             z <= m_streamingFocusChunk.z + kStreamRadiusXZ;
             ++z) {
            for (int x = m_streamingFocusChunk.x - kStreamRadiusXZ;
                 x <= m_streamingFocusChunk.x + kStreamRadiusXZ;
                 ++x) {
                requestChunk(ChunkCoord{.x = x, .y = y, .z = z});
            }
        }
    }
}

bool WorldChunkManager::shouldKeepChunk(ChunkCoord coord) const noexcept
{
    if (!m_hasStreamingCamera) {
        return true;
    }

    const bool withinXZ =
        std::abs(coord.x - m_streamingFocusChunk.x) <= kRetentionRadiusXZ &&
        std::abs(coord.z - m_streamingFocusChunk.z) <= kRetentionRadiusXZ;
    const bool withinY =
        coord.y >= m_streamingFocusChunk.y - kRetentionChunksBelowFocus &&
        coord.y <= m_streamingFocusChunk.y + kRetentionChunksAboveFocus;
    return withinXZ && withinY;
}

bool WorldChunkManager::shouldRequestChunk(ChunkCoord coord) const noexcept
{
    if (!m_hasStreamingCamera) {
        return true;
    }

    const bool withinXZ =
        std::abs(coord.x - m_streamingFocusChunk.x) <= kStreamRadiusXZ &&
        std::abs(coord.z - m_streamingFocusChunk.z) <= kStreamRadiusXZ;
    const bool withinY =
        coord.y >= m_streamingFocusChunk.y - kStreamChunksBelowFocus &&
        coord.y <= m_streamingFocusChunk.y + kStreamChunksAboveFocus;
    return withinXZ && withinY;
}

void WorldChunkManager::pruneChunksOutsideRetention()
{
    bool removedResidentChunk = false;

    for (auto it = m_residentChunks.chunks().begin(); it != m_residentChunks.chunks().end();) {
        const ChunkCoord coord = it->second.coord();
        const ChunkKey key = it->first;
        ++it;
        if (shouldKeepChunk(coord)) {
            continue;
        }

        m_residentChunks.erase(key);
        m_chunkRecords.erase(key);
        removedResidentChunk = true;
    }

    if (removedResidentChunk) {
        ++m_renderRevision;
    }

    for (auto recordIt = m_chunkRecords.begin(); recordIt != m_chunkRecords.end();) {
        if (recordIt->second.status == ChunkStatus::Generating || shouldKeepChunk(recordIt->second.coord)) {
            ++recordIt;
            continue;
        }

        recordIt = m_chunkRecords.erase(recordIt);
    }

    m_pendingRequests.erase(
        std::remove_if(
            m_pendingRequests.begin(),
            m_pendingRequests.end(),
            [this](ChunkCoord coord) {
                return !shouldRequestChunk(coord);
            }),
        m_pendingRequests.end());

    for (auto tileIt = m_heightmapTiles.begin(); tileIt != m_heightmapTiles.end();) {
        const ChunkCoord coord = tileIt->second.coord;
        const bool withinRetention =
            std::abs(coord.x - m_streamingFocusChunk.x) <= kRetentionRadiusXZ &&
            std::abs(coord.z - m_streamingFocusChunk.z) <= kRetentionRadiusXZ;
        if (withinRetention) {
            ++tileIt;
            continue;
        }

        tileIt = m_heightmapTiles.erase(tileIt);
    }
}

void WorldChunkManager::requestChunk(ChunkCoord coord)
{
    if (!shouldRequestChunk(coord)) {
        return;
    }

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
    ++m_renderRevision;

    if (m_hasStreamingCamera) {
        queueChunksAroundFocus();
    }

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

        if (!shouldRequestChunk(coord)) {
            m_chunkRecords.erase(key);
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

            if (!shouldKeepChunk(job.coord)) {
                m_chunkRecords.erase(chunkIt);
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
                voxelMaterialName(representativeSurfaceMaterial(*residentChunk)),
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
            const float erosion = heightIndex < tile.erosion.size() ? tile.erosion[heightIndex] : 0.5F;
            const float treeCoverage =
                heightIndex < tile.treeCoverage.size() ? tile.treeCoverage[heightIndex] : 0.0F;
            const float surfaceHeight =
                settings.minWorldHeight + grayscale * settings.heightRange();
            const TerrainMaterialSample materialSample = sampleTerrainMaterial(
                surfaceHeight,
                ridgeMap,
                erosion,
                treeCoverage,
                settings);
            const VoxelMaterialId surfaceMaterial = chooseSurfaceMaterial(materialSample);

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
                const VoxelMaterialId material = chooseSubsurfaceMaterial(
                    materialSample,
                    surfaceMaterial,
                    surfaceDepth);
                voxel.materialId = static_cast<std::uint8_t>(material);
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