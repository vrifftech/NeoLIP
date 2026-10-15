#include "playback.hpp"
namespace neolip {
neoshared::audio::PcmView playbackPcm(std::shared_ptr<const AudioBuffer> audio) {
    if (!audio) return {};
    neoshared::audio::PcmView pcm;
    pcm.sampleRate=audio->waveform.sampleRate;pcm.channels=1;
    pcm.samples=audio->waveform.mono.data();pcm.frames=audio->waveform.mono.size();
    pcm.owner=std::move(audio);
    return pcm;
}
void PcmTransport::load(std::shared_ptr<const AudioBuffer> audio) {transport_.load(playbackPcm(std::move(audio)));}
void PcmTransport::play(){transport_.play();}
void PcmTransport::pause(){transport_.pause();}
void PcmTransport::stop(){transport_.stop();}
void PcmTransport::seek(double seconds){transport_.seek(seconds);}
void PcmTransport::setLoop(bool enabled,double begin,double end){transport_.setLoop(enabled,begin,end);}
void PcmTransport::setVolume(float gain){transport_.setVolume(gain);}
bool PcmTransport::playing() const{return transport_.playing();}
double PcmTransport::position() const{return transport_.position();}
std::size_t PcmTransport::render(float* output,std::size_t frames){return transport_.render(output,frames);}
}
