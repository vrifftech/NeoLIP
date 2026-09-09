#include "review.hpp"
#include <algorithm>
#include <cmath>
#include <map>
namespace neolip {
std::vector<ReviewWarning> reviewAlignment(const AudioBuffer& audio,const cslu::ViterbiAlignment& alignment,const cslu::TraceGraph& graph){
    auto warnings=audio.warnings;const auto& wave=audio.waveform;
    if(!wave.sampleRate)return warnings;
    const auto add=[&](const char* code,const char* message,double a,double b){
        if(warnings.size()<16)warnings.push_back({code,message,std::max(0.0,a),std::min(b,wave.durationSeconds())});
    };
    for(const auto& segment:alignment.phn.intervals){
        if(segment.label==".pau" || segment.label=="pau" || segment.label==".garbage")continue;
        const auto a=std::min(wave.mono.size(),static_cast<std::size_t>(std::max(0,segment.startMs)*double(wave.sampleRate)/1000));
        const auto b=std::min(wave.mono.size(),static_cast<std::size_t>(std::max(0,segment.endMs)*double(wave.sampleRate)/1000));
        if(segment.endMs-segment.startMs>1200)add("stretched-sound","A sound spans more than 1.2 seconds. Check the transcript and preview this section.",segment.startMs/1000.0,segment.endMs/1000.0);
        const auto hop=std::max<std::size_t>(1,wave.sampleRate/100);std::size_t quiet=0;
        for(auto i=a;i<b;i+=hop){
            cslu::checkWork();const auto end=std::min(i+hop,b);double energy=0;
            for(auto j=i;j<end;++j)energy+=double(wave.mono[j])*wave.mono[j];
            if(end>i && std::sqrt(energy/(end-i))<0.0003)quiet+=end-i;else quiet=0;
            if(quiet>=wave.sampleRate*.3){add("speech-in-silence","Speech was aligned across a very quiet section. Check for missing words or an internal pause.",double(end-quiet)/wave.sampleRate,double(end)/wave.sampleRate);break;}
        }
    }
    std::map<std::string,cslu::StateDurationBounds> bounds;
    for(std::size_t i=0;i<std::min(graph.statesInitial.size(),graph.stateDurations.size());++i)bounds.emplace(graph.statesInitial[i],graph.stateDurations[i]);
    std::size_t measured=0,extreme=0;
    for(const auto& s:alignment.stateSegments){
        const auto it=bounds.find(s.label);if(it==bounds.end() || it->second.maximumFrames<=0 || s.label.find("pau")!=std::string::npos)continue;
        ++measured;const double frames=(s.endMs-s.startMs)/5.0;
        if(frames<it->second.minimumFrames*.5 || frames>it->second.maximumFrames*2)++extreme;
    }
    if(measured>=12 && extreme*3>measured)add("duration-strain","Many sounds have unusually short or long durations. Verify the transcript against the recording.",0,wave.durationSeconds());
    return warnings;
}
}
