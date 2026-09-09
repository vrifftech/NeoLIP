#pragma once
#include "audio.hpp"
#include <memory>
#include <mutex>
namespace neolip {
// Shared, testable state machine. The audio callback consumes PCM frames;
// wall-clock timers never advance this cursor. Device buffering adds latency.
class PcmTransport {
public:
    void load(std::shared_ptr<const AudioBuffer> audio);
    void play(); void pause(); void stop(); void seek(double seconds);
    void setLoop(bool enabled,double begin,double end);
    void setVolume(float gain);
    bool playing() const; double position() const;
    std::size_t render(float* output,std::size_t frames);
private:
    mutable std::mutex mutex_;
    std::shared_ptr<const AudioBuffer> audio_;
    std::size_t cursor_=0,loopBegin_=0,loopEnd_=0;
    bool playing_=false,loop_=false;
    float gain_=1.f;
};
class AudioPlayback {
public:
    AudioPlayback(); ~AudioPlayback();
    AudioPlayback(const AudioPlayback&)=delete;
    AudioPlayback& operator=(const AudioPlayback&)=delete;
    void load(std::shared_ptr<const AudioBuffer> audio);
    void play(); void pause(); void stop(); void seek(double seconds);
    void setLoop(bool enabled,double begin,double end);
    void setVolume(float gain);
    std::string pollError(); // Called on the UI thread; reconnect on the next Play.
    bool playing() const; double position() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
