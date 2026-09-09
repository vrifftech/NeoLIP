#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cslu {

enum class AudioContainerKind {
    Unknown,
    RiffWave,
    RawMp3
};

enum class AudioEncodingKind {
    Unknown,
    PcmInteger,
    PcmFloat,
    WaveExtensiblePcm,
    WaveExtensibleFloat,
    Mp3,
    MsAdpcm,
    ImaAdpcm,
    UnsupportedCompressed
};

struct WavChunkInfo {
    std::string id;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

struct WavInfo {
    std::uint16_t formatTag = 0;
    std::uint16_t effectiveFormatTag = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t byteRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t validBitsPerSample = 0;
    std::uint16_t samplesPerBlock = 0; // IMA/MS ADPCM extension field when present
    std::uint32_t channelMask = 0;
    std::uint64_t dataBytes = 0;
    std::uint64_t frames = 0;
    double durationSeconds = 0.0;
    double minSample = 0.0;
    double maxSample = 0.0;
    double meanSample = 0.0;
    AudioContainerKind containerKind = AudioContainerKind::Unknown;
    AudioEncodingKind encodingKind = AudioEncodingKind::Unknown;
    bool isRiffWave = false;
    bool isRawMp3 = false;
    bool isKotorStreamCandidate = false;
    bool canDecodeNatively = false;
    bool requiresExternalDecoder = false; // Legacy metadata field; product never runs a decoder subprocess.
    std::string description;
    std::string warning;
    std::vector<WavChunkInfo> chunks;
};

struct DecodedWaveform {
    std::uint32_t sampleRate = 0;
    std::vector<float> mono;

    double durationSeconds() const noexcept {
        return sampleRate == 0 ? 0.0
                               : static_cast<double>(mono.size()) / static_cast<double>(sampleRate);
    }
};

const char* toString(AudioContainerKind kind);
const char* toString(AudioEncodingKind kind);

WavInfo readWavInfo(const std::filesystem::path& path);
bool looksLikeMp3File(const std::filesystem::path& path);
DecodedWaveform readMonoWaveform(const std::filesystem::path& path);
void writePcm16WaveformSegment(const std::filesystem::path& path,
                               const DecodedWaveform& waveform,
                               double startSeconds,
                               double maxSeconds = 8.0);

} // namespace cslu
