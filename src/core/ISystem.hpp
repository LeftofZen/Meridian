#pragma once

namespace Meridian {

class ISystem {
public:
    virtual ~ISystem() = default;

    virtual void update(float deltaTimeSeconds) = 0;
};

} // namespace Meridian