#include "audio.hpp"
namespace neolip {
InterleavedAudio decodeCompressedAudio(const unsigned char* bytes,std::size_t size) {
    return neoshared::audio::decodeCompressed(bytes,size,[]{cslu::checkWork();});
}
}
