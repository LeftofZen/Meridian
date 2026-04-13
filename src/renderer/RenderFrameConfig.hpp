#pragma once

#include <array>

namespace Meridian {

struct RenderFrameConfig {
    std::array<float, 4> clearColor{0.08F, 0.09F, 0.11F, 1.0F};
};

} // namespace Meridian
