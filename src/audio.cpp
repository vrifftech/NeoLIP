#include "audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
namespace neolip {
namespace {
constexpr std::size_t maxBytes = 512u * 1024u * 1024u;
unsigned u16(const unsigned char* p) { return unsigned(p[0]) | (unsigned(p[1])<<8); }
std::uint32_t u32(const unsigned char* p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1])<<8) | (std::uint32_t(p[2])<<16) | (std::uint32_t(p[3])<<24); }
std::vector<unsigned char> read(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f) throw std::runtime_error("Cannot open audio: " + path.u8string());
    const auto n=f.tellg();
    if(n <= 0 || static_cast<std::uint64_t>(n)>maxBytes) throw std::runtime_error("Audio is empty or exceeds the 512 MiB input limit.");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(n)); f.seekg(0);
    for(std::size_t i=0;i<bytes.size();) {
        cslu::checkWork(); const auto take=std::min<std::size_t>(65536,bytes.size()-i);
        if(!f.read(reinterpret_cast<char*>(bytes.data()+i),static_cast<std::streamsize>(take))) throw std::runtime_error("The audio file changed or is truncated.");
        i+=take;
    }
    return bytes;
}
std::size_t mp3FrameLength(const unsigned char* p) {
    const unsigned v=(p[1]>>3)&3, layer=(p[1]>>1)&3, br=p[2]>>4, sr=(p[2]>>2)&3;
    if(p[0]!=255 || (p[1]&0xe0)!=0xe0 || v==1 || layer!=1 || br==0 || br==15 || sr==3) return 0;
    static constexpr int rates[]{44100,48000,32000};
    static constexpr int b1[]{0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
    static constexpr int b2[]{0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
    const auto rate=rates[sr]/(v==3?1:v==2?2:4);
    return static_cast<std::size_t>((v==3?144000:72000)*(v==3?b1[br]:b2[br])/rate + ((p[2]>>1)&1));
}
std::size_t mp3Offset(const std::vector<unsigned char>& bytes,std::size_t begin) {
    const auto end=std::min(bytes.size(),begin+4096);
    if(begin+3<=bytes.size() && std::memcmp(bytes.data()+begin,"ID3",3)==0) return begin;
    for(auto i=begin;i+4<=end;++i) {
        const auto n=mp3FrameLength(bytes.data()+i);
        if(n && n<=bytes.size()-i && i+n+4<=bytes.size() && mp3FrameLength(bytes.data()+i+n)) return i;
    }
    throw std::runtime_error("The KotOR audio wrapper has no valid MP3 payload.");
}
InterleavedAudio decode(const std::vector<unsigned char>& b) {
    if(b.size()<12 || std::memcmp(b.data(),"RIFF",4) || std::memcmp(b.data()+8,"WAVE",4))
        return decodeCompressedAudio(b.data(),b.size());
    unsigned tag=0,ch=0,rate=0,bits=0,align=0;
    std::vector<std::pair<std::size_t,std::size_t>> chunks;
    for(std::size_t i=12;i+8<=b.size();) {
        const auto size=static_cast<std::size_t>(u32(b.data()+i+4)), body=i+8;
        if(size>b.size()-body) throw std::runtime_error("Truncated WAVE chunk.");
        if(!std::memcmp(b.data()+i,"fmt ",4)) {
            if(size<16) throw std::runtime_error("Invalid WAVE format header.");
            tag=u16(b.data()+body); ch=u16(b.data()+body+2); rate=u32(b.data()+body+4);
            align=u16(b.data()+body+12); bits=u16(b.data()+body+14);
            if(tag==0xfffe) {
                if(size<40 || u16(b.data()+body+16)<22) throw std::runtime_error("Invalid extensible WAVE header.");
                static constexpr unsigned char tail[]{0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
                if(std::memcmp(b.data()+body+28,tail,12)) throw std::runtime_error("Unsupported WAVE subformat.");
                tag=u32(b.data()+body+24);
            }
        } else if(!std::memcmp(b.data()+i,"data",4)) {
            if(size==0 && body<b.size()) { const auto offset=mp3Offset(b,body); return decodeCompressedAudio(b.data()+offset,b.size()-offset); }
            chunks.emplace_back(body,size);
        }
        const auto pad=size+(size&1u);
        if(pad>b.size()-body) { if(size==b.size()-body) break; throw std::runtime_error("Invalid WAVE padding."); }
        i=body+pad;
    }
    if(ch==0 || ch>32 || rate<1000 || rate>384000 || chunks.empty()) throw std::runtime_error("WAVE has no valid audio data.");
    if(tag!=1 && tag!=3) {
        if(tag==0x55 && chunks.size()==1) return decodeCompressedAudio(b.data()+chunks[0].first,chunks[0].second);
        return decodeCompressedAudio(b.data(),b.size()); // IMA and Microsoft ADPCM.
    }
    if((tag==1 && bits!=8 && bits!=16 && bits!=24 && bits!=32) || (tag==3 && bits!=32 && bits!=64) || align<ch*(bits/8))
        throw std::runtime_error("Unsupported PCM WAVE format.");
    InterleavedAudio out; out.sampleRate=rate; out.channels=ch;
    for(const auto& chunk:chunks) {
        if(chunk.second%align) throw std::runtime_error("Incomplete PCM frame at end of WAVE data.");
        const auto frames=chunk.second/align;
        if(frames>maxBytes/sizeof(float)/ch || out.samples.size()>maxBytes/sizeof(float)-frames*ch) throw std::runtime_error("Decoded audio exceeds the memory limit.");
        out.samples.reserve(out.samples.size()+frames*ch);
        for(std::size_t f=0;f<frames;++f) {
            if((f&4095)==0) cslu::checkWork();
            for(unsigned c=0;c<ch;++c) {
                const auto* p=b.data()+chunk.first+f*align+c*(bits/8); double x=0;
                if(tag==1) {
                    if(bits==8) x=(int(p[0])-128)/128.0;
                    else if(bits==16) x=static_cast<std::int16_t>(u16(p))/32768.0;
                    else if(bits==24) { std::uint32_t v=u16(p)|(std::uint32_t(p[2])<<16); if(v&0x800000) v|=0xff000000; x=static_cast<std::int32_t>(v)/8388608.0; }
                    else x=static_cast<std::int32_t>(u32(p))/2147483648.0;
                } else if(bits==32) { const auto v=u32(p); float t=0; std::memcpy(&t,&v,4); x=t; }
                else { std::uint64_t v=0; for(unsigned j=0;j<8;++j) v|=std::uint64_t(p[j])<<(8*j); std::memcpy(&x,&v,8); }
                if(!std::isfinite(x)) throw std::runtime_error("Audio contains NaN or infinite samples. Export a finite PCM WAV and retry.");
                if(std::abs(x)>std::numeric_limits<float>::max()) throw std::runtime_error("Audio sample exceeds the supported range.");
                out.samples.push_back(static_cast<float>(x));
            }
        }
    }
    return out;
}
}
std::shared_ptr<const AudioBuffer> analyseAudio(InterleavedAudio data,int channel) {
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
std::shared_ptr<const AudioBuffer> decodeAudio(const std::filesystem::path& path,int channel) { return analyseAudio(decode(read(path)),channel); }
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
