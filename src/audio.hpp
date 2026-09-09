#pragma once
#include "wav.hpp"
#include "work_control.hpp"
#include <memory>
#include <vector>
#include <string>
#include <cstddef>
namespace neolip {
struct ReviewWarning {
    std::string code, message;
    double startSeconds = 0, endSeconds = 0;
};
struct AudioBuffer {
    cslu::DecodedWaveform waveform;
    unsigned sourceChannels = 0;
    int selectedChannel = -1; // -1 is the arithmetic mean; >=0 is explicit.
    std::vector<double> channelRms;
    double rms = 0, peak = 0, clippedFraction = 0;
    bool channelChoiceRequired = false;
    std::vector<ReviewWarning> warnings;
};
// Both creation and the editor use this decoder. No trimming or gain changes.
std::shared_ptr<const AudioBuffer> decodeAudio(const std::filesystem::path& path,
                                             int channel = -1);
struct InterleavedAudio {
    unsigned sampleRate = 0, channels = 0;
    std::vector<float> samples;
};
// The compressed-codec boundary is implemented by the pinned built-in codec,
// not a subprocess. PCM/float WAVE is handled by the same strict native parser.
InterleavedAudio decodeCompressedAudio(const unsigned char* bytes, std::size_t size);
std::shared_ptr<const AudioBuffer> analyseAudio(InterleavedAudio audio, int channel = -1);
class WaveformPeaks {
public:
    explicit WaveformPeaks(std::shared_ptr<const AudioBuffer> audio = {});
    std::pair<float,float> range(std::size_t begin, std::size_t end) const;
private:
    std::shared_ptr<const AudioBuffer> audio_;
    std::vector<std::vector<std::pair<float,float>>> levels_;
};
} // namespace neolip
