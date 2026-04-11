#include "physics/PhysicsSystem.hpp"

#include "core/Logger.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <thread>

namespace Meridian {

static void joltTrace(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, args); // NOLINT
    va_end(args);
    MRD_TRACE("[Jolt] {}", buf);
}

PhysicsSystem::PhysicsSystem(const PhysicsConfig& config) : m_config(config) {}

PhysicsSystem::~PhysicsSystem()
{
    shutdown();
}

bool PhysicsSystem::init()
{
    JPH::RegisterDefaultAllocator();

    JPH::Trace = joltTrace;

    JPH::Factory::sInstance = new JPH::Factory(); // NOLINT(cppcoreguidelines-owning-memory)
    JPH::RegisterTypes();

    const uint32_t allocBytes = m_config.tempAllocatorSizeMB * 1024U * 1024U;
    m_tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(allocBytes);

    const unsigned int hardwareThreads = std::max(2U, std::thread::hardware_concurrency());
    const int workerThreads = static_cast<int>(hardwareThreads - 1U);
    m_jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs,
        JPH::cMaxPhysicsBarriers,
        workerThreads);

    m_bpLayerInterface = std::make_unique<BPLayerInterfaceImpl>();
    m_objLayerPairFilter = std::make_unique<JPH::ObjectLayerPairFilterTable>(Layers::NUM_LAYERS);
    m_objLayerPairFilter->EnableCollision(Layers::NON_MOVING, Layers::MOVING);
    m_objLayerPairFilter->EnableCollision(Layers::MOVING, Layers::MOVING);
    m_objVsBPFilter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
        *m_bpLayerInterface,
        BroadPhaseLayers::NUM_LAYERS,
        *m_objLayerPairFilter,
        Layers::NUM_LAYERS);

    m_physicsSystem = std::make_unique<JPH::PhysicsSystem>();
    m_physicsSystem->Init(
        m_config.maxBodies,
        m_config.numBodyMutexes,
        m_config.maxBodyPairs,
        m_config.maxContactConstraints,
        *m_bpLayerInterface,
        *m_objVsBPFilter,
        *m_objLayerPairFilter);

    m_initialised = true;
    MRD_INFO("Jolt Physics ready (max bodies: {}, threads: {})",
        m_config.maxBodies, workerThreads);
    return true;
}

void PhysicsSystem::shutdown()
{
    if (!m_initialised) {
        return;
    }

    m_physicsSystem.reset();
    m_objLayerPairFilter.reset();
    m_objVsBPFilter.reset();
    m_bpLayerInterface.reset();
    m_jobSystem.reset();
    m_tempAllocator.reset();

    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance; // NOLINT(cppcoreguidelines-owning-memory)
    JPH::Factory::sInstance = nullptr;
    m_initialised = false;
}

void PhysicsSystem::update(float deltaTimeSeconds, int collisionSteps)
{
    if (!m_initialised || !m_physicsSystem) {
        return;
    }

    m_physicsSystem->Update(
        deltaTimeSeconds, collisionSteps, m_tempAllocator.get(), m_jobSystem.get());
}

} // namespace Meridian
