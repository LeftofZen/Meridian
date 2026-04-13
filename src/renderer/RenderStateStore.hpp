#pragma once

#include "core/SystemFrameStats.hpp"

#include <mutex>
#include <span>
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
};

struct RenderStateSnapshot {
    RenderTimingSnapshot timing;
    WorldRenderSnapshot world;
    std::vector<SystemFrameStat> systemFrameStats;
};

class RenderStateStore {
public:
    void updateUpdateStats(
        std::span<const SystemFrameStat> systemFrameStats,
        float updateDeltaMilliseconds,
        float updateCpuMilliseconds)
    {
        std::scoped_lock lock(m_mutex);
        m_snapshot.systemFrameStats.assign(systemFrameStats.begin(), systemFrameStats.end());
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
        std::size_t pendingChunkCount)
    {
        std::scoped_lock lock(m_mutex);
        m_snapshot.world.residentChunkCount = residentChunkCount;
        m_snapshot.world.inFlightChunkCount = inFlightChunkCount;
        m_snapshot.world.pendingChunkCount = pendingChunkCount;
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