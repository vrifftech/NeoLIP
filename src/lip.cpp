#include "lip.hpp"
#include "tsv.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cslu {
namespace {

std::uint32_t readU32LE(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

float readF32LE(const unsigned char* p) {
    std::uint32_t u = readU32LE(p);
    float f;
    static_assert(sizeof(float) == 4, "float must be 32-bit");
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

void writeU32LE(std::ostream& out, std::uint32_t v) {
    unsigned char b[4] = {
        static_cast<unsigned char>(v & 0xff),
        static_cast<unsigned char>((v >> 8) & 0xff),
        static_cast<unsigned char>((v >> 16) & 0xff),
        static_cast<unsigned char>((v >> 24) & 0xff)
    };
    out.write(reinterpret_cast<const char*>(b), 4);
}

void writeF32LE(std::ostream& out, float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    writeU32LE(out, u);
}

std::uint64_t temporarySaveNonce() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::uint64_t nonce = static_cast<std::uint64_t>(now) ^
                          (++counter * 0x9e3779b97f4a7c15ULL);
    try {
        nonce ^= static_cast<std::uint64_t>(std::random_device{}()) << 32U;
        nonce ^= static_cast<std::uint64_t>(std::random_device{}());
    } catch (...) {
        // A timestamp plus the process-local counter still prevents routine collisions.
    }
    return nonce;
}

class TemporarySaveDirectory {
public:
    explicit TemporarySaveDirectory(const std::filesystem::path& target) {
        const std::filesystem::path parent = target.parent_path().empty()
            ? std::filesystem::current_path()
            : target.parent_path();
        std::error_code ec;
        if (!std::filesystem::is_directory(parent, ec) || ec) {
            throw std::runtime_error("LIP output directory does not exist: " + parent.string());
        }
        for (int attempt = 0; attempt < 128; ++attempt) {
            directory_ = parent / (".neolip-save-" + std::to_string(temporarySaveNonce()));
            ec.clear();
            if (std::filesystem::create_directory(directory_, ec)) {
#ifndef _WIN32
                std::filesystem::permissions(
                    directory_, std::filesystem::perms::owner_all,
                    std::filesystem::perm_options::replace, ec);
                if (ec) {
                    std::filesystem::remove(directory_);
                    throw std::runtime_error("cannot secure temporary LIP save directory: " +
                                             ec.message());
                }
#endif
                file_ = directory_ / "payload.tmp";
                return;
            }
            if (ec && ec != std::errc::file_exists) {
                throw std::runtime_error("cannot create temporary LIP save directory: " +
                                         ec.message());
            }
        }
        throw std::runtime_error("cannot allocate a unique temporary LIP save directory");
    }

    ~TemporarySaveDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    const std::filesystem::path& file() const noexcept { return file_; }
    const std::filesystem::path& directory() const noexcept { return directory_; }

private:
    std::filesystem::path directory_;
    std::filesystem::path file_;
};

void syncCompletedFile(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "cannot open completed temporary LIP file for flush");
    }
    if (FlushFileBuffers(handle) == FALSE) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "cannot flush completed temporary LIP file");
    }
    if (CloseHandle(handle) == FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "cannot close completed temporary LIP file");
    }
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "cannot open completed temporary LIP file for flush");
    }
    if (::fsync(descriptor) != 0) {
        const int error = errno;
        ::close(descriptor);
        throw std::system_error(error, std::generic_category(),
                                "cannot flush completed temporary LIP file");
    }
    if (::close(descriptor) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "cannot close completed temporary LIP file");
    }
#endif
}

void syncDirectoryBestEffort(const std::filesystem::path& path) noexcept {
#ifndef _WIN32
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (descriptor >= 0) {
        (void)::fsync(descriptor);
        (void)::close(descriptor);
    }
#else
    (void)path;
#endif
}

void replaceWithCompletedFile(const std::filesystem::path& temporary,
                              const std::filesystem::path& target) {
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "cannot replace LIP output with completed temporary file");
    }
#else
    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        throw std::filesystem::filesystem_error(
            "cannot replace LIP output with completed temporary file", temporary, target, ec);
    }
    syncDirectoryBestEffort(target.parent_path().empty()
                                ? std::filesystem::current_path()
                                : target.parent_path());
#endif
}

void writeLipBytesAtomically(const std::filesystem::path& requestedPath,
                             const std::string& bytes) {
    if (requestedPath.empty() || requestedPath.filename().empty()) {
        throw std::runtime_error("cannot write a LIP file to an empty path");
    }
    const std::filesystem::path target = requestedPath.is_absolute()
        ? requestedPath.lexically_normal()
        : std::filesystem::absolute(requestedPath).lexically_normal();

    std::error_code ec;
    const auto status = std::filesystem::symlink_status(target, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("cannot inspect LIP output path: " + ec.message());
    }
    const bool targetExists = !ec && std::filesystem::exists(status);
    if (targetExists && !std::filesystem::is_regular_file(status)) {
        throw std::runtime_error("refusing to replace a non-regular LIP output: " +
                                 target.string());
    }

    TemporarySaveDirectory temporary(target);
    {
        std::ofstream out(temporary.file(), std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create temporary LIP output");
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) throw std::runtime_error("failed writing temporary LIP output");
        out.close();
        if (!out) throw std::runtime_error("failed closing temporary LIP output");
    }

    if (targetExists) {
        std::filesystem::permissions(
            temporary.file(), status.permissions(),
            std::filesystem::perm_options::replace, ec);
        if (ec) throw std::runtime_error("cannot preserve LIP output permissions: " + ec.message());
    }
    syncCompletedFile(temporary.file());
    replaceWithCompletedFile(temporary.file(), target);
}

float u32BitsToFloat(std::uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

std::uint32_t floatToU32Bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

[[maybe_unused]] long javaRound(double x) {
    return static_cast<long>(std::floor(x + 0.5));
}

[[maybe_unused]] unsigned char toSignedByteBits(long v) {
    return static_cast<unsigned char>(static_cast<std::int8_t>(v));
}

[[maybe_unused]] int signedByte(unsigned char b) {
    return static_cast<int>(static_cast<std::int8_t>(b));
}

[[maybe_unused]] std::uint32_t bytesBEToU32(unsigned char b0, unsigned char b1, unsigned char b2, unsigned char b3) {
    return (static_cast<std::uint32_t>(b0) << 24) |
           (static_cast<std::uint32_t>(b1) << 16) |
           (static_cast<std::uint32_t>(b2) << 8)  |
           static_cast<std::uint32_t>(b3);
}

std::string normalizePhoneLabel(std::string p) {
    p = trimCR(p);
    while (!p.empty() && (p.back() == ',' || p.back() == ';')) p.pop_back();
    return p;
}

bool isRestOrPausePhone(const std::string& phoneIn) {
    const std::string phn = normalizePhoneLabel(phoneIn);
    if (phn.empty()) return false;
    if (phn == ".pau" || phn == "pau" || phn == "sil" || phn == "sp" || phn == "h#") return true;
    // CSLU pause/silence labels are normally dot-prefixed. Treat unknown
    // dot-prefixed labels as silence-like rather than mouth shapes.
    return !phn.empty() && phn[0] == '.';
}

bool isPreservedClosureShape(std::uint8_t shape) {
    // Visually important closures/fricatives/plosive contacts. These should not
    // be smoothed out merely because they are short.
    return shape == 11 || // M/P/B
           shape == 8  || // F/V
           shape == 15 || // K/G
           shape == 12;   // T/D
}

int anticipationMsForShape(std::uint8_t shape, const LipQualityOptions& post) {
    // These offsets shift only the visible mouth-shape key earlier. The PHN
    // acoustic alignment remains unchanged. Values are intentionally small so
    // plosive/fricative mouth contacts can be visible before the sound begins.
    switch (shape) {
        case 11: // M/P/B bilabial closure
            return std::max(0, post.anticipateClosuresMs);
        case 8:  // F/V
        case 10: // TH -- dental fricative, handled with the fricative offset
            return std::max(0, post.anticipateFricativesMs);
        case 12: // T/D
        case 15: // K/G
            return std::max(0, post.anticipateStopsMs);
        default:
            return 0;
    }
}

int lipTimeMs(double seconds) {
    if (!std::isfinite(seconds)) return 0;
    return static_cast<int>(std::llround(seconds * 1000.0));
}

void addVisibleEntry(std::vector<KotorVisibleKeyframe>& entries, int timeMs,
                     std::uint8_t shape) {
    if (timeMs < 0) timeMs = 0;
    if (shape > 15) shape = 15;
    const double timeSeconds = static_cast<double>(timeMs) / 1000.0;
    if (!entries.empty()) {
        if (entries.back().timeSeconds == timeSeconds) {
            entries.back().shape = shape;
            return;
        }
        if (entries.back().shape == shape) return;
    }
    entries.push_back({timeSeconds, shape});
}

std::vector<KotorVisibleKeyframe> smoothVisibleEntries(
    std::vector<KotorVisibleKeyframe> entries, const LipQualityOptions& post) {
    if (entries.empty()) return entries;
    std::sort(entries.begin(), entries.end(), [](const KotorVisibleKeyframe& a,
                                                  const KotorVisibleKeyframe& b) {
        return a.timeSeconds < b.timeSeconds;
    });

    std::vector<KotorVisibleKeyframe> compact;
    for (auto entry : entries) {
        if (entry.shape > 15) entry.shape = 15;
        if (!compact.empty() && compact.back().timeSeconds == entry.timeSeconds) {
            compact.back() = entry;
        } else if (!compact.empty() && compact.back().shape == entry.shape) {
            continue;
        } else {
            compact.push_back(entry);
        }
    }
    if (post.minKeyMs <= 0 || compact.size() <= 2) return compact;

    std::vector<KotorVisibleKeyframe> out;
    out.reserve(compact.size());
    out.push_back(compact.front());
    for (std::size_t i = 1; i < compact.size(); ++i) {
        const KotorVisibleKeyframe& cur = compact[i];
        KotorVisibleKeyframe& prev = out.back();
        const int curMs = lipTimeMs(cur.timeSeconds);
        const int prevMs = lipTimeMs(prev.timeSeconds);
        const int gap = curMs - prevMs;
        if (gap >= post.minKeyMs || cur.shape == 0) {
            if (prev.shape != cur.shape) out.push_back(cur);
            continue;
        }

        const bool curPreserved = post.preserveClosures && isPreservedClosureShape(cur.shape);
        const bool prevPreserved = post.preserveClosures && isPreservedClosureShape(prev.shape);
        if (curPreserved) {
            if (!prevPreserved && prev.shape != 0 && out.size() > 1) {
                prev = cur;
            } else if (prev.shape != cur.shape) {
                out.push_back(cur);
            }
        } else if (prevPreserved) {
            continue;
        } else {
            prev = cur;
        }
    }

    std::vector<KotorVisibleKeyframe> finalEntries;
    for (const auto& entry : out) {
        if (!finalEntries.empty() && finalEntries.back().shape == entry.shape) continue;
        finalEntries.push_back(entry);
    }
    return finalEntries;
}

std::vector<KotorVisibleKeyframe> phnToKotorVisibleEntries(
    const PhnFile& phn, const LipQualityOptions& post) {
    std::vector<KotorVisibleKeyframe> out;
    std::int16_t lastMouth = -1;
    int lastStartTime = -1;
    for (const auto& iv : phn.intervals) {
        std::int16_t mouth = csluPhoneToKotorShapeOrSkip(iv.label);
        if (mouth == -1) {
            if (post.closeOnPause && isRestOrPausePhone(iv.label) &&
                (iv.endMs - iv.startMs) >= post.closeOnPauseMs) {
                addVisibleEntry(out, iv.startMs, 0);
                lastMouth = 0;
            }
            continue;
        }
        int start = iv.startMs;
        if (lastMouth == 16) start = lastStartTime;
        lastMouth = mouth;
        if (lastMouth == 16) {
            lastStartTime = iv.startMs;
            continue;
        }
        const std::uint8_t shape = static_cast<std::uint8_t>(lastMouth);
        start = std::max(0, start - anticipationMsForShape(shape, post));
        addVisibleEntry(out, start, shape);
    }
    return smoothVisibleEntries(std::move(out), post);
}

} // namespace

int resolveTailRestMs(int phnDurationMs, const LipTailRestOptions& options) {
    if (!options.automatic) return std::max(0, options.fixedMs);
    if (phnDurationMs <= 0) return 0;
    // Stock module lips commonly carry a short final rest beyond the aligned speech.
    // Auto is intentionally simple and predictable: short barks get 250 ms,
    // normal dialogue gets 400 ms, and long lines get 500 ms.
    if (phnDurationMs < 1500) return 250;
    if (phnDurationMs < 8000) return 400;
    return 500;
}

LipSmoothingPreset parseLipSmoothingPreset(const std::string& valueIn) {
    std::string value = valueIn;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (value == "none" || value == "off" || value == "0") return LipSmoothingPreset::None;
    if (value == "light") return LipSmoothingPreset::Light;
    if (value == "medium" || value == "med") return LipSmoothingPreset::Medium;
    if (value == "strong") return LipSmoothingPreset::Strong;
    throw std::runtime_error("unknown LIP smoothing preset: " + valueIn);
}

const char* lipSmoothingPresetName(LipSmoothingPreset preset) {
    switch (preset) {
        case LipSmoothingPreset::None: return "none";
        case LipSmoothingPreset::Light: return "light";
        case LipSmoothingPreset::Medium: return "medium";
        case LipSmoothingPreset::Strong: return "strong";
    }
    return "light";
}

void applyLipSmoothingPreset(LipQualityOptions& options, LipSmoothingPreset preset) {
    options.smoothing = preset;
    switch (preset) {
        case LipSmoothingPreset::None:
            options.minKeyMs = 0;
            options.preserveClosures = false;
            break;
        case LipSmoothingPreset::Light:
            options.minKeyMs = 40;
            options.preserveClosures = true;
            break;
        case LipSmoothingPreset::Medium:
            options.minKeyMs = 60;
            options.preserveClosures = true;
            break;
        case LipSmoothingPreset::Strong:
            options.minKeyMs = 80;
            options.preserveClosures = true;
            break;
    }
}

const std::vector<LipShapeInfo>& kotorLipShapes() {
    static const std::vector<LipShapeInfo> shapes = {
        {0,  "EE",    "i:"},
        {1,  "EH",    "I, I_x, E, ei, e&"},
        {2,  "SCHWA", ">, 3r, &r"},
        {3,  "AH",    "@, A, ^, aI, aU"},
        {4,  "OH",    "U, &, &_0, 5, >i, oU"},
        {5,  "OOH",   "u, u_x, u&, w"},
        {6,  "Y",     "j"},
        {7,  "S/TS",  "s, z"},
        {8,  "F/V",   "f, v"},
        {9,  "N/NG",  "n, N, n_(, n="},
        {10, "TH",    "T, D"},
        {11, "M/P/B", "ph, pc, b, bc, m, m=, +"},
        {12, "T/D",   "th, tc, d, dc, d_(, th_("},
        {13, "J/SH",  "S, Z, tS, tSc, dZ, dZc"},
        {14, "L/R",   "l=, l, 9r"},
        {15, "K/G",   "kh, kc, g, gc"},
    };
    return shapes;
}

const char* kotorLipShapeName(std::uint8_t shape) {
    const auto& s = kotorLipShapes();
    if (shape < s.size()) return s[shape].name;
    return "OUT_OF_RANGE";
}

std::uint32_t kotorStockTimeToPos(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    return floatToU32Bits(static_cast<float>(seconds));
}

double kotorStockPosToTime(std::uint32_t position) {
    return static_cast<double>(u32BitsToFloat(position));
}

float kotorStockPosToFloat(std::uint32_t position) { return u32BitsToFloat(position); }
std::uint32_t kotorStockFloatToPos(float f) { return floatToU32Bits(f); }

LipFile readLip(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open LIP file: " + path.string());
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (data.size() < 16) throw std::runtime_error("LIP file too short: " + path.string());
    if (std::memcmp(data.data(), "LIP V1.0", 8) != 0) {
        throw std::runtime_error("invalid LIP signature: " + path.string());
    }

    LipFile lip;
    lip.durationSeconds = readF32LE(data.data() + 8);
    const std::uint32_t recordCount = readU32LE(data.data() + 12);
    if (recordCount == 0) {
        throw std::runtime_error("LIP record count is zero: " + path.string());
    }
    if (recordCount > (data.size() - 16U) / 5U) {
        throw std::runtime_error("truncated LIP record table: " + path.string());
    }
    const std::size_t expectedSize =
        16U + 5U * static_cast<std::size_t>(recordCount);
    if (data.size() != expectedSize) {
        throw std::runtime_error(
            "LIP file size does not match its uint32 record count: " + path.string());
    }

    lip.frames.reserve(recordCount);
    const unsigned char* record = data.data() + 16;
    for (std::uint32_t i = 0; i < recordCount; ++i, record += 5) {
        lip.frames.push_back({readF32LE(record), record[4]});
    }
    return lip;
}

void writeLip(const std::filesystem::path& path, const LipFile& lip) {
    if (lip.frames.empty()) throw std::runtime_error("LIP requires at least one record");
    if (lip.frames.size() > 0xffffffffull) throw std::runtime_error("too many LIP records");
    if (!(lip.durationSeconds > 0.0f) || !std::isfinite(lip.durationSeconds)) {
        throw std::runtime_error("LIP duration must be positive and finite");
    }
    if (lip.frames.front().timeSeconds != 0.0f || lip.frames.front().shape != 0) {
        throw std::runtime_error("LIP first record must be time 0.0 with shape 0");
    }
    for (std::size_t i = 0; i < lip.frames.size(); ++i) {
        const LipKeyframe& frame = lip.frames[i];
        if (!std::isfinite(frame.timeSeconds) || frame.timeSeconds < 0.0f) {
            throw std::runtime_error("LIP record times must be finite and non-negative");
        }
        if (frame.shape > 15) {
            throw std::runtime_error("LIP shape ids must be in the range 0..15");
        }
        if (i > 0 && frame.timeSeconds < lip.frames[i - 1].timeSeconds) {
            throw std::runtime_error("LIP record times must be monotonic");
        }
    }
    if (lip.frames.back().timeSeconds != lip.durationSeconds) {
        throw std::runtime_error("LIP duration must equal the final record time");
    }

    std::ostringstream out(std::ios::out | std::ios::binary);
    out.write("LIP V1.0", 8);
    writeF32LE(out, lip.durationSeconds);
    writeU32LE(out, static_cast<std::uint32_t>(lip.frames.size()));
    for (const auto& record : lip.frames) {
        writeF32LE(out, record.timeSeconds);
        out.put(static_cast<char>(record.shape));
    }
    if (!out) throw std::runtime_error("failed serializing LIP file: " + path.string());
    writeLipBytesAtomically(path, out.str());
}

LipValidation validateLipForKotorRuntime(const LipFile& lip, std::uintmax_t fileSize) {
    LipValidation v;
    auto error = [&](const std::string& s){ v.ok = false; v.errors.push_back(s); };
    auto warn = [&](const std::string& s){ v.warnings.push_back(s); };
    if (!(lip.durationSeconds > 0.0f) || !std::isfinite(lip.durationSeconds)) error("duration field must be positive finite");
    if (lip.frames.empty()) { error("entry count must be greater than zero"); return v; }
    if (fileSize != 0) {
        const std::uintmax_t expected = 16 + 5 * static_cast<std::uintmax_t>(lip.frames.size());
        if (fileSize != expected) warn("file size is not exactly 16 + 5*entry_count");
    }
    const float firstNorm = lip.frames.front().timeSeconds / lip.durationSeconds;
    if (!(firstNorm == 0.0f)) error("first record time must normalize to exactly 0.0");
    if (lip.frames.front().shape != 0) error("first record shape must be the rest shape 0");
    const float lastNorm = lip.frames.back().timeSeconds / lip.durationSeconds;
    if (!(lastNorm < 1.000010014f && lastNorm > 0.999989986f)) warn("last record does not normalize to approximately 1.0");
    for (std::size_t i = 0; i < lip.frames.size(); ++i) {
        const auto& k = lip.frames[i];
        if (!std::isfinite(k.timeSeconds)) error("non-finite record time at index " + std::to_string(i));
        if (i > 0 && k.timeSeconds < lip.frames[i-1].timeSeconds) warn("non-monotonic record time at index " + std::to_string(i));
        if (k.shape > 15) warn("shape id outside safe 0..15 at index " + std::to_string(i));
    }
    return v;
}

std::vector<KotorVisibleKeyframe> readKotorVisibleKeyframes(
    const std::filesystem::path& path, double* durationSeconds) {
    const LipFile lip = readLip(path);
    if (lip.frames.front().timeSeconds != 0.0f || lip.frames.front().shape != 0) {
        throw std::runtime_error(
            "LIP first record is not the required time 0.0, shape 0 rest record: " +
            path.string());
    }
    if (durationSeconds != nullptr) {
        *durationSeconds = static_cast<double>(lip.durationSeconds);
    }

    std::vector<KotorVisibleKeyframe> entries;
    entries.reserve(lip.frames.size() - 1U);
    for (std::size_t i = 1; i < lip.frames.size(); ++i) {
        entries.push_back(
            {static_cast<double>(lip.frames[i].timeSeconds), lip.frames[i].shape});
    }
    if (entries.empty()) {
        const double duration = static_cast<double>(lip.durationSeconds);
        entries.push_back({duration > 0.0 ? duration : 0.125, 0});
    }
    return entries;
}

void normalizeKotorVisibleKeyframes(std::vector<KotorVisibleKeyframe>& entries) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const auto& entry) {
        return !std::isfinite(entry.timeSeconds);
    }), entries.end());

    for (auto& entry : entries) {
        entry.timeSeconds = std::max(0.0, entry.timeSeconds);
        entry.shape = static_cast<std::uint8_t>(std::min<int>(entry.shape, 15));
    }
    std::stable_sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.timeSeconds < rhs.timeSeconds;
    });

    double previous = -1.0;
    for (auto& entry : entries) {
        if (previous >= 0.0 && entry.timeSeconds <= previous) {
            entry.timeSeconds = previous + 0.001;
        }
        previous = entry.timeSeconds;
    }
    if (entries.empty()) entries.push_back({0.125, 0});
    entries.back().shape = 0;
}

void writeKotorVisibleKeyframes(const std::filesystem::path& path,
                                std::vector<KotorVisibleKeyframe> entries) {
    normalizeKotorVisibleKeyframes(entries);
    if (entries.size() > 0xfffffffeull) {
        throw std::runtime_error("too many visible LIP keyframes");
    }

    LipFile lip;
    lip.durationSeconds = static_cast<float>(entries.back().timeSeconds);
    lip.frames.reserve(entries.size() + 1U);
    lip.frames.push_back({0.0f, 0});
    for (const auto& entry : entries) {
        lip.frames.push_back({static_cast<float>(entry.timeSeconds), entry.shape});
    }
    writeLip(path, lip);
}

std::int16_t csluPhoneToKotorShapeOrSkip(const std::string& phoneIn) {
    const std::string phn = normalizePhoneLabel(phoneIn);
    static const std::map<std::string, int> table = {
        {"i:",0}, {"I",1}, {"I_x",1}, {"E",1}, {"@",3}, {"A",3}, {"^",3}, {">",2},
        {"U",4}, {"u",5}, {"u_x",5}, {"&",4}, {"&_0",4}, {"3r",2}, {"&r",2}, {"5",4},
        {"ei",1}, {">i",4}, {"aI",3}, {"aU",3}, {"oU",4}, {"iU",0}, {"i&",0}, {"u&",5}, {"e&",1},
        {"ph",11}, {"pc",11}, {"b",11}, {"bc",11}, {"th",12}, {"tc",12}, {"d",12}, {"dc",12},
        {"kh",15}, {"kc",15}, {"g",15}, {"gc",15}, {"f",8}, {"v",8}, {"T",10}, {"D",10}, {"s",7}, {"z",7},
        {"S",13}, {"Z",13}, {"h",16}, {"h_v",16}, {"tS",13}, {"tSc",13}, {"dZ",13}, {"dZc",13},
        {"m",11}, {"n",9}, {"N",9}, {"d_(",12}, {"th_(",12}, {"n_(",9}, {"l=",14}, {"m=",11}, {"n=",9},
        {"l",14}, {"9r",14}, {"j",6}, {"w",5}, {"+",11}
    };
    const auto it = table.find(phn);
    if (it != table.end()) return static_cast<std::int16_t>(it->second);
    return -1;
}

std::uint8_t csluPhoneToKotorShape(const std::string& phone) {
    const std::int16_t s = csluPhoneToKotorShapeOrSkip(phone);
    if (s >= 0 && s <= 15) return static_cast<std::uint8_t>(s);
    return 0;
}

void writePhnAsLip(const std::filesystem::path& path, const PhnFile& phn,
                   LipTailRestOptions tailRest, LipQualityOptions post) {
    std::vector<KotorVisibleKeyframe> entries = phnToKotorVisibleEntries(phn, post);
    int durationMs = 125;
    if (!phn.intervals.empty()) durationMs = std::max(0, phn.intervals.back().endMs);
    durationMs += resolveTailRestMs(durationMs, tailRest);
    const double finalTimeSeconds = static_cast<double>(durationMs) / 1000.0;

    if (entries.empty()) {
        entries.push_back({finalTimeSeconds, 0});
    } else if (entries.back().timeSeconds == finalTimeSeconds) {
        entries.back().shape = 0;
    } else {
        entries.push_back({finalTimeSeconds, 0});
    }

    LipFile lip;
    lip.durationSeconds = static_cast<float>(finalTimeSeconds);
    lip.frames.reserve(entries.size() + 1U);
    lip.frames.push_back({0.0f, 0});
    for (const auto& entry : entries) {
        lip.frames.push_back({static_cast<float>(entry.timeSeconds), entry.shape});
    }
    writeLip(path, lip);
}

} // namespace cslu
