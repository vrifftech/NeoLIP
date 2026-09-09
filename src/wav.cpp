#include "work_control.hpp"
#include "wav.hpp"
#include "audio.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cslu {
namespace {

constexpr std::uint16_t WAVE_FORMAT_PCM = 0x0001;
constexpr std::uint16_t WAVE_FORMAT_ADPCM = 0x0002;
constexpr std::uint16_t WAVE_FORMAT_IEEE_FLOAT = 0x0003;
constexpr std::uint16_t WAVE_FORMAT_MPEGLAYER3 = 0x0055;
constexpr std::uint16_t WAVE_FORMAT_DVI_ADPCM = 0x0011;
constexpr std::uint16_t WAVE_FORMAT_EXTENSIBLE = 0xfffe;

std::uint16_t le16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0] |
                                      (p[1] << 8) |
                                      (p[2] << 16) |
                                      (p[3] << 24));
}

void readExact(std::ifstream& in, char* dst, std::streamsize n, const std::string& what) {
    in.read(dst, n);
    if (in.gcount() != n) {
        throw std::runtime_error("unexpected EOF while reading " + what);
    }
}

bool guidIsPcm(const unsigned char* g) {
    static constexpr std::array<unsigned char, 16> kPcm = {{
        0x01,0x00,0x00,0x00, 0x00,0x00, 0x10,0x00, 0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71
    }};
    return std::equal(kPcm.begin(), kPcm.end(), g);
}

bool guidIsFloat(const unsigned char* g) {
    static constexpr std::array<unsigned char, 16> kFloat = {{
        0x03,0x00,0x00,0x00, 0x00,0x00, 0x10,0x00, 0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71
    }};
    return std::equal(kFloat.begin(), kFloat.end(), g);
}

bool guidIsImaAdpcm(const unsigned char* g) {
    static constexpr std::array<unsigned char, 16> kIma = {{
        0x11,0x00,0x00,0x00, 0x00,0x00, 0x10,0x00, 0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71
    }};
    return std::equal(kIma.begin(), kIma.end(), g);
}

std::string formatTagHex(std::uint16_t tag) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(4) << std::setfill('0') << tag;
    return oss.str();
}

bool looksLikeMp3Bytes(const std::vector<unsigned char>& b) {
    if (b.size() >= 3 && b[0] == 'I' && b[1] == 'D' && b[2] == '3') return true;
    const std::size_t limit = std::min<std::size_t>(b.size(), 4096);
    for (std::size_t i = 0; i + 1 < limit; ++i) {
        if ((i & 63) == 0) checkWork();
        if (b[i] == 0xff && (b[i + 1] & 0xe0U) == 0xe0U) {
            const unsigned layer = (b[i + 1] >> 1U) & 0x03U;
            if (layer != 0) return true;
        }
    }
    return false;
}

std::vector<unsigned char> readPrefix(const std::filesystem::path& path, std::size_t n) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open audio file: " + path.string());
    std::vector<unsigned char> b(n, 0);
    in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
    b.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    return b;
}

std::vector<unsigned char> readWholeFileBytesForInfo(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open audio file: " + path.string());
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Mp3PayloadInfo {
    bool found = false;
    std::size_t offset = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t samples = 0;
    std::uint64_t payloadBytes = 0;
    std::uint32_t frameCount = 0;
    std::uint32_t bitrate = 0;
};

int mp3FrameLengthAtInfo(const std::vector<unsigned char>& b, std::size_t i, std::uint32_t* outSampleRate=nullptr,
                         std::uint32_t* outSamplesPerFrame=nullptr, std::uint32_t* outBitrate=nullptr) {
    if (i + 4 > b.size()) return 0;
    const std::uint32_t x = (static_cast<std::uint32_t>(b[i]) << 24U) |
                            (static_cast<std::uint32_t>(b[i+1]) << 16U) |
                            (static_cast<std::uint32_t>(b[i+2]) << 8U) |
                            static_cast<std::uint32_t>(b[i+3]);
    if (((x >> 21U) & 0x7ffU) != 0x7ffU) return 0;
    const unsigned ver = (x >> 19U) & 0x03U;
    const unsigned layer = (x >> 17U) & 0x03U;
    const unsigned brIndex = (x >> 12U) & 0x0fU;
    const unsigned srIndex = (x >> 10U) & 0x03U;
    const unsigned pad = (x >> 9U) & 0x01U;
    if (ver == 1 || layer == 0 || brIndex == 0 || brIndex == 15 || srIndex == 3) return 0;
    static const int brMpeg1L3[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const int brMpeg1L2[16] = {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0};
    static const int brMpeg1L1[16] = {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0};
    static const int brMpeg2L3[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const int brMpeg2L2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const int brMpeg2L1[16] = {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0};
    static const int srTable[4][4] = {{11025,12000,8000,0}, {0,0,0,0}, {22050,24000,16000,0}, {44100,48000,32000,0}};
    const bool mpeg1 = (ver == 3);
    int brKbps = 0;
    if (mpeg1 && layer == 1) brKbps = brMpeg1L3[brIndex];
    else if (mpeg1 && layer == 2) brKbps = brMpeg1L2[brIndex];
    else if (mpeg1 && layer == 3) brKbps = brMpeg1L1[brIndex];
    else if (!mpeg1 && layer == 1) brKbps = brMpeg2L3[brIndex];
    else if (!mpeg1 && layer == 2) brKbps = brMpeg2L2[brIndex];
    else if (!mpeg1 && layer == 3) brKbps = brMpeg2L1[brIndex];
    const int sr = srTable[ver][srIndex];
    if (brKbps <= 0 || sr <= 0) return 0;
    const int bitrate = brKbps * 1000;
    std::uint32_t samples = 0;
    int len = 0;
    if (layer == 3) { len = static_cast<int>((12.0 * bitrate / sr + pad) * 4.0); samples = 384; }
    else if (layer == 2) { len = static_cast<int>(144.0 * bitrate / sr + pad); samples = 1152; }
    else { len = static_cast<int>((mpeg1 ? 144.0 : 72.0) * bitrate / sr + pad); samples = mpeg1 ? 1152 : 576; }
    if (outSampleRate) *outSampleRate = static_cast<std::uint32_t>(sr);
    if (outSamplesPerFrame) *outSamplesPerFrame = samples;
    if (outBitrate) *outBitrate = static_cast<std::uint32_t>(bitrate);
    return len;
}

std::size_t findValidatedMp3SyncInfo(const std::vector<unsigned char>& b, std::size_t begin, std::size_t end) {
    end = std::min(end, b.size());
    for (std::size_t i = begin; i + 4 < end; ++i) {
        if ((i & 63) == 0) checkWork();
        if (b[i] != 0xff || (b[i+1] & 0xe0U) != 0xe0U) continue;
        const int len = mp3FrameLengthAtInfo(b, i);
        if (len <= 0) continue;
        const std::size_t next = i + static_cast<std::size_t>(len);
        if (next + 4 <= b.size() && b[next] == 0xff && (b[next+1] & 0xe0U) == 0xe0U && mp3FrameLengthAtInfo(b, next) > 0) return i;
    }
    return static_cast<std::size_t>(-1);
}

Mp3PayloadInfo findKotorAppendedMp3Info(const std::filesystem::path& path) {
    Mp3PayloadInfo out;
    const std::vector<unsigned char> b = readWholeFileBytesForInfo(path);
    if (b.size() < 64 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) return out;
    std::size_t off = 12;
    while (off + 8 <= b.size()) {
        const std::string id(reinterpret_cast<const char*>(b.data() + off), 4);
        const std::uint32_t sz = le32(b.data() + off + 4);
        const std::size_t dataStart = off + 8;
        if (id == "data" && sz == 0) {
            const std::size_t sync = findValidatedMp3SyncInfo(b, dataStart, std::min<std::size_t>(b.size(), dataStart + 4096));
            if (sync == static_cast<std::size_t>(-1)) return out;
            std::uint32_t sr = 0, spf = 0, br = 0;
            mp3FrameLengthAtInfo(b, sync, &sr, &spf, &br);
            out.found = true;
            out.offset = sync;
            out.sampleRate = sr;
            out.payloadBytes = static_cast<std::uint64_t>(b.size() - sync);
            out.bitrate = br;
            const auto searchBegin = b.begin() + static_cast<std::ptrdiff_t>(sync);
            const auto searchEnd = b.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(b.size(), sync + 256U));
            const unsigned char infoNeedle[4] = {'I', 'n', 'f', 'o'};
            const auto infoIt = std::search(searchBegin, searchEnd, std::begin(infoNeedle), std::end(infoNeedle));
            if (infoIt != searchEnd) {
                const std::size_t info = static_cast<std::size_t>(std::distance(b.begin(), infoIt));
                if (info + 16U <= b.size()) {
                    const std::uint32_t beFlags = (static_cast<std::uint32_t>(b[info + 4U]) << 24U) |
                                                  (static_cast<std::uint32_t>(b[info + 5U]) << 16U) |
                                                  (static_cast<std::uint32_t>(b[info + 6U]) << 8U) |
                                                  static_cast<std::uint32_t>(b[info + 7U]);
                    const std::size_t p = info + 8U;
                    if (beFlags & 0x1U) {
                        out.frameCount = (static_cast<std::uint32_t>(b[p]) << 24U) |
                                         (static_cast<std::uint32_t>(b[p + 1U]) << 16U) |
                                         (static_cast<std::uint32_t>(b[p + 2U]) << 8U) |
                                         static_cast<std::uint32_t>(b[p + 3U]);
                    }
                }
            }
            if (out.frameCount > 0 && spf > 0) out.samples = static_cast<std::uint64_t>(out.frameCount) * spf;
            return out;
        }
        const std::size_t next = dataStart + static_cast<std::size_t>(sz) + (sz & 1U);
        if (next <= off || next > b.size()) break;
        off = next;
    }
    return out;
}

void classify(WavInfo& info) {
    if (info.effectiveFormatTag == 0 || info.formatTag != WAVE_FORMAT_EXTENSIBLE) info.effectiveFormatTag = info.formatTag;
    if (info.containerKind == AudioContainerKind::RawMp3 || info.isRawMp3) {
        info.encodingKind = AudioEncodingKind::Mp3;
        info.requiresExternalDecoder = false; // No subprocess decoders in this product.
        info.canDecodeNatively = false;
        info.isKotorStreamCandidate = true;
        info.description = "raw MP3 payload using a .wav-style filename";
        return;
    }

    switch (info.formatTag) {
        case WAVE_FORMAT_PCM:
            info.encodingKind = AudioEncodingKind::PcmInteger;
            info.canDecodeNatively = (info.channels > 0 && info.blockAlign > 0 &&
                                      (info.bitsPerSample == 8 || info.bitsPerSample == 16 ||
                                       info.bitsPerSample == 24 || info.bitsPerSample == 32));
            info.description = "RIFF/WAVE PCM integer";
            break;
        case WAVE_FORMAT_IEEE_FLOAT:
            info.encodingKind = AudioEncodingKind::PcmFloat;
            info.canDecodeNatively = (info.channels > 0 && info.blockAlign > 0 &&
                                      (info.bitsPerSample == 32 || info.bitsPerSample == 64));
            info.description = "RIFF/WAVE IEEE float";
            break;
        case WAVE_FORMAT_EXTENSIBLE:
            if (info.effectiveFormatTag == WAVE_FORMAT_PCM) {
                info.encodingKind = AudioEncodingKind::WaveExtensiblePcm;
                info.canDecodeNatively = (info.channels > 0 && info.blockAlign > 0 &&
                                          (info.bitsPerSample == 8 || info.bitsPerSample == 16 ||
                                           info.bitsPerSample == 24 || info.bitsPerSample == 32));
                info.description = "WAVE_FORMAT_EXTENSIBLE PCM";
            } else if (info.effectiveFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                info.encodingKind = AudioEncodingKind::WaveExtensibleFloat;
                info.canDecodeNatively = (info.channels > 0 && info.blockAlign > 0 &&
                                          (info.bitsPerSample == 32 || info.bitsPerSample == 64));
                info.description = "WAVE_FORMAT_EXTENSIBLE IEEE float";
            } else if (info.effectiveFormatTag == WAVE_FORMAT_DVI_ADPCM) {
                info.encodingKind = AudioEncodingKind::ImaAdpcm;
                info.canDecodeNatively = (info.channels > 0 && info.blockAlign >= info.channels * 4 && info.sampleRate > 0);
                info.requiresExternalDecoder = false;
                info.description = "WAVE_FORMAT_EXTENSIBLE IMA/DVI ADPCM";
            } else {
                info.encodingKind = AudioEncodingKind::UnsupportedCompressed;
                info.requiresExternalDecoder = false; // No subprocess decoders in this product.
                info.description = "unsupported WAVE_FORMAT_EXTENSIBLE subformat";
            }
            break;
        case WAVE_FORMAT_MPEGLAYER3:
            info.encodingKind = AudioEncodingKind::Mp3;
            info.canDecodeNatively = true;
            info.requiresExternalDecoder = false; // No subprocess decoders in this product.
            info.isKotorStreamCandidate = true;
            info.description = "RIFF/WAVE MP3 payload; common in KOTOR stream audio";
            break;
        case WAVE_FORMAT_ADPCM:
            info.encodingKind = AudioEncodingKind::MsAdpcm;
            info.canDecodeNatively = info.channels > 0 && info.sampleRate > 0;
            info.requiresExternalDecoder = false; // No subprocess decoders in this product.
            info.description = "RIFF/WAVE Microsoft ADPCM";
            break;
        case WAVE_FORMAT_DVI_ADPCM:
            info.encodingKind = AudioEncodingKind::ImaAdpcm;
            info.canDecodeNatively = (info.channels > 0 && info.blockAlign >= info.channels * 4 && info.sampleRate > 0);
            info.requiresExternalDecoder = false;
            info.description = "RIFF/WAVE IMA/DVI ADPCM";
            break;
        default:
            info.encodingKind = AudioEncodingKind::UnsupportedCompressed;
            info.requiresExternalDecoder = false; // No subprocess decoders in this product.
            info.description = "unsupported RIFF/WAVE format tag " + formatTagHex(info.formatTag);
            break;
    }
    if (!info.canDecodeNatively && !info.requiresExternalDecoder && info.isRiffWave) {
        info.warning = "This audio layout is unsupported or structurally invalid";
    }
}

void accumulatePcm16Stats(const WavInfo& info, const std::vector<unsigned char>& data, WavInfo& out) {
    if (!info.canDecodeNatively || info.channels == 0 || info.blockAlign == 0 || data.empty()) return;
    double minv = std::numeric_limits<double>::infinity();
    double maxv = -std::numeric_limits<double>::infinity();
    long double sum = 0.0L;
    std::uint64_t sampleCount = 0;
    const std::uint64_t frames = data.size() / info.blockAlign;
    for (std::uint64_t frame = 0; frame < frames; ++frame) {
        const std::size_t base = static_cast<std::size_t>(frame * info.blockAlign);
        for (std::uint16_t ch = 0; ch < info.channels; ++ch) {
            const std::size_t off = base + static_cast<std::size_t>(ch) * (info.bitsPerSample / 8U);
            if (off >= data.size()) continue;
            double s = 0.0;
            if ((info.encodingKind == AudioEncodingKind::PcmInteger || info.encodingKind == AudioEncodingKind::WaveExtensiblePcm) && info.bitsPerSample == 8) {
                s = (static_cast<int>(data[off]) - 128) / 128.0;
            } else if ((info.encodingKind == AudioEncodingKind::PcmInteger || info.encodingKind == AudioEncodingKind::WaveExtensiblePcm) && info.bitsPerSample == 16 && off + 1 < data.size()) {
                const std::uint16_t u = static_cast<std::uint16_t>(data[off] | (data[off + 1] << 8));
                s = static_cast<double>(static_cast<std::int16_t>(u)) / 32768.0;
            } else {
                continue;
            }
            minv = std::min(minv, s);
            maxv = std::max(maxv, s);
            sum += s;
            ++sampleCount;
        }
    }
    if (sampleCount) {
        out.minSample = minv;
        out.maxSample = maxv;
        out.meanSample = static_cast<double>(sum / static_cast<long double>(sampleCount));
    }
}

} // namespace

const char* toString(AudioContainerKind kind) {
    switch (kind) {
        case AudioContainerKind::RiffWave: return "riff_wave";
        case AudioContainerKind::RawMp3: return "raw_mp3";
        default: return "unknown";
    }
}

const char* toString(AudioEncodingKind kind) {
    switch (kind) {
        case AudioEncodingKind::PcmInteger: return "pcm_integer";
        case AudioEncodingKind::PcmFloat: return "pcm_float";
        case AudioEncodingKind::WaveExtensiblePcm: return "wave_extensible_pcm";
        case AudioEncodingKind::WaveExtensibleFloat: return "wave_extensible_float";
        case AudioEncodingKind::Mp3: return "mp3";
        case AudioEncodingKind::MsAdpcm: return "ms_adpcm";
        case AudioEncodingKind::ImaAdpcm: return "ima_adpcm";
        case AudioEncodingKind::UnsupportedCompressed: return "unsupported_compressed";
        default: return "unknown";
    }
}

bool looksLikeMp3File(const std::filesystem::path& path) {
    return looksLikeMp3Bytes(readPrefix(path, 4096));
}

WavInfo readWavInfo(const std::filesystem::path& path) {
    const std::vector<unsigned char> prefix = readPrefix(path, 4096);
    WavInfo info;

    if (prefix.size() < 12 || std::memcmp(prefix.data(), "RIFF", 4) != 0 || std::memcmp(prefix.data() + 8, "WAVE", 4) != 0) {
        if (looksLikeMp3Bytes(prefix)) {
            info.containerKind = AudioContainerKind::RawMp3;
            info.isRawMp3 = true;
            classify(info);
            return info;
        }
        throw std::runtime_error("not a RIFF/WAVE file and not an MP3-looking KOTOR stream file: " + path.string());
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open WAV: " + path.string());
    unsigned char riff[12];
    readExact(in, reinterpret_cast<char*>(riff), 12, "RIFF header");

    info.containerKind = AudioContainerKind::RiffWave;
    info.isRiffWave = true;

    std::vector<unsigned char> allData;
    bool haveFmt = false;
    bool haveData = false;

    while (in) {
        const std::streamoff chunkStart = in.tellg();
        unsigned char chunkHeader[8];
        in.read(reinterpret_cast<char*>(chunkHeader), 8);
        if (in.gcount() == 0) break;
        if (in.gcount() != 8) throw std::runtime_error("truncated WAV chunk header");

        const std::uint32_t chunkSize = le32(chunkHeader + 4);
        const std::string id(reinterpret_cast<char*>(chunkHeader), 4);
        info.chunks.push_back(WavChunkInfo{id, static_cast<std::uint64_t>(chunkStart), chunkSize});

        if (id == "fmt ") {
            std::vector<unsigned char> fmt(chunkSize);
            if (chunkSize) readExact(in, reinterpret_cast<char*>(fmt.data()), chunkSize, "fmt chunk");
            if (chunkSize < 16) throw std::runtime_error("WAV fmt chunk too short");
            info.formatTag = le16(fmt.data() + 0);
            info.effectiveFormatTag = info.formatTag;
            info.channels = le16(fmt.data() + 2);
            info.sampleRate = le32(fmt.data() + 4);
            info.byteRate = le32(fmt.data() + 8);
            info.blockAlign = le16(fmt.data() + 12);
            info.bitsPerSample = le16(fmt.data() + 14);
            info.validBitsPerSample = info.bitsPerSample;
            if ((info.formatTag == WAVE_FORMAT_DVI_ADPCM || info.formatTag == WAVE_FORMAT_ADPCM) && chunkSize >= 20) {
                info.samplesPerBlock = le16(fmt.data() + 18);
            }
            if (info.formatTag == WAVE_FORMAT_EXTENSIBLE && chunkSize >= 40) {
                info.validBitsPerSample = le16(fmt.data() + 18);
                info.channelMask = le32(fmt.data() + 20);
                const unsigned char* guid = fmt.data() + 24;
                if (guidIsPcm(guid)) info.effectiveFormatTag = WAVE_FORMAT_PCM;
                else if (guidIsFloat(guid)) info.effectiveFormatTag = WAVE_FORMAT_IEEE_FLOAT;
                else if (guidIsImaAdpcm(guid)) info.effectiveFormatTag = WAVE_FORMAT_DVI_ADPCM;
            }
            haveFmt = true;
        } else if (id == "data") {
            std::vector<unsigned char> data(chunkSize);
            if (chunkSize) readExact(in, reinterpret_cast<char*>(data.data()), chunkSize, "data chunk");
            allData.insert(allData.end(), data.begin(), data.end());
            info.dataBytes += chunkSize;
            haveData = true;
            if (chunkSize == 0) break; // KOTOR stream-voice wrappers append MP3 bytes after this marker.
        } else {
            in.seekg(chunkSize, std::ios::cur);
            if (!in) throw std::runtime_error("failed skipping WAV chunk " + id);
        }

        if (chunkSize & 1U) {
            in.seekg(1, std::ios::cur);
            if (!in) break;
        }
    }

    if (!haveFmt) throw std::runtime_error("WAV missing fmt chunk");
    if (!haveData) throw std::runtime_error("WAV missing data chunk");
    const Mp3PayloadInfo mp3Payload = (info.dataBytes == 0) ? findKotorAppendedMp3Info(path) : Mp3PayloadInfo{};
    if (mp3Payload.found) {
        info.effectiveFormatTag = WAVE_FORMAT_MPEGLAYER3;
        info.encodingKind = AudioEncodingKind::Mp3;
        info.canDecodeNatively = true; // Built-in MP3 decoder.
        info.requiresExternalDecoder = false; // No subprocess decoders in this product.
        info.isKotorStreamCandidate = true;
        info.description = "KOTOR RIFF/WAVE wrapper with appended MP3 stream";
        info.warning = "RIFF header advertises PCM with a zero-length data chunk; audio payload begins at MP3 sync byte " + std::to_string(mp3Payload.offset);
        info.dataBytes = mp3Payload.payloadBytes;
        if (mp3Payload.sampleRate) info.sampleRate = mp3Payload.sampleRate;
        if (mp3Payload.samples) info.frames = mp3Payload.samples;
        if (info.sampleRate > 0 && info.frames > 0) info.durationSeconds = static_cast<double>(info.frames) / static_cast<double>(info.sampleRate);
        return info;
    }
    classify(info);
    if (info.blockAlign > 0) {
        if (info.encodingKind == AudioEncodingKind::ImaAdpcm) {
            const std::uint64_t blocks = info.dataBytes / info.blockAlign;
            const std::uint64_t spb = info.samplesPerBlock ? info.samplesPerBlock
                : (((info.blockAlign - static_cast<std::uint32_t>(4 * info.channels)) * 2U) / info.channels + 1U);
            info.frames = blocks * spb;
        } else {
            info.frames = info.dataBytes / info.blockAlign;
        }
    }
    if (info.sampleRate > 0) {
        info.durationSeconds = static_cast<double>(info.frames) / static_cast<double>(info.sampleRate);
    }
    accumulatePcm16Stats(info, allData, info);
    return info;
}

DecodedWaveform readMonoWaveform(const std::filesystem::path& path) {
    const auto audio = neolip::decodeAudio(path);
    if (audio->channelChoiceRequired) throw std::runtime_error(audio->warnings.front().message);
    return audio->waveform;
}

void writePcm16WaveformSegment(const std::filesystem::path& path,
                               const DecodedWaveform& waveform,
                               double startSeconds,
                               double maxSeconds) {
    if (waveform.sampleRate == 0 || waveform.mono.empty()) {
        throw std::runtime_error("no waveform loaded");
    }
    const auto start = std::min(
        waveform.mono.size(),
        static_cast<std::size_t>(std::max(0.0, startSeconds) * waveform.sampleRate));
    const auto maximum = static_cast<std::size_t>(
        std::max(0.0, maxSeconds) * waveform.sampleRate);
    const auto end = std::min(waveform.mono.size(), start + maximum);
    const std::uint64_t dataSize64 = static_cast<std::uint64_t>(end - start) * 2U;
    if (dataSize64 > std::numeric_limits<std::uint32_t>::max() - 36U) {
        throw std::runtime_error("waveform segment is too large for RIFF/WAVE");
    }
    const std::uint32_t dataSize = static_cast<std::uint32_t>(dataSize64);

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write WAV file: " + path.string());
    auto write16 = [&](std::uint16_t value) {
        const unsigned char data[2] = {
            static_cast<unsigned char>(value & 0xffU),
            static_cast<unsigned char>((value >> 8U) & 0xffU)};
        out.write(reinterpret_cast<const char*>(data), 2);
    };
    auto write32 = [&](std::uint32_t value) {
        const unsigned char data[4] = {
            static_cast<unsigned char>(value & 0xffU),
            static_cast<unsigned char>((value >> 8U) & 0xffU),
            static_cast<unsigned char>((value >> 16U) & 0xffU),
            static_cast<unsigned char>((value >> 24U) & 0xffU)};
        out.write(reinterpret_cast<const char*>(data), 4);
    };

    out.write("RIFF", 4);
    write32(36U + dataSize);
    out.write("WAVEfmt ", 8);
    write32(16);
    write16(WAVE_FORMAT_PCM);
    write16(1);
    write32(waveform.sampleRate);
    write32(waveform.sampleRate * 2U);
    write16(2);
    write16(16);
    out.write("data", 4);
    write32(dataSize);
    for (std::size_t i = start; i < end; ++i) {
        if ((i & 63) == 0) checkWork();
        const double value = std::clamp(static_cast<double>(waveform.mono[i]), -1.0, 1.0);
        const auto pcm = static_cast<std::int16_t>(std::lrint(value * 32767.0));
        write16(static_cast<std::uint16_t>(pcm));
    }
    out.flush();
    if (!out) throw std::runtime_error("failed writing WAV file: " + path.string());
    out.close();
    if (!out) throw std::runtime_error("failed closing WAV file: " + path.string());
}

} // namespace cslu
