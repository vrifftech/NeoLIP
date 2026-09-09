#include "playback.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace neolip {
void PcmTransport::load(std::shared_ptr<const AudioBuffer> audio) {
    std::lock_guard<std::mutex> lock(mutex_);audio_=std::move(audio);cursor_=loopBegin_=0;
    loopEnd_=audio_?audio_->waveform.mono.size():0;playing_=loop_=false;
}
void PcmTransport::play() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!audio_ || audio_->waveform.mono.empty()) throw std::runtime_error("Load audio before playback.");
    if(cursor_>=audio_->waveform.mono.size()) cursor_=loop_?loopBegin_:0;
    if(loop_ && (cursor_<loopBegin_ || cursor_>=loopEnd_)) cursor_=loopBegin_;
    playing_=true;
}
void PcmTransport::pause(){std::lock_guard<std::mutex> lock(mutex_);playing_=false;}
void PcmTransport::stop(){std::lock_guard<std::mutex> lock(mutex_);playing_=false;cursor_=0;}
void PcmTransport::seek(double seconds) {
    if(!std::isfinite(seconds)) throw std::runtime_error("Playback position must be finite.");
    std::lock_guard<std::mutex> lock(mutex_);if(!audio_)return;
    const auto& wave=audio_->waveform;
    cursor_=static_cast<std::size_t>(std::clamp(seconds,0.0,wave.durationSeconds())*wave.sampleRate);
    cursor_=std::min(cursor_,wave.mono.size());
}
void PcmTransport::setLoop(bool enabled,double begin,double end) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!audio_ || !enabled){loop_=false;return;}
    const auto& wave=audio_->waveform;
    if(!std::isfinite(begin)||!std::isfinite(end)||begin<0||end<=begin||end>wave.durationSeconds()+1e-6)
        throw std::runtime_error("Select a valid audio range before enabling looping.");
    const auto a=static_cast<std::size_t>(begin*wave.sampleRate),b=std::min(wave.mono.size(),static_cast<std::size_t>(end*wave.sampleRate));
    if(b<=a) throw std::runtime_error("Loop selection is too short.");
    loopBegin_=a;loopEnd_=b;loop_=true;
    if(playing_ && (cursor_<a || cursor_>=b))cursor_=a;
}
void PcmTransport::setVolume(float gain) {
    if(!std::isfinite(gain))throw std::invalid_argument("Invalid preview volume.");
    std::lock_guard<std::mutex> lock(mutex_);gain_=std::clamp(gain,0.f,1.f);
}
bool PcmTransport::playing() const {std::lock_guard<std::mutex> lock(mutex_);return playing_;}
double PcmTransport::position() const {std::lock_guard<std::mutex> lock(mutex_);return audio_?double(cursor_)/audio_->waveform.sampleRate:0;}
std::size_t PcmTransport::render(float* output,std::size_t frames) {
    if(!output && frames)throw std::invalid_argument("Null audio buffer.");
    std::lock_guard<std::mutex> lock(mutex_);std::fill_n(output,frames,0.f);
    if(!playing_ || !audio_)return 0;
    const auto& wave=audio_->waveform;std::size_t done=0;
    while(done<frames && playing_) {
        const auto end=loop_?loopEnd_:wave.mono.size();
        if(cursor_>=end){if(loop_)cursor_=loopBegin_;else{playing_=false;break;}}
        const auto count=std::min(frames-done,end-cursor_);
        std::copy_n(wave.mono.data()+cursor_,count,output+done);done+=count;cursor_+=count;
        if(cursor_==end){if(loop_)cursor_=loopBegin_;else playing_=false;}
    }
    for(std::size_t i=0;i<done;++i)output[i]*=gain_;
    return done;
}
}
