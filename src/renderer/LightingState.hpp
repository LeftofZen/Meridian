#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Meridian {

constexpr std::size_t kMaxPointLights = 8;
constexpr std::size_t kMaxAreaLights = 8;

struct DirectionalLightRenderState {
    std::array<float, 3> direction{-0.37363395F, 0.87594587F, -0.29890716F};
    std::array<float, 3> color{1.0F, 0.95F, 0.84F};
    float intensity{4.5F};
};

struct PointLightRenderState {
    std::array<float, 3> positionMeters{};
    std::array<float, 3> color{1.0F, 1.0F, 1.0F};
    float intensity{1600.0F};
    float rangeMeters{24.0F};
};

struct AreaLightRenderState {
    std::array<float, 3> centerMeters{};
    std::array<float, 3> rightExtentMeters{4.0F, 0.0F, 0.0F};
    std::array<float, 3> upExtentMeters{0.0F, 0.0F, 2.0F};
    std::array<float, 3> color{1.0F, 1.0F, 1.0F};
    float intensity{5.0F};
    bool doubleSided{false};
};

struct LightingRenderSnapshot {
    DirectionalLightRenderState sun;
    std::vector<PointLightRenderState> pointLights;
    std::vector<AreaLightRenderState> areaLights;
    std::uint64_t revision{0};
};

} // namespace Meridian