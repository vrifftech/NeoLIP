#include "playback.hpp"
namespace neolip {
neoshared::audio::PcmView playbackPcm(std::shared_ptr<const AudioBuffer> audio);
struct AudioPlayback::Impl {neoshared::audio::AudioPlayback playback;};
AudioPlayback::AudioPlayback():impl_(std::make_unique<Impl>()){}
AudioPlayback::~AudioPlayback()=default;
void AudioPlayback::load(std::shared_ptr<const AudioBuffer> audio){impl_->playback.load(playbackPcm(std::move(audio)));}
void AudioPlayback::play(){impl_->playback.play();}
void AudioPlayback::pause(){impl_->playback.pause();}
void AudioPlayback::stop(){impl_->playback.stop();}
void AudioPlayback::seek(double seconds){impl_->playback.seek(seconds);}
void AudioPlayback::setLoop(bool enabled,double begin,double end){impl_->playback.setLoop(enabled,begin,end);}
void AudioPlayback::setVolume(float gain){impl_->playback.setVolume(gain);}
std::string AudioPlayback::pollError(){return impl_->playback.pollError();}
bool AudioPlayback::playing() const{return impl_->playback.playing();}
double AudioPlayback::position() const{return impl_->playback.position();}
}
