#include "world/WorldChunkManager.hpp"

#include "core/Logger.hpp"
#include "world/WorldChunkDatabase.hpp"
#include "world/TerrainHeightmapGenerator.hpp"
#include "world/SparseVoxelOctree.hpp"
#include "world/WorldChunkStorage.hpp"

#include "tasks/TaskSystem.hpp"

#include <bit>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <system_error>

#include <tracy/Tracy.hpp>

namespace Meridian {

namespace {

[[nodiscard]] bool isChunkFullySolid(const WorldChunkStorage& chunkStorage) noexcept
{
    const std::uint64_t resolution = chunkStorage.voxelResolution();
    const std::uint64_t voxelCount = resolution * resolution * resolution;
    return chunkStorage.solidVoxelCount() == voxelCount;
}

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

[[nodiscard]] float sampleHeightmapChannel(
    const std::vector<float>& channel,
    std::uint32_t resolution,
    float normalizedX,
    float normalizedZ,
    float fallback) noexcept
{
    if (resolution == 0 ||
        channel.size() < static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution)) {
        return fallback;
    }

    const float sampleX = saturate(normalizedX) * static_cast<float>(resolution) - 0.5F;
    const float sampleZ = saturate(normalizedZ) * static_cast<float>(resolution) - 0.5F;
    const int x0 = std::clamp(static_cast<int>(std::floor(sampleX)), 0, static_cast<int>(resolution) - 1);
    const int z0 = std::clamp(static_cast<int>(std::floor(sampleZ)), 0, static_cast<int>(resolution) - 1);
    const int x1 = std::min(x0 + 1, static_cast<int>(resolution) - 1);
    const int z1 = std::min(z0 + 1, static_cast<int>(resolution) - 1);
    const float tx = sampleX - static_cast<float>(x0);
    const float tz = sampleZ - static_cast<float>(z0);

    const auto sampleAt = [&](int sampleXIndex, int sampleZIndex) noexcept {
        const std::size_t sampleIndex =
            static_cast<std::size_t>(sampleXIndex) +
            static_cast<std::size_t>(sampleZIndex) * static_cast<std::size_t>(resolution);
        return channel[sampleIndex];
    };

    const float value00 = sampleAt(x0, z0);
    const float value10 = sampleAt(x1, z0);
    const float value01 = sampleAt(x0, z1);
    const float value11 = sampleAt(x1, z1);
    const float value0 = std::lerp(value00, value10, tx);
    const float value1 = std::lerp(value01, value11, tx);
    return std::lerp(value0, value1, tz);
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

void logResidentChunk(const WorldChunkStorage& chunk, std::size_t residentCount, bool loadedFromDatabase)
{
    MRD_TRACE(
        "World chunk [{}, {}, {}] {} (material {}, {}^3 voxels, solid {}, svo nodes {}, resident: {})",
        chunk.coord().x,
        chunk.coord().y,
        chunk.coord().z,
        loadedFromDatabase ? "loaded from database" : "ready",
        voxelMaterialName(representativeSurfaceMaterial(chunk)),
        chunk.voxelResolution(),
        chunk.solidVoxelCount(),
        chunk.octree().stats().nodeCount,
        residentCount);
}

[[nodiscard]] std::uint64_t hashCombine(std::uint64_t seed, std::uint64_t value) noexcept
{
    return seed ^ (value + 0x9E3779B97F4A7C15ULL + (seed << 6U) + (seed >> 2U));
}

[[nodiscard]] std::uint64_t terrainSettingsSignature(const TerrainHeightmapSettings& settings) noexcept
{
    std::uint64_t signature = 0xCBF29CE484222325ULL;

    const auto addInteger = [&signature](std::uint64_t value) {
        signature = hashCombine(signature, value);
    };
    const auto addFloat = [&signature](float value) {
        signature = hashCombine(signature, static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value)));
    };

    addInteger(settings.tileResolution);
    addInteger(settings.octaveCount);
    addInteger(settings.heightOctaveCount);
    addInteger(settings.worldSeed);
    addFloat(settings.minWorldHeight);
    addFloat(settings.maxWorldHeight);
    addFloat(settings.baseFrequency);
    addFloat(settings.heightFrequency);
    addFloat(settings.heightAmplitude);
    addFloat(settings.heightLacunarity);
    addFloat(settings.heightGain);
    addFloat(settings.erosionFrequency);
    addFloat(settings.erosionStrength);
    addFloat(settings.octaveGain);
    addFloat(settings.lacunarity);
    addFloat(settings.cellSizeMultiplier);
    addFloat(settings.slopeScale);
    addFloat(settings.stackedDetail);
    addFloat(settings.normalizationFactor);
    addFloat(settings.straightSteeringStrength);
    addFloat(settings.gullyWeight);
    addFloat(settings.ridgeRounding);
    addFloat(settings.creaseRounding);
    addFloat(settings.inputRoundingMultiplier);
    addFloat(settings.octaveRoundingMultiplier);
    addFloat(settings.onsetInitial);
    addFloat(settings.onsetOctave);
    addFloat(settings.ridgeMapOnsetInitial);
    addFloat(settings.ridgeMapOnsetOctave);
    addFloat(settings.heightOffsetBase);
    addFloat(settings.heightOffsetFadeInfluence);
    return signature;
}

[[nodiscard]] std::filesystem::path worldChunkDatabasePath()
{
    return std::filesystem::current_path() / "build" / "cache" / "world-chunks";
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

    std::promise<bool> initPromise;
    std::future<bool> initFuture = initPromise.get_future();

    {
        std::scoped_lock lock(m_commandMutex);
        m_stopWorker = false;
        m_updateRequested = false;
        m_rebuildRequested = false;
        m_hasPendingStreamingCamera = false;
        m_hasPendingGenerationDistance = false;
    }

    try {
        m_workerThread = std::thread(&WorldChunkManager::workerMain, this, std::move(initPromise));
    } catch (const std::system_error& error) {
        MRD_ERROR("WorldChunkManager init failed: could not start worker thread: {}", error.what());
        return false;
    }

    m_initialised = initFuture.get();
    if (!m_initialised) {
        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }
        return false;
    }

    MRD_INFO("WorldChunkManager initialized (chunk resolution: {}^3 voxels)",
        kWorldChunkResolution);
    return true;
}

void WorldChunkManager::shutdown()
{
    if (!m_initialised) {
        return;
    }

    m_initialised = false;
    {
        std::scoped_lock lock(m_commandMutex);
        m_stopWorker = true;
        m_updateRequested = true;
    }
    m_commandCondition.notify_one();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    MRD_INFO("WorldChunkManager shutting down");
}

void WorldChunkManager::update(float /*deltaTimeSeconds*/)
{
    if (!m_initialised) {
        return;
    }

    {
        std::scoped_lock lock(m_commandMutex);
        m_updateRequested = true;
    }
    m_commandCondition.notify_one();
}

std::size_t WorldChunkManager::getResidentChunkCount() const noexcept
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot.residentChunkCount;
}

std::size_t WorldChunkManager::getInFlightChunkCount() const noexcept
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot.inFlightChunkCount;
}

std::size_t WorldChunkManager::getPendingChunkCount() const noexcept
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot.pendingChunkCount;
}

std::uint64_t WorldChunkManager::renderRevision() const noexcept
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot.renderRevision;
}

std::vector<WorldChunkRenderData> WorldChunkManager::buildRenderData() const
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot.renderData;
}

void WorldChunkManager::cacheResidentChunkRenderData(const WorldChunkStorage& chunkStorage)
{
    m_renderChunkData.insert_or_assign(chunkStorage.key(), createRenderData(chunkStorage));
    if (isChunkFullySolid(chunkStorage)) {
        m_solidOccluderKeys.insert(chunkStorage.key());
    } else {
        m_solidOccluderKeys.erase(chunkStorage.key());
    }
}

WorldChunkRenderData WorldChunkManager::createRenderData(const WorldChunkStorage& chunkStorage)
{
    auto packedOctreeNodes = std::make_shared<std::vector<std::uint32_t>>();
    const auto& nodes = chunkStorage.octree().nodes();
    packedOctreeNodes->reserve(nodes.size() * 9U);

    for (const SparseVoxelOctree::Node& node : nodes) {
        for (std::uint32_t childIndex : node.children) {
            packedOctreeNodes->push_back(childIndex);
        }

        packedOctreeNodes->push_back(
            static_cast<std::uint32_t>(node.childMask) |
            (static_cast<std::uint32_t>(node.leaf ? 1U : 0U) << 8U) |
            (static_cast<std::uint32_t>(node.materialId) << 16U));
    }

    return WorldChunkRenderData{
        .coord = chunkStorage.coord(),
        .voxelResolution = chunkStorage.voxelResolution(),
        .packedOctreeNodes = std::move(packedOctreeNodes),
    };
}

void WorldChunkManager::setStreamingCamera(const CameraRenderState& cameraState) noexcept
{
    if (!m_initialised) {
        return;
    }

    {
        std::scoped_lock lock(m_commandMutex);
        m_pendingStreamingCameraState = cameraState;
        m_hasPendingStreamingCamera = true;
    }
    m_commandCondition.notify_one();
}

void WorldChunkManager::setStreamingCameraWorker(const CameraRenderState& cameraState) noexcept
{
    m_streamingCameraState = cameraState;
    m_streamingFocusChunk = ChunkCoord{
        .x = worldToChunkCoord(cameraState.position[0]),
        .y = worldToChunkCoord(cameraState.position[1]),
        .z = worldToChunkCoord(cameraState.position[2]),
    };
    m_hasStreamingCamera = true;
}

void WorldChunkManager::setChunkGenerationDistanceChunks(float generationDistanceChunks) noexcept
{
    if (!m_initialised) {
        return;
    }

    {
        std::scoped_lock lock(m_commandMutex);
        m_pendingGenerationDistanceChunks = generationDistanceChunks;
        m_hasPendingGenerationDistance = true;
    }
    m_commandCondition.notify_one();
}

void WorldChunkManager::setChunkGenerationDistanceChunksWorker(float generationDistanceChunks) noexcept
{
    m_generationDistanceChunks = std::clamp(generationDistanceChunks, 1.0F, 32.0F);
}

void WorldChunkManager::workerMain(std::promise<bool> initResult)
{
    tracy::SetThreadName("Chunk Manager Worker");

    std::shared_ptr<WorldChunkDatabase> chunkDatabase = std::make_shared<WorldChunkDatabase>();
    if (chunkDatabase->init(worldChunkDatabasePath())) {
        m_chunkDatabase = std::move(chunkDatabase);
    } else {
        MRD_WARN("World chunk database unavailable; falling back to procedural generation only");
    }

    publishSnapshot();
    initResult.set_value(true);

    for (;;) {
        bool stopRequested = false;
        bool updateRequested = false;
        bool rebuildRequested = false;
        bool hasPendingStreamingCamera = false;
        bool hasPendingGenerationDistance = false;
        CameraRenderState pendingStreamingCameraState{};
        float pendingGenerationDistanceChunks = kDefaultGenerationDistanceChunks;

        {
            std::unique_lock lock(m_commandMutex);
            const auto waitDuration =
                m_inFlightJobs.empty() ? std::chrono::milliseconds(16) : std::chrono::milliseconds(1);
            m_commandCondition.wait_for(lock, waitDuration, [this]() {
                return m_stopWorker ||
                    m_updateRequested ||
                    m_rebuildRequested ||
                    m_hasPendingStreamingCamera ||
                    m_hasPendingGenerationDistance;
            });

            stopRequested = m_stopWorker;
            updateRequested = m_updateRequested;
            m_updateRequested = false;
            rebuildRequested = m_rebuildRequested;
            m_rebuildRequested = false;
            hasPendingStreamingCamera = m_hasPendingStreamingCamera;
            if (hasPendingStreamingCamera) {
                pendingStreamingCameraState = m_pendingStreamingCameraState;
                m_hasPendingStreamingCamera = false;
            }
            hasPendingGenerationDistance = m_hasPendingGenerationDistance;
            if (hasPendingGenerationDistance) {
                pendingGenerationDistanceChunks = m_pendingGenerationDistanceChunks;
                m_hasPendingGenerationDistance = false;
            }
        }

        if (hasPendingGenerationDistance) {
            setChunkGenerationDistanceChunksWorker(pendingGenerationDistanceChunks);
        }
        if (hasPendingStreamingCamera) {
            setStreamingCameraWorker(pendingStreamingCameraState);
        }
        if (rebuildRequested) {
            rebuildActiveTerrainWorker();
        }
        if (updateRequested || !m_inFlightJobs.empty()) {
            updateWorker();
        }
        publishSnapshot();

        if (stopRequested) {
            break;
        }
    }

    persistResidentChunks();
    drainInFlightJobs();
    m_pendingRequests.clear();
    m_inFlightJobs.clear();
    m_chunkRecords.clear();
    m_renderChunkData.clear();
    m_solidOccluderKeys.clear();
    m_residentChunks.clear();
    m_chunkDatabase.reset();
    ++m_renderRevision;
    publishSnapshot();
}

void WorldChunkManager::updateWorker()
{
    ZoneScopedN("WorldChunkManager::updateWorker");
    if (m_hasStreamingCamera) {
        pruneChunksOutsideRetention();
        queueChunksAroundFocus();
    }

    dispatchChunkJobs();
    collectCompletedJobs();
}

void WorldChunkManager::publishSnapshot()
{
    Snapshot snapshot{
        .residentChunkCount = m_residentChunks.size(),
        .inFlightChunkCount = m_inFlightJobs.size(),
        .pendingChunkCount = m_pendingRequests.size(),
        .renderRevision = m_renderRevision,
    };

    bool rebuildRenderData = true;
    {
        std::scoped_lock lock(m_snapshotMutex);
        if (m_snapshot.renderRevision == m_renderRevision) {
            snapshot.renderData = m_snapshot.renderData;
            rebuildRenderData = false;
        }
    }

    if (rebuildRenderData) {
        snapshot.renderData.reserve(m_renderChunkData.size());
        for (const auto& [key, chunkData] : m_renderChunkData) {
            (void)key;
            snapshot.renderData.push_back(chunkData);
        }

        std::sort(
            snapshot.renderData.begin(),
            snapshot.renderData.end(),
            [](const WorldChunkRenderData& left, const WorldChunkRenderData& right) {
                if (left.coord.y != right.coord.y) {
                    return left.coord.y < right.coord.y;
                }
                if (left.coord.z != right.coord.z) {
                    return left.coord.z < right.coord.z;
                }
                return left.coord.x < right.coord.x;
            });
    }

    std::scoped_lock lock(m_snapshotMutex);
    m_snapshot = std::move(snapshot);
}

int WorldChunkManager::generationRadiusXZ() const noexcept
{
    return std::max(1, static_cast<int>(std::ceil(m_generationDistanceChunks)));
}

int WorldChunkManager::retentionRadiusXZ() const noexcept
{
    return std::max(1, static_cast<int>(std::ceil(m_generationDistanceChunks + kRetentionPaddingChunks)));
}

void WorldChunkManager::queueChunksAroundFocus()
{
    if (!m_hasStreamingCamera) {
        return;
    }

    ZoneScopedN("WorldChunkManager::queueChunksAroundFocus");
    const int radiusXZ = generationRadiusXZ();

    for (int y = m_streamingFocusChunk.y - kStreamChunksBelowFocus;
         y <= m_streamingFocusChunk.y + kStreamChunksAboveFocus;
         ++y) {
        for (int z = m_streamingFocusChunk.z - radiusXZ;
             z <= m_streamingFocusChunk.z + radiusXZ;
             ++z) {
            for (int x = m_streamingFocusChunk.x - radiusXZ;
                 x <= m_streamingFocusChunk.x + radiusXZ;
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

    const int radiusXZ = retentionRadiusXZ();
    const bool withinXZ =
        std::abs(coord.x - m_streamingFocusChunk.x) <= radiusXZ &&
        std::abs(coord.z - m_streamingFocusChunk.z) <= radiusXZ;
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

    const int radiusXZ = generationRadiusXZ();
    const bool withinXZ =
        std::abs(coord.x - m_streamingFocusChunk.x) <= radiusXZ &&
        std::abs(coord.z - m_streamingFocusChunk.z) <= radiusXZ;
    const bool withinY =
        coord.y >= m_streamingFocusChunk.y - kStreamChunksBelowFocus &&
        coord.y <= m_streamingFocusChunk.y + kStreamChunksAboveFocus;
    return withinXZ && withinY;
}

bool WorldChunkManager::shouldGenerateChunk(ChunkCoord coord) const noexcept
{
    if (!shouldRequestChunk(coord)) {
        return false;
    }

    if (!m_hasStreamingCamera) {
        return true;
    }

    return m_chunkGenerationVisibility.canGenerateChunk(
        coord,
        m_streamingCameraState,
        m_solidOccluderKeys);
}

void WorldChunkManager::persistResidentChunks()
{
    if (m_chunkDatabase == nullptr) {
        return;
    }

    for (const auto& [key, chunkStorage] : m_residentChunks.chunks()) {
        (void)key;
        persistResidentChunk(chunkStorage);
    }
}

void WorldChunkManager::persistResidentChunk(const WorldChunkStorage& chunkStorage)
{
    if (m_chunkDatabase == nullptr) {
        return;
    }

    const TerrainHeightmapSettings heightmapSettings =
        m_heightmapGenerator != nullptr ? m_heightmapGenerator->settings() : TerrainHeightmapSettings{};
    const std::uint64_t settingsSignature = terrainSettingsSignature(heightmapSettings);
    (void)m_chunkDatabase->storeChunk(chunkStorage, settingsSignature);
}

void WorldChunkManager::pruneChunksOutsideRetention()
{
    ZoneScopedN("WorldChunkManager::pruneChunksOutsideRetention");
    bool removedResidentChunk = false;

    for (auto it = m_residentChunks.chunks().begin(); it != m_residentChunks.chunks().end();) {
        const WorldChunkStorage& chunkStorage = it->second;
        const ChunkCoord coord = chunkStorage.coord();
        const ChunkKey key = it->first;
        ++it;
        if (shouldKeepChunk(coord)) {
            continue;
        }

        persistResidentChunk(chunkStorage);
        m_solidOccluderKeys.erase(key);
        m_residentChunks.erase(key);
        m_renderChunkData.erase(key);
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

}

void WorldChunkManager::requestChunk(ChunkCoord coord)
{
    ZoneScopedN("WorldChunkManager::requestChunk");
    if (!shouldRequestChunk(coord)) {
        return;
    }

    const ChunkKey key = makeChunkKey(coord);
    auto existingRecord = m_chunkRecords.find(key);
    if (existingRecord != m_chunkRecords.end()) {
        if (existingRecord->second.status == ChunkStatus::DeferredGeneration && shouldGenerateChunk(coord)) {
            existingRecord->second.status = ChunkStatus::Requested;
            m_pendingRequests.push_back(coord);
        }
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
    if (!m_initialised) {
        return;
    }

    {
        std::scoped_lock lock(m_commandMutex);
        m_rebuildRequested = true;
        m_updateRequested = true;
    }
    m_commandCondition.notify_one();
}

void WorldChunkManager::rebuildActiveTerrainWorker()
{
    ZoneScopedN("WorldChunkManager::rebuildActiveTerrainWorker");
    drainInFlightJobs();
    m_pendingRequests.clear();
    m_inFlightJobs.clear();
    m_chunkRecords.clear();
    m_renderChunkData.clear();
    m_solidOccluderKeys.clear();
    m_residentChunks.clear();
    ++m_renderRevision;

    if (m_hasStreamingCamera) {
        queueChunksAroundFocus();
    }

    MRD_INFO("World chunk manager rebuilding active terrain");
}

void WorldChunkManager::dispatchChunkJobs()
{
    ZoneScopedN("WorldChunkManager::dispatchChunkJobs");
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

        const TerrainHeightmapSettings heightmapSettings =
            m_heightmapGenerator != nullptr ? m_heightmapGenerator->settings() : TerrainHeightmapSettings{};
        const std::uint64_t settingsSignature = terrainSettingsSignature(heightmapSettings);
        chunkIt->second.status = ChunkStatus::Generating;
        const bool allowGeneration = shouldGenerateChunk(coord);

        m_inFlightJobs.push_back(ChunkJob{
            .coord = coord,
            .key = key,
            .future = m_tasks.async([
                coord,
                key,
                allowGeneration,
                heightmapGenerator = m_heightmapGenerator,
                heightmapSettings,
                settingsSignature,
                chunkDatabase = m_chunkDatabase]() {
                ZoneScopedN("WorldChunkManager Chunk Job");

                if (chunkDatabase != nullptr) {
                    ZoneScopedN("WorldChunkManager::tryLoadCachedChunkAsync");
                    if (std::optional<WorldChunkStorage> cachedChunk =
                            chunkDatabase->loadChunk(coord, key, settingsSignature);
                        cachedChunk.has_value()) {
                        if (heightmapGenerator != nullptr) {
                            ZoneScopedN("WorldChunkManager::populateCachedHeightmapTile");
                            (void)heightmapGenerator->generateTile(coord);
                        }
                        return std::optional<ChunkJobResult>{ChunkJobResult{
                            .chunkStorage = std::move(*cachedChunk),
                            .loadedFromDatabase = true,
                        }};
                    }
                }

                if (!allowGeneration) {
                    return std::optional<ChunkJobResult>{};
                }

                std::shared_ptr<const TerrainHeightmapTile> heightmapTile;
                if (heightmapGenerator != nullptr) {
                    ZoneScopedN("WorldChunkManager::generateHeightmapTile");
                    heightmapTile = heightmapGenerator->generateTile(coord);
                }

                ChunkJobResult generatedChunk =
                    WorldChunkManager::generateChunk(coord, heightmapTile, heightmapSettings);
                if (chunkDatabase != nullptr) {
                    ZoneScopedN("WorldChunkManager::storeGeneratedChunk");
                    (void)chunkDatabase->storeChunk(generatedChunk.chunkStorage, settingsSignature);
                }
                return std::optional<ChunkJobResult>{std::move(generatedChunk)};
            }),
        });
    }
}

void WorldChunkManager::drainInFlightJobs()
{
    ZoneScopedN("WorldChunkManager::drainInFlightJobs");

    for (ChunkJob& job : m_inFlightJobs) {
        if (!job.future.valid()) {
            continue;
        }

        job.future.wait();
        job.future.get();
    }
}

void WorldChunkManager::collectCompletedJobs()
{
    using namespace std::chrono_literals;

    ZoneScopedN("WorldChunkManager::collectCompletedJobs");
    auto nextJob = std::remove_if(
        m_inFlightJobs.begin(),
        m_inFlightJobs.end(),
        [this](ChunkJob& job) {
            ZoneScopedN("WorldChunkManager::collectCompletedJob");
            if (job.future.wait_for(0ms) != std::future_status::ready) {
                return false;
            }

            std::optional<ChunkJobResult> jobResult = job.future.get();
            auto chunkIt = m_chunkRecords.find(job.key);
            if (chunkIt == m_chunkRecords.end()) {
                return true;
            }

            if (!shouldKeepChunk(job.coord)) {
                m_chunkRecords.erase(chunkIt);
                return true;
            }

            if (!jobResult.has_value()) {
                chunkIt->second.status = ChunkStatus::DeferredGeneration;
                return true;
            }

            ChunkRecord& record = chunkIt->second;
            record.status = ChunkStatus::Resident;
            m_residentChunks.upsert(std::move(jobResult->chunkStorage));
            if (const WorldChunkStorage* residentChunk = m_residentChunks.find(job.key);
                residentChunk != nullptr) {
                cacheResidentChunkRenderData(*residentChunk);
            }
            ++m_renderRevision;

            const WorldChunkStorage* residentChunk = m_residentChunks.find(job.key);
            if (residentChunk == nullptr) {
                return true;
            }

            logResidentChunk(*residentChunk, m_residentChunks.size(), jobResult->loadedFromDatabase);
            return true;
        });

    m_inFlightJobs.erase(nextJob, m_inFlightJobs.end());
}

WorldChunkManager::ChunkJobResult WorldChunkManager::generateChunk(
    ChunkCoord coord,
    std::shared_ptr<const TerrainHeightmapTile> heightmapTile,
    TerrainHeightmapSettings heightmapSettings)
{
    ZoneScopedN("WorldChunkManager::generateChunk");
    const ChunkKey key = makeChunkKey(coord);
    std::vector<VoxelSample> voxels = (heightmapTile && !heightmapTile->grayscale.empty())
        ? generateHeightmapChunkVoxels(coord, *heightmapTile, heightmapSettings)
        : generateDefaultChunkVoxels(coord);
    SparseVoxelOctree octree;
    {
        ZoneScopedN("SparseVoxelOctree::build");
        octree = SparseVoxelOctree::build(voxels, kWorldChunkResolution);
    }

    return ChunkJobResult{
        .chunkStorage = WorldChunkStorage(
            coord,
            key,
            kWorldChunkResolution,
            std::move(voxels),
            std::move(octree)),
        .loadedFromDatabase = false,
    };
}

std::vector<VoxelSample> WorldChunkManager::generateHeightmapChunkVoxels(
    ChunkCoord coord,
    const TerrainHeightmapTile& tile,
    const TerrainHeightmapSettings& settings)
{
    ZoneScopedN("WorldChunkManager::generateHeightmapChunkVoxels");
    std::vector<VoxelSample> voxels(
        static_cast<std::size_t>(kWorldChunkResolution) *
        kWorldChunkResolution *
        kWorldChunkResolution);

    const int chunkBaseY = coord.y * kWorldChunkSize;
    for (std::uint32_t localZ = 0; localZ < kWorldChunkResolution; ++localZ) {
        for (std::uint32_t localX = 0; localX < kWorldChunkResolution; ++localX) {
            const float normalizedX =
                (static_cast<float>(localX) + 0.5F) / static_cast<float>(kWorldChunkResolution);
            const float normalizedZ =
                (static_cast<float>(localZ) + 0.5F) / static_cast<float>(kWorldChunkResolution);
            const float grayscale =
                sampleHeightmapChannel(tile.grayscale, tile.resolution, normalizedX, normalizedZ, 0.5F);
            const float ridgeMap =
                sampleHeightmapChannel(tile.ridgeMap, tile.resolution, normalizedX, normalizedZ, 0.5F);
            const float erosion =
                sampleHeightmapChannel(tile.erosion, tile.resolution, normalizedX, normalizedZ, 0.5F);
            const float treeCoverage = sampleHeightmapChannel(
                tile.treeCoverage,
                tile.resolution,
                normalizedX,
                normalizedZ,
                0.0F);
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
    ZoneScopedN("WorldChunkManager::generateDefaultChunkVoxels");
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