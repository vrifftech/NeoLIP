#include "playback.hpp"
#include "miniaudio.h"
#include <stdexcept>

namespace neolip {
struct AudioPlayback::Impl {
    PcmTransport pcm;
    ma_device device{};
#ifdef NEOLIP_AUDIO_TEST_NULL_DEVICE
    // Only the separate, non-installed native audio regression target selects
    // the deterministic null output. Product builds always use the real device.
    ma_context context{};
    bool contextReady = false;
#endif
    bool ready = false;
    bool started = false;
    std::shared_ptr<const AudioBuffer> audio;

    ~Impl() { close(); }
    void close() {
        if (ready) ma_device_uninit(&device); // Joins/stops callbacks before buffers change.
        ready = started = false;
#ifdef NEOLIP_AUDIO_TEST_NULL_DEVICE
        if (contextReady) ma_context_uninit(&context);
        contextReady = false;
#endif
    }
    void suspendDevice() {
        if (ready && started) ma_device_stop(&device);
        started = false;
    }
    void open() {
        if (ready) return;
        if (!audio) throw std::runtime_error("Load matching audio before playback.");
        auto config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 1;
        config.sampleRate = audio->waveform.sampleRate;
        config.periodSizeInMilliseconds = 10; // A request, not a promised hardware latency.
        config.pUserData = this;
        config.dataCallback = [](ma_device* d, void* out, const void*, ma_uint32 count) {
            auto* self = static_cast<Impl*>(d->pUserData);
            self->pcm.render(static_cast<float*>(out), count);
        };
        ma_context* selectedContext = nullptr;
#ifdef NEOLIP_AUDIO_TEST_NULL_DEVICE
        const ma_backend backend = ma_backend_null;
        if (ma_context_init(&backend, 1, nullptr, &context) != MA_SUCCESS)
            throw std::runtime_error("Cannot initialize the test-only null audio backend.");
        contextReady = true;
        selectedContext = &context;
#endif
        if (ma_device_init(selectedContext, &config, &device) != MA_SUCCESS) {
            close();
            throw std::runtime_error("No audio output is available. Check the selected system playback device.");
        }
        ready = true;
    }
};
AudioPlayback::AudioPlayback() : impl_(std::make_unique<Impl>()) {}
AudioPlayback::~AudioPlayback() = default;
void AudioPlayback::load(std::shared_ptr<const AudioBuffer> audio) {
    impl_->close();
    impl_->audio = std::move(audio);
    impl_->pcm.load(impl_->audio);
}
void AudioPlayback::play() {
    if(impl_->ready && impl_->started && !ma_device_is_started(&impl_->device))impl_->close();
    impl_->open();
    impl_->pcm.play();
    if (!impl_->started) {
        if (ma_device_start(&impl_->device) != MA_SUCCESS) {
            impl_->pcm.pause();
            impl_->close();
            throw std::runtime_error("Audio playback could not start.");
        }
        impl_->started = true;
    }
}
void AudioPlayback::pause() {
    // Keep the device alive outputting silence. Abruptly stopping its queue on
    // Pause can drop already submitted samples and skip them on Resume.
    impl_->pcm.pause();
}
void AudioPlayback::stop() {
    impl_->pcm.stop();
    impl_->suspendDevice();
}
void AudioPlayback::seek(double seconds) {
    const bool wasPlaying = playing();
    impl_->pcm.pause();
    impl_->suspendDevice(); // Seeking deliberately discards queued old-position audio.
    impl_->pcm.seek(seconds);
    if (wasPlaying) play();
}
void AudioPlayback::setLoop(bool enabled, double begin, double end) {
    impl_->pcm.setLoop(enabled, begin, end);
}
void AudioPlayback::setVolume(float gain) { impl_->pcm.setVolume(gain); }
std::string AudioPlayback::pollError() {
    if(impl_->ready && impl_->started && impl_->pcm.playing() && !ma_device_is_started(&impl_->device)) {
        impl_->pcm.pause();impl_->close();
        return "Audio output stopped or changed. Check the system output, then press Play to reconnect. Your edits are unchanged.";
    }
    return {};
}
bool AudioPlayback::playing() const { return impl_->pcm.playing(); }
double AudioPlayback::position() const { return impl_->pcm.position(); }
} // namespace neolip
