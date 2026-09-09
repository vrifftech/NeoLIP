#include "audio.hpp"
#include "miniaudio.h"
#include <algorithm>
#include <stdexcept>
namespace neolip {
InterleavedAudio decodeCompressedAudio(const unsigned char* bytes,std::size_t size) {
    ma_decoder decoder{};
    auto config=ma_decoder_config_init(ma_format_f32,0,0);
    if(ma_decoder_init_memory(bytes,size,&config,&decoder)!=MA_SUCCESS)
        throw std::runtime_error("Unsupported or damaged audio. Choose a PCM/float/ADPCM WAV or MP3 recording.");
    struct Cleanup { ma_decoder* p; ~Cleanup(){ma_decoder_uninit(p);} } cleanup{&decoder};
    InterleavedAudio result; result.channels=decoder.outputChannels;result.sampleRate=decoder.outputSampleRate;
    if(result.channels==0 || result.channels>32 || result.sampleRate<1000 || result.sampleRate>384000)
        throw std::runtime_error("Unsupported audio channel count or sample rate.");
    std::vector<float> block(4096*result.channels);
    for(;;) {
        cslu::checkWork(); ma_uint64 count=0;
        const auto status=ma_decoder_read_pcm_frames(&decoder,block.data(),4096,&count);
        if(status!=MA_SUCCESS && status!=MA_AT_END) throw std::runtime_error("Audio decoding failed; the recording may be damaged.");
        const auto samples=static_cast<std::size_t>(count)*result.channels;
        if(samples>block.size() || result.samples.size()+samples>128u*1024u*1024u) throw std::runtime_error("Decoded audio exceeds the memory limit.");
        result.samples.insert(result.samples.end(),block.begin(),block.begin()+static_cast<std::ptrdiff_t>(samples));
        if(status==MA_AT_END || count==0) break;
    }
    if(result.samples.empty()) throw std::runtime_error("The audio recording has no decodable samples.");
    return result;
}
}
