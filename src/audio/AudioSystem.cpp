#include "audio/AudioSystem.hpp"

#include "core/Logger.hpp"

namespace Meridian {

AudioSystem::AudioSystem(const AudioConfig& config) : m_config(config) {}

AudioSystem::~AudioSystem()
{
    shutdown();
}

bool AudioSystem::init()
{
    m_audioSettings.samplingRate = m_config.samplingRate;
    m_audioSettings.frameSize = m_config.frameSize;

    IPLContextSettings contextSettings{};
    contextSettings.version = STEAMAUDIO_VERSION;

    const IPLerror err = iplContextCreate(&contextSettings, &m_context);
    if (err != IPL_STATUS_SUCCESS) {
        MRD_ERROR("iplContextCreate failed (error {})", static_cast<int>(err));
        return false;
    }

    MRD_INFO("Steam Audio context ready  ({}Hz, {} frames)", m_config.samplingRate,
        m_config.frameSize);
    return true;
}

void AudioSystem::shutdown()
{
    if (m_context) {
        iplContextRelease(&m_context);
        m_context = nullptr;
    }
}

} // namespace Meridian
