#pragma once

#include "core/SystemFrameStats.hpp"
#include "renderer/CameraState.hpp"
#include "renderer/RenderBackendStats.hpp"
#include "world/WorldRenderData.hpp"

#include <algorithm>
#include <array>
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
    float renderFrameFenceWaitMilliseconds{0.0F};
    float renderAcquireWaitMilliseconds{0.0F};
    float renderImageFenceWaitMilliseconds{0.0F};
    float renderFrontendMilliseconds{0.0F};
    float renderCommandRecordingMilliseconds{0.0F};
    float renderSubmitPresentMilliseconds{0.0F};
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
    std::vector<SystemFrameStat> systemFrameStats;
    std::vector<RenderPhaseFrameStat> renderPhaseStats;
    std::vector<RenderFeatureFrameStat> renderFeatureStats;
};

class RenderStateStore {
public:
    void updateUpdateStats(
        std::span<const SystemFrameStat> systemFrameStats,
        float updateDeltaMilliseconds,
        float updateCpuMilliseconds)
    {
        std::scoped_lock lock(m_mutex);
        rebuildSystemAccumulators(systemFrameStats);
        m_snapshot.systemFrameStats.assign(systemFrameStats.begin(), systemFrameStats.end());
        for (std::size_t index = 0; index < systemFrameStats.size(); ++index) {
            SystemMetricAccumulator& accumulator = m_systemMetricAccumulators[index];
            accumulator.metric.push(systemFrameStats[index].updateTimeMilliseconds);

            SystemFrameStat& output = m_snapshot.systemFrameStats[index];
            output.averageUpdateTimeMilliseconds = accumulator.metric.average();
            output.maxUpdateTimeMilliseconds = accumulator.metric.max();
            output.frameSharePercent =
                updateCpuMilliseconds > 0.0F
                    ? (output.updateTimeMilliseconds / updateCpuMilliseconds) * 100.0F
                    : 0.0F;
        }
        m_snapshot.timing.updateDeltaMilliseconds = updateDeltaMilliseconds;
        m_snapshot.timing.updateCpuMilliseconds = updateCpuMilliseconds;
        m_snapshot.timing.updatesPerSecond =
            updateDeltaMilliseconds > 0.0F ? 1000.0F / updateDeltaMilliseconds : 0.0F;
    }

    void updateRenderFrontendStats(
        std::span<const RenderPhaseTimingSample> renderPhaseSamples,
        std::span<const RenderFeatureTimingSample> renderFeatureSamples)
    {
        std::scoped_lock lock(m_mutex);
        rebuildRenderPhaseAccumulators(renderPhaseSamples);
        rebuildRenderFeatureAccumulators(renderFeatureSamples);

        float totalFrontendMilliseconds = 0.0F;
        for (const RenderPhaseTimingSample& sample : renderPhaseSamples) {
            totalFrontendMilliseconds += sample.timeMilliseconds;
        }

        m_snapshot.renderPhaseStats.resize(renderPhaseSamples.size());
        for (std::size_t index = 0; index < renderPhaseSamples.size(); ++index) {
            RenderMetricAccumulator& accumulator = m_renderPhaseAccumulators[index];
            accumulator.metric.push(renderPhaseSamples[index].timeMilliseconds);

            RenderPhaseFrameStat& output = m_snapshot.renderPhaseStats[index];
            output.name = renderPhaseSamples[index].name;
            output.timeMilliseconds = renderPhaseSamples[index].timeMilliseconds;
            output.averageTimeMilliseconds = accumulator.metric.average();
            output.maxTimeMilliseconds = accumulator.metric.max();
            output.frameSharePercent =
                totalFrontendMilliseconds > 0.0F
                    ? (output.timeMilliseconds / totalFrontendMilliseconds) * 100.0F
                    : 0.0F;
        }

        m_snapshot.renderFeatureStats.resize(renderFeatureSamples.size());
        for (std::size_t index = 0; index < renderFeatureSamples.size(); ++index) {
            RenderFeatureAccumulator& accumulator = m_renderFeatureAccumulators[index];
            const RenderFeatureTimingSample& input = renderFeatureSamples[index];
            const float totalMilliseconds =
                input.configureTimeMilliseconds +
                input.beginTimeMilliseconds +
                input.recordTimeMilliseconds;

            accumulator.configureMetric.push(input.configureTimeMilliseconds);
            accumulator.beginMetric.push(input.beginTimeMilliseconds);
            accumulator.recordMetric.push(input.recordTimeMilliseconds);
            accumulator.totalMetric.push(totalMilliseconds);

            RenderFeatureFrameStat& output = m_snapshot.renderFeatureStats[index];
            output.name = input.name;
            output.configureTimeMilliseconds = input.configureTimeMilliseconds;
            output.beginTimeMilliseconds = input.beginTimeMilliseconds;
            output.recordTimeMilliseconds = input.recordTimeMilliseconds;
            output.totalTimeMilliseconds = totalMilliseconds;
            output.averageTotalTimeMilliseconds = accumulator.totalMetric.average();
            output.maxTotalTimeMilliseconds = accumulator.totalMetric.max();
            output.frameSharePercent =
                totalFrontendMilliseconds > 0.0F
                    ? (totalMilliseconds / totalFrontendMilliseconds) * 100.0F
                    : 0.0F;
        }
    }

    void updateRenderStats(
        float renderDeltaMilliseconds,
        float renderCpuMilliseconds,
        const RenderBackendStats& backendStats)
    {
        std::scoped_lock lock(m_mutex);
        m_snapshot.timing.renderDeltaMilliseconds = renderDeltaMilliseconds;
        m_snapshot.timing.renderCpuMilliseconds = renderCpuMilliseconds;
        m_snapshot.timing.framesPerSecond =
            renderDeltaMilliseconds > 0.0F ? 1000.0F / renderDeltaMilliseconds : 0.0F;
        m_snapshot.timing.renderFrameFenceWaitMilliseconds = backendStats.frameFenceWaitMilliseconds;
        m_snapshot.timing.renderAcquireWaitMilliseconds = backendStats.acquireWaitMilliseconds;
        m_snapshot.timing.renderImageFenceWaitMilliseconds = backendStats.imageFenceWaitMilliseconds;
        m_snapshot.timing.renderFrontendMilliseconds = backendStats.frontendMilliseconds;
        m_snapshot.timing.renderCommandRecordingMilliseconds =
            backendStats.commandRecordingMilliseconds;
        m_snapshot.timing.renderSubmitPresentMilliseconds =
            backendStats.submitPresentMilliseconds;
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
            m_snapshot.world.uploadedVoxelCount += chunk.materialIds.size();
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
    static constexpr std::size_t kHistorySampleCount{180};

    struct RollingMetricHistory {
        std::array<float, kHistorySampleCount> samples{};
        std::size_t nextIndex{0};
        std::size_t sampleCount{0};

        void push(float value) noexcept
        {
            samples[nextIndex] = value;
            nextIndex = (nextIndex + 1) % samples.size();
            sampleCount = std::min(sampleCount + 1, samples.size());
        }

        [[nodiscard]] float average() const noexcept
        {
            if (sampleCount == 0) {
                return 0.0F;
            }

            float total = 0.0F;
            for (std::size_t index = 0; index < sampleCount; ++index) {
                total += samples[index];
            }
            return total / static_cast<float>(sampleCount);
        }

        [[nodiscard]] float max() const noexcept
        {
            if (sampleCount == 0) {
                return 0.0F;
            }

            return *std::max_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(sampleCount));
        }
    };

    struct SystemMetricAccumulator {
        std::string_view name;
        RollingMetricHistory metric;
    };

    struct RenderMetricAccumulator {
        std::string_view name;
        RollingMetricHistory metric;
    };

    struct RenderFeatureAccumulator {
        std::string_view name;
        RollingMetricHistory configureMetric;
        RollingMetricHistory beginMetric;
        RollingMetricHistory recordMetric;
        RollingMetricHistory totalMetric;
    };

    void rebuildSystemAccumulators(std::span<const SystemFrameStat> systemFrameStats)
    {
        bool rebuild = m_systemMetricAccumulators.size() != systemFrameStats.size();
        if (!rebuild) {
            for (std::size_t index = 0; index < systemFrameStats.size(); ++index) {
                if (m_systemMetricAccumulators[index].name != systemFrameStats[index].name) {
                    rebuild = true;
                    break;
                }
            }
        }

        if (!rebuild) {
            return;
        }

        m_systemMetricAccumulators.clear();
        m_systemMetricAccumulators.reserve(systemFrameStats.size());
        for (const SystemFrameStat& stat : systemFrameStats) {
            m_systemMetricAccumulators.push_back(SystemMetricAccumulator{.name = stat.name});
        }
    }

    void rebuildRenderPhaseAccumulators(std::span<const RenderPhaseTimingSample> renderPhaseSamples)
    {
        bool rebuild = m_renderPhaseAccumulators.size() != renderPhaseSamples.size();
        if (!rebuild) {
            for (std::size_t index = 0; index < renderPhaseSamples.size(); ++index) {
                if (m_renderPhaseAccumulators[index].name != renderPhaseSamples[index].name) {
                    rebuild = true;
                    break;
                }
            }
        }

        if (!rebuild) {
            return;
        }

        m_renderPhaseAccumulators.clear();
        m_renderPhaseAccumulators.reserve(renderPhaseSamples.size());
        for (const RenderPhaseTimingSample& sample : renderPhaseSamples) {
            m_renderPhaseAccumulators.push_back(RenderMetricAccumulator{.name = sample.name});
        }
    }

    void rebuildRenderFeatureAccumulators(std::span<const RenderFeatureTimingSample> renderFeatureSamples)
    {
        bool rebuild = m_renderFeatureAccumulators.size() != renderFeatureSamples.size();
        if (!rebuild) {
            for (std::size_t index = 0; index < renderFeatureSamples.size(); ++index) {
                if (m_renderFeatureAccumulators[index].name != renderFeatureSamples[index].name) {
                    rebuild = true;
                    break;
                }
            }
        }

        if (!rebuild) {
            return;
        }

        m_renderFeatureAccumulators.clear();
        m_renderFeatureAccumulators.reserve(renderFeatureSamples.size());
        for (const RenderFeatureTimingSample& sample : renderFeatureSamples) {
            m_renderFeatureAccumulators.push_back(RenderFeatureAccumulator{.name = sample.name});
        }
    }

    mutable std::mutex m_mutex;
    RenderStateSnapshot m_snapshot;
    std::vector<SystemMetricAccumulator> m_systemMetricAccumulators;
    std::vector<RenderMetricAccumulator> m_renderPhaseAccumulators;
    std::vector<RenderFeatureAccumulator> m_renderFeatureAccumulators;
};

} // namespace Meridian