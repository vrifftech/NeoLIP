#include "audio.hpp"
#include <neoshared/PathUtf8.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
namespace neolip {
std::shared_ptr<const AudioBuffer> analyseAudio(const InterleavedAudio& data,int channel) {
    if(!data.sampleRate || !data.channels || data.channels>32 || data.samples.empty() || data.samples.size()%data.channels)
        throw std::runtime_error("Audio recording is empty or malformed.");
    if(channel < -1 || channel>=static_cast<int>(data.channels)) throw std::runtime_error("Selected audio channel does not exist.");
    auto result=std::make_shared<AudioBuffer>(); result->sourceChannels=data.channels; result->selectedChannel=channel;
    result->waveform.sampleRate=data.sampleRate; result->channelRms.assign(data.channels,0);
    const auto frames=data.samples.size()/data.channels; result->waveform.mono.resize(frames);
    std::vector<long double> power(data.channels,0); long double sum=0, mixPower=0; std::size_t clipped=0;
    for(std::size_t i=0;i<frames;++i) {
        if((i&4095)==0) cslu::checkWork();
        double mixed=0,selected=0;
        for(unsigned c=0;c<data.channels;++c) {
            const double x=data.samples[i*data.channels+c];
            if(!std::isfinite(x)) throw std::runtime_error("Audio contains NaN or infinite samples. Export a finite PCM WAV and retry.");
            power[c]+=x*x; mixed+=x;
            if(static_cast<int>(c)==channel) selected=x;
        }
        mixed/=data.channels; mixPower+=mixed*mixed;
        if(channel<0) selected=mixed;
        result->peak=std::max(result->peak,std::abs(selected)); if(std::abs(selected)>=0.999) ++clipped;
        sum+=selected*selected;
        result->waveform.mono[i]=static_cast<float>(std::clamp(selected,-1.0,32767.0/32768.0));
    }
    for(unsigned c=0;c<data.channels;++c) result->channelRms[c]=std::sqrt(static_cast<double>(power[c]/frames));
    result->rms=std::sqrt(static_cast<double>(sum/frames)); result->clippedFraction=double(clipped)/frames;
    const auto loudest=*std::max_element(result->channelRms.begin(),result->channelRms.end());
    const auto quietest=*std::min_element(result->channelRms.begin(),result->channelRms.end());
    const auto mixRms=std::sqrt(static_cast<double>(mixPower/frames));
    const auto duration=result->waveform.durationSeconds();
    if(channel<0 && data.channels>1 && loudest>1e-4 && (mixRms<loudest*.15 || quietest<loudest*.10)) {
        result->channelChoiceRequired=true;
        result->warnings.push_back({"channel-choice",mixRms<loudest*.15 ? "The channels cancel when mixed. Select the channel containing the voice." : "The channels have very different levels. Select the channel containing the voice.",0,duration});
    }
    if(result->rms<0.0001) result->warnings.push_back({"near-silence","This recording is almost silent; speech timing may be unreliable.",0,duration});
    if(result->clippedFraction>.01) result->warnings.push_back({"clipping","Many samples are at full scale. Check the recording for clipping.",0,duration});
    return result;
}
std::shared_ptr<const AudioBuffer> decodeAudio(const std::filesystem::path& path,int channel) { return analyseAudio(*neoshared::audio::decodeFile(path,[]{cslu::checkWork();}),channel); }
WaveformPeaks::WaveformPeaks(std::shared_ptr<const AudioBuffer> audio):audio_(std::move(audio)) {
    if(!audio_ || audio_->waveform.mono.empty()) return;
    const auto& samples=audio_->waveform.mono;
    std::vector<std::pair<float,float>> base; base.reserve((samples.size()+63)/64);
    for(std::size_t i=0;i<samples.size();i+=64) {
        float lo=0,hi=0; for(auto j=i;j<std::min(i+64,samples.size());++j) {lo=std::min(lo,samples[j]);hi=std::max(hi,samples[j]);}
        base.emplace_back(lo,hi);
    }
    levels_.push_back(std::move(base));
    while(levels_.back().size()>1) {
        const auto& prev=levels_.back(); std::vector<std::pair<float,float>> next;
        for(std::size_t i=0;i<prev.size();i+=2) {auto q=prev[i];if(i+1<prev.size()){q.first=std::min(q.first,prev[i+1].first);q.second=std::max(q.second,prev[i+1].second);} next.push_back(q);}
        levels_.push_back(std::move(next));
    }
}
std::pair<float,float> WaveformPeaks::range(std::size_t a,std::size_t b) const {
    std::pair<float,float> result{0,0}; if(!audio_) return result;
    const auto& samples=audio_->waveform.mono; a=std::min(a,samples.size());b=std::min(b,samples.size());
    const auto merge=[&](std::pair<float,float> p){result.first=std::min(result.first,p.first);result.second=std::max(result.second,p.second);};
    while(a<b) {
        if(a%64 || b-a<64) {merge({samples[a],samples[a]});++a;continue;}
        std::size_t level=0,span=64;
        while(level+1<levels_.size() && a%(span*2)==0 && span*2<=b-a) {++level;span*=2;}
        merge(levels_[level][a/span]);a+=span;
    }
    return result;
}
} // namespace neolip
