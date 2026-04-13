#pragma once

#include <string_view>

namespace Meridian {

struct SystemFrameStat {
    std::string_view name;
    float updateTimeMilliseconds{0.0F};
};

} // namespace Meridian