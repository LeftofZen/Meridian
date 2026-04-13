#pragma once

#include <string_view>

namespace Meridian {

struct SystemFrameStat {
    std::string_view name;
    float updateTimeMilliseconds{0.0F};
    float averageUpdateTimeMilliseconds{0.0F};
    float maxUpdateTimeMilliseconds{0.0F};
    float frameSharePercent{0.0F};
};

struct RenderPhaseFrameStat {
    std::string_view name;
    float timeMilliseconds{0.0F};
    float averageTimeMilliseconds{0.0F};
    float maxTimeMilliseconds{0.0F};
    float frameSharePercent{0.0F};
};

struct RenderFeatureFrameStat {
    std::string_view name;
    float configureTimeMilliseconds{0.0F};
    float beginTimeMilliseconds{0.0F};
    float recordTimeMilliseconds{0.0F};
    float totalTimeMilliseconds{0.0F};
    float averageTotalTimeMilliseconds{0.0F};
    float maxTotalTimeMilliseconds{0.0F};
    float frameSharePercent{0.0F};
};

struct RenderPhaseTimingSample {
    std::string_view name;
    float timeMilliseconds{0.0F};
};

struct RenderFeatureTimingSample {
    std::string_view name;
    float configureTimeMilliseconds{0.0F};
    float beginTimeMilliseconds{0.0F};
    float recordTimeMilliseconds{0.0F};
};

} // namespace Meridian