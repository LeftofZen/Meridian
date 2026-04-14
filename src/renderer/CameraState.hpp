#pragma once

#include <array>

namespace Meridian {

struct CameraProjectionState {
    float verticalFovDegrees{42.0F};
    float aspectRatio{16.0F / 9.0F};
    float nearClipDistance{0.1F};
};

struct CameraRenderState {
    std::array<float, 3> position{48.0F, 56.0F, 112.0F};
    std::array<float, 3> forward{-0.36565238F, -0.3047103F, -0.879019F};
    CameraProjectionState projection{};
};

} // namespace Meridian