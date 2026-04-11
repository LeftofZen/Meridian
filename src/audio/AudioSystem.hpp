#pragma once

#include <phonon.h>

namespace Meridian {

struct AudioConfig {
    int samplingRate{48000};
    int frameSize{512};
};

class AudioSystem {
public:
    explicit AudioSystem(const AudioConfig& config = {});
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    AudioSystem(AudioSystem&&) = delete;
    AudioSystem& operator=(AudioSystem&&) = delete;

    [[nodiscard]] bool init();
    void shutdown();

    [[nodiscard]] IPLContext getContext() const noexcept { return m_context; }
    [[nodiscard]] const IPLAudioSettings& getAudioSettings() const noexcept
    {
        return m_audioSettings;
    }
    [[nodiscard]] bool isInitialised() const noexcept { return m_initialised; }

private:
    AudioConfig m_config;
    IPLContext m_context{nullptr};
    IPLAudioSettings m_audioSettings{};
    bool m_initialised{false};
};

} // namespace Meridian
