#include "physics/PhysicsSystem.hpp"

#include "core/Logger.hpp"

#include <cstdarg>
#include <cstdio>
#include <thread>

namespace Meridian {

// Simple trace callback used by Jolt for internal logging
static void joltTrace(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, args); // NOLINT
    va_end(args);
    MRD_TRACE("[Jolt] {}", buf);
}

// ─── construction ────────────────────────────────────────────────────────────

PhysicsSystem::PhysicsSystem(const PhysicsConfig& config) : m_config(config) {}

PhysicsSystem::~PhysicsSystem()
{
    shutdown();
}

// ─── init / shutdown ─────────────────────────────────────────────────────────

bool PhysicsSystem::init()
{
    JPH::RegisterDefaultAllocator();

    JPH::Trace = joltTrace;

    JPH::Factory::sInstance = new JPH::Factory(); // NOLINT(cppcoreguidelines-owning-memory)
    JPH::RegisterTypes();

    const uint32_t allocBytes = m_config.tempAllocatorSizeMB * 1024U * 1024U;
    m_tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(allocBytes);

    // Job system: 0 = use hardware_concurrency - 1 threads
    m_jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs,
        JPH::cMaxPhysicsBarriers,
        static_cast<int>(std::thread::hardware_concurrency()) - 1);

    m_bpLayerInterface = std::make_unique<BPLayerInterfaceImpl>();
    m_objVsBPFilter = std::make_unique<ObjectVsBPLayerFilterImpl>();
    m_objLayerPairFilter = std::make_unique<ObjectLayerPairFilterImpl>();

    m_physicsSystem = std::make_unique<JPH::PhysicsSystem>();
    m_physicsSystem->Init(
        m_config.maxBodies,
        m_config.numBodyMutexes,
        m_config.maxBodyPairs,
        m_config.maxContactConstraints,
        *m_bpLayerInterface,
        *m_objVsBPFilter,
        *m_objLayerPairFilter);

    MRD_INFO("Jolt Physics ready  (max bodies: {}, threads: {})",
        m_config.maxBodies, std::thread::hardware_concurrency() - 1);
    return true;
}

void PhysicsSystem::shutdown()
{
    m_physicsSystem.reset();
    m_objLayerPairFilter.reset();
    m_objVsBPFilter.reset();
    m_bpLayerInterface.reset();
    m_jobSystem.reset();
    m_tempAllocator.reset();

    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance; // NOLINT(cppcoreguidelines-owning-memory)
    JPH::Factory::sInstance = nullptr;
}

// ─── update ──────────────────────────────────────────────────────────────────

void PhysicsSystem::update(float deltaTimeSeconds, int collisionSteps)
{
    m_physicsSystem->Update(
        deltaTimeSeconds, collisionSteps, m_tempAllocator.get(), m_jobSystem.get());
}

} // namespace Meridian
