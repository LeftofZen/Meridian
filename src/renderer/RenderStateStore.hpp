#pragma once

#include "renderer/CameraState.hpp"
#include "world/WorldRenderData.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace Meridian {

struct RenderTimingSnapshot {
    float updateDeltaMilliseconds{0.0F};
    float updateCpuMilliseconds{0.0F};
    float updatesPerSecond{0.0F};
    float renderDeltaMilliseconds{0.0F};
    float renderCpuMilliseconds{0.0F};
    float framesPerSecond{0.0F};
};

struct WorldRenderSnapshot {
    std::size_t residentChunkCount{0};
    std::size_t inFlightChunkCount{0};
    std::size_t pendingChunkCount{0};
    std::size_t uploadedVoxelCount{0};
    std::uint64_t revision{0};
    std::vector<WorldChunkRenderData> chunks;
};

struct WorldRenderSettingsSnapshot {
    float renderDistanceChunks{8.0F};
    std::uint64_t revision{0};
};

struct RenderStateSnapshot {
    RenderTimingSnapshot timing;
    WorldRenderSnapshot world;
    WorldRenderSettingsSnapshot worldRenderSettings;
    CameraRenderState camera;
};

class RenderStateStore {
public:
    void updateUpdateTiming(float updateDeltaMilliseconds, float updateCpuMilliseconds)
    {
        std::scoped_lock lock(m_mutex);
        m_snapshot.timing.updateDeltaMilliseconds = updateDeltaMilliseconds;
        m_snapshot.timing.updateCpuMilliseconds = updateCpuMilliseconds;
        m_snapshot.timing.updatesPerSecond =
            updateDeltaMilliseconds > 0.0F ? 1000.0F / updateDeltaMilliseconds : 0.0F;
    }

    void updateRenderStats(float renderDeltaMilliseconds, float renderCpuMilliseconds)
    {
        std::scoped_lock lock(m_mutex);
        m_snapshot.timing.renderDeltaMilliseconds = renderDeltaMilliseconds;
        m_snapshot.timing.renderCpuMilliseconds = renderCpuMilliseconds;
        m_snapshot.timing.framesPerSecond =
            renderDeltaMilliseconds > 0.0F ? 1000.0F / renderDeltaMilliseconds : 0.0F;
    }

    void updateWorldStats(
        std::size_t residentChunkCount,
        std::size_t inFlightChunkCount,
        std::size_t pendingChunkCount,
        std::uint64_t revision)
    {
        std::scoped_lock lock(m_mutex);
        m_snapshot.world.residentChunkCount = residentChunkCount;
        m_snapshot.world.inFlightChunkCount = inFlightChunkCount;
        m_snapshot.world.pendingChunkCount = pendingChunkCount;
        m_snapshot.world.revision = revision;
    }

    void updateWorldStats(
        std::size_t residentChunkCount,
        std::size_t inFlightChunkCount,
        std::size_t pendingChunkCount,
        std::uint64_t revision,
        std::vector<WorldChunkRenderData> chunks)
    {
        std::scoped_lock lock(m_mutex);
        m_snapshot.world.residentChunkCount = residentChunkCount;
        m_snapshot.world.inFlightChunkCount = inFlightChunkCount;
        m_snapshot.world.pendingChunkCount = pendingChunkCount;
        m_snapshot.world.revision = revision;
        m_snapshot.world.uploadedVoxelCount = 0;
        for (const WorldChunkRenderData& chunk : chunks) {
            m_snapshot.world.uploadedVoxelCount += chunk.voxelCount();
        }
        m_snapshot.world.chunks = std::move(chunks);
    }

    void updateCameraState(const CameraRenderState& camera)
    {
        std::scoped_lock lock(m_mutex);
        m_snapshot.camera = camera;
    }

    void setWorldRenderDistanceChunks(float renderDistanceChunks)
    {
        std::scoped_lock lock(m_mutex);
        const float clampedDistance = std::clamp(renderDistanceChunks, 1.0F, 32.0F);
        if (std::abs(m_snapshot.worldRenderSettings.renderDistanceChunks - clampedDistance) < 0.001F) {
            return;
        }

        m_snapshot.worldRenderSettings.renderDistanceChunks = clampedDistance;
        ++m_snapshot.worldRenderSettings.revision;
    }

    [[nodiscard]] float worldRenderDistanceChunks() const
    {
        std::scoped_lock lock(m_mutex);
        return m_snapshot.worldRenderSettings.renderDistanceChunks;
    }

    [[nodiscard]] RenderStateSnapshot snapshot() const
    {
        std::scoped_lock lock(m_mutex);
        return m_snapshot;
    }

private:
    mutable std::mutex m_mutex;
    RenderStateSnapshot m_snapshot;
};

} // namespace Meridian