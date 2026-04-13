#pragma once

namespace Meridian {

struct RenderBackendStats {
    float frameFenceWaitMilliseconds{0.0F};
    float acquireWaitMilliseconds{0.0F};
    float imageFenceWaitMilliseconds{0.0F};
    float frontendMilliseconds{0.0F};
    float commandRecordingMilliseconds{0.0F};
    float submitPresentMilliseconds{0.0F};
};

} // namespace Meridian