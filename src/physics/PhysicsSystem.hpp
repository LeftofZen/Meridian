#pragma once

// Jolt requires these before its headers
#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS_PUSH
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilter.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilter.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
JPH_SUPPRESS_WARNINGS_POP

#include <cstdint>
#include <memory>

namespace Meridian {

// ─── object / broadphase layer definitions ───────────────────────────────────

namespace Layers {
    static constexpr JPH::ObjectLayer NON_MOVING{0};
    static constexpr JPH::ObjectLayer MOVING{1};
    static constexpr JPH::ObjectLayer NUM_LAYERS{2};
} // namespace Layers

namespace BroadPhaseLayers {
    static constexpr JPH::BroadPhaseLayer NON_MOVING{0};
    static constexpr JPH::BroadPhaseLayer MOVING{1};
    static constexpr uint32_t NUM_LAYERS{2};
} // namespace BroadPhaseLayers

// Maps object layers to broad-phase layers
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl()
    {
        m_objectToBP[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        m_objectToBP[Layers::MOVING] = BroadPhaseLayers::MOVING;
    }

    [[nodiscard]] uint32_t GetNumBroadPhaseLayers() const noexcept override
    {
        return BroadPhaseLayers::NUM_LAYERS;
    }

    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(
        JPH::ObjectLayer layer) const noexcept override
    {
        return m_objectToBP[layer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    [[nodiscard]] const char* GetBroadPhaseLayerName(
        JPH::BroadPhaseLayer layer) const noexcept override
    {
        switch (static_cast<uint8_t>(layer)) {
            case static_cast<uint8_t>(BroadPhaseLayers::NON_MOVING): return "NON_MOVING";
            case static_cast<uint8_t>(BroadPhaseLayers::MOVING): return "MOVING";
            default: return "UNKNOWN";
        }
    }
#endif

private:
    JPH::BroadPhaseLayer m_objectToBP[Layers::NUM_LAYERS]{};
};

class ObjectVsBPLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    [[nodiscard]] bool ShouldCollide(
        JPH::ObjectLayer layer,
        JPH::BroadPhaseLayer bpLayer) const noexcept override
    {
        switch (layer) {
            case Layers::NON_MOVING:
                return bpLayer == BroadPhaseLayers::MOVING;
            case Layers::MOVING:
                return true;
            default:
                return false;
        }
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    [[nodiscard]] bool ShouldCollide(
        JPH::ObjectLayer obj1,
        JPH::ObjectLayer obj2) const noexcept override
    {
        switch (obj1) {
            case Layers::NON_MOVING:
                return obj2 == Layers::MOVING;
            case Layers::MOVING:
                return true;
            default:
                return false;
        }
    }
};

// ─── PhysicsSystem ───────────────────────────────────────────────────────────

struct PhysicsConfig {
    uint32_t maxBodies{65536};
    uint32_t numBodyMutexes{0}; // 0 = auto
    uint32_t maxBodyPairs{65536};
    uint32_t maxContactConstraints{16384};
    uint32_t tempAllocatorSizeMB{10};
};

class PhysicsSystem {
public:
    explicit PhysicsSystem(const PhysicsConfig& config = {});
    ~PhysicsSystem();

    PhysicsSystem(const PhysicsSystem&) = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;
    PhysicsSystem(PhysicsSystem&&) = delete;
    PhysicsSystem& operator=(PhysicsSystem&&) = delete;

    [[nodiscard]] bool init();
    void shutdown();

    void update(float deltaTimeSeconds, int collisionSteps = 1);

    [[nodiscard]] JPH::PhysicsSystem& getSystem() noexcept { return *m_physicsSystem; }
    [[nodiscard]] const JPH::PhysicsSystem& getSystem() const noexcept
    {
        return *m_physicsSystem;
    }
    [[nodiscard]] JPH::BodyInterface& getBodyInterface() noexcept
    {
        return m_physicsSystem->GetBodyInterface();
    }

private:
    PhysicsConfig m_config;

    std::unique_ptr<JPH::TempAllocatorImpl> m_tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> m_jobSystem;
    std::unique_ptr<BPLayerInterfaceImpl> m_bpLayerInterface;
    std::unique_ptr<ObjectVsBPLayerFilterImpl> m_objVsBPFilter;
    std::unique_ptr<ObjectLayerPairFilterImpl> m_objLayerPairFilter;
    std::unique_ptr<JPH::PhysicsSystem> m_physicsSystem;
};

} // namespace Meridian
