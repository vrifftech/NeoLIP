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
#include <limits>
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

int anticipationMsForShape(std::uint8_t shape, const LipQualityOptions& post) {
    // Nonzero values are opt-in authoring settings, not game rules.
    switch (shape) {
        case 11: return std::max(0, post.anticipateClosuresMs);
        case 8: case 10: return std::max(0, post.anticipateFricativesMs);
        case 12: case 15: return std::max(0, post.anticipateStopsMs);
        default: return 0;
    }
}

struct PoseSpan {
    double start = 0, end = 0;
    std::uint8_t shape = 0;
};

std::vector<PoseSpan> phonemeSpans(const PhnFile& phn, const LipQualityOptions& post) {
    std::vector<PoseSpan> spans;
    int previousEnd = 0;
    int pendingBreathStart = -1;
    for (std::size_t i = 0; i < phn.intervals.size(); ++i) {
        const auto& iv = phn.intervals[i];
        if (iv.startMs < previousEnd || iv.endMs < iv.startMs || iv.startMs < 0)
            throw std::runtime_error("Phoneme intervals must be ordered, non-negative and non-overlapping.");
        previousEnd = iv.endMs;
        if (iv.startMs == iv.endMs) continue;
        const bool pause = isRestOrPausePhone(iv.label);
        auto mouth = csluPhoneToKotorShapeOrSkip(iv.label);
        if (pause) {
            pendingBreathStart = -1;
            // Leading/trailing silence gets its own hold even when brief.
            if (!post.closeOnPause || (i != 0 && i + 1 != phn.intervals.size() &&
                    iv.endMs - iv.startMs < post.closeOnPauseMs)) continue;
            mouth = 0;
        } else if (mouth == 16) {
            // Preserve the previous HH-to-following-vowel convention.
            if (pendingBreathStart < 0) pendingBreathStart = iv.startMs;
            continue;
        } else if (mouth < 0) {
            pendingBreathStart = -1;
            continue;
        }
        const int start = pendingBreathStart >= 0 ? pendingBreathStart : iv.startMs;
        pendingBreathStart = -1;
        PoseSpan span{start / 1000.0, iv.endMs / 1000.0, static_cast<std::uint8_t>(mouth)};
        if (!spans.empty() && spans.back().shape == span.shape) spans.back().end = span.end;
        else spans.push_back(span);
    }
    return spans;
}

void appendGeneratedKey(LipFile& lip, double time, std::uint8_t shape) {
    const float t = static_cast<float>(time);
    if (!std::isfinite(t) || t < 0 || t > lip.durationSeconds || shape > 15)
        throw std::runtime_error("Generated LIP target is outside the clip.");
    if (!lip.frames.empty() && t <= lip.frames.back().timeSeconds) {
        if (t == lip.frames.back().timeSeconds && shape == lip.frames.back().shape) return;
        throw std::runtime_error("Mouth targets are too close for the LIP timestamp format.");
    }
    lip.frames.push_back({t, shape});
}

} // namespace

int resolveTailRestMs(int phnDurationMs, const LipTailRestOptions& options) {
    if (!options.automatic) return std::max(0, options.fixedMs);
    if (phnDurationMs <= 0) return 0;
    // Legacy opt-in authoring policy, NOT a requirement of KotOR playback.
    // Normal product generation keeps fixedMs=0 and does not extend the WAV.
    if (phnDurationMs < 1500) return 250;
    if (phnDurationMs < 8000) return 400;
    return 500;
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
    LipFile lip = neoshared::lip::readLip(data, path.string());
    if (lip.frames.empty()) {
        throw std::runtime_error("LIP record count is zero: " + path.string());
    }
    const std::size_t expectedSize =
        16U + 5U * lip.frames.size();
    if (data.size() != expectedSize) {
        throw std::runtime_error(
            "LIP file size does not match its uint32 record count: " + path.string());
    }
    return lip;
}

void writeLip(const std::filesystem::path& path, const LipFile& lip) {
    if (lip.frames.empty()) throw std::runtime_error("LIP requires at least one record");
    if (lip.frames.size() > 0xffffffffull) throw std::runtime_error("too many LIP records");
    if (!(lip.durationSeconds > 0.0f) || !std::isfinite(lip.durationSeconds)) {
        throw std::runtime_error("LIP duration must be positive and finite");
    }
    const auto validation = validateLipForKotorRuntime(lip);
    if (!validation.ok) {
        std::string message = "LIP was not saved. The animation needs explicit repair:";
        for (const auto& error : validation.errors) message += "\n" + error;
        throw std::runtime_error(message);
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
    auto error = [&](const std::string& message) { v.ok = false; v.errors.push_back(message); };
    auto warn = [&](const std::string& message) { v.warnings.push_back(message); };
    if (!(lip.durationSeconds > 0.0f) || !std::isfinite(lip.durationSeconds))
        error("Clip duration must be positive and finite.");
    if (lip.frames.empty()) { error("At least one mouth key is required."); return v; }
    if (fileSize != 0 && fileSize != 16 + 5 * static_cast<std::uintmax_t>(lip.frames.size()))
        error("File size does not match the counted keys.");
    if (lip.frames.front().timeSeconds != 0.0f) error("The first key must be at time zero.");
    if (lip.frames.front().shape != 0) warn("The starting pose differs from the usual pose 0; it will be preserved.");
    if (lip.durationSeconds > 0.0f && std::isfinite(lip.durationSeconds)) {
        const float lastNorm = lip.frames.back().timeSeconds / lip.durationSeconds;
        if (!(lastNorm < 1.000010014f && lastNorm > 0.999989986f))
            error("The final key must match the stored clip duration (within the game's tolerance).");
    }
    bool duplicates = false;
    for (std::size_t i = 0; i < lip.frames.size(); ++i) {
        const auto& k = lip.frames[i];
        if (!std::isfinite(k.timeSeconds) || k.timeSeconds < 0)
            error("Invalid timestamp at key " + std::to_string(i) + ".");
        if (i > 0 && k.timeSeconds < lip.frames[i - 1].timeSeconds)
            error("Descending timestamps at key " + std::to_string(i) + "; saving never sorts them silently.");
        if (i > 0 && k.timeSeconds == lip.frames[i - 1].timeSeconds) duplicates = true;
        if (k.shape > 15) error("Pose ID outside 0..15 at key " + std::to_string(i) + ".");
    }
    if (duplicates) warn("Equal-time keys are preserved. Preview uses the last key at each shared time; game playback can be ambiguous.");
    return v;
}

std::vector<KotorKeyframe> readKotorKeyframes(const std::filesystem::path& path, double* durationSeconds) {
    const LipFile lip = readLip(path);
    if (durationSeconds) *durationSeconds = static_cast<double>(lip.durationSeconds);
    std::vector<KotorKeyframe> entries;
    entries.reserve(lip.frames.size());
    for (const auto& frame : lip.frames)
        entries.push_back({static_cast<double>(frame.timeSeconds), frame.shape});
    return entries;
}

LipFile lipFromKeyframes(const std::vector<KotorKeyframe>& entries, double durationSeconds) {
    LipFile lip;
    lip.durationSeconds = static_cast<float>(durationSeconds);
    lip.frames.reserve(entries.size());
    for (const auto& entry : entries) lip.frames.push_back({static_cast<float>(entry.timeSeconds), entry.shape});
    return lip;
}

void writeKotorKeyframes(const std::filesystem::path& path,
                        const std::vector<KotorKeyframe>& entries, double durationSeconds) {
    // No normalization, sorting, time nudges, hidden keys, duration inference,
    // or final-pose substitution. Imported float32 values round-trip exactly.
    writeLip(path, lipFromKeyframes(entries, durationSeconds));
}

void repairKotorKeyframes(std::vector<KotorKeyframe>& entries, double durationSeconds) {
    const float duration = static_cast<float>(durationSeconds);
    if (!(duration > 0) || !std::isfinite(duration))
        throw std::runtime_error("Set a positive finite clip duration before repairing keys.");
    const auto finalShape = entries.empty() ? std::uint8_t{0} : std::min<std::uint8_t>(entries.back().shape, 15);
    std::vector<KotorKeyframe> repaired;
    for (auto entry : entries) {
        if (!std::isfinite(entry.timeSeconds)) continue;
        entry.timeSeconds = static_cast<float>(std::clamp(entry.timeSeconds, 0.0, static_cast<double>(duration)));
        entry.shape = std::min<std::uint8_t>(entry.shape, 15);
        repaired.push_back(entry);
    }
    std::stable_sort(repaired.begin(), repaired.end(), [](const auto& a, const auto& b) { return a.timeSeconds < b.timeSeconds; });
    std::vector<KotorKeyframe> distinct;
    for (const auto& key : repaired) {
        if (!distinct.empty() && distinct.back().timeSeconds == key.timeSeconds) distinct.back() = key;
        else distinct.push_back(key);
    }
    if (distinct.empty() || distinct.front().timeSeconds > 0) distinct.insert(distinct.begin(), {0, 0});
    if (distinct.back().timeSeconds < duration) distinct.push_back({duration, finalShape});
    else distinct.back().shape = finalShape;
    entries = std::move(distinct);
}

LipPoseBlend sampleLipPose(const LipFile& lip, double timeSeconds) {
    if (!std::isfinite(timeSeconds)) throw std::runtime_error("Preview time must be finite.");
    const auto check = validateLipForKotorRuntime(lip);
    if (!check.ok) throw std::runtime_error("Cannot preview an invalid key curve: " + check.errors.front());
    LipPoseBlend result;
    const double time = std::clamp(timeSeconds, 0.0, static_cast<double>(lip.durationSeconds));
    result.completed = timeSeconds >= lip.durationSeconds;
    auto upper = std::upper_bound(lip.frames.begin(), lip.frames.end(), time,
        [](double t, const LipKeyframe& key) { return t < key.timeSeconds; });
    result.fromIndex = upper == lip.frames.begin() ? 0 : static_cast<std::size_t>(upper - lip.frames.begin() - 1);
    result.toIndex = upper == lip.frames.end() ? result.fromIndex : static_cast<std::size_t>(upper - lip.frames.begin());
    if (result.completed) result.fromIndex = result.toIndex = lip.frames.size() - 1;
    const auto& a = lip.frames[result.fromIndex];
    const auto& b = lip.frames[result.toIndex];
    result.fromShape = a.shape; result.toShape = b.shape;
    result.fromTalkPosition = (a.shape + 1.0) / 16.0;
    result.toTalkPosition = (b.shape + 1.0) / 16.0;
    if (result.fromIndex != result.toIndex)
        result.weight = std::clamp((time - a.timeSeconds) / (b.timeSeconds - a.timeSeconds), 0.0, 1.0);
    return result;
}

void simplifyLipHolds(LipFile& lip) {
    std::vector<LipKeyframe> compact;
    for (const auto& key : lip.frames) {
        if (compact.size() >= 2 && compact[compact.size() - 2].shape == key.shape &&
            compact.back().shape == key.shape && compact[compact.size() - 2].timeSeconds < compact.back().timeSeconds &&
            compact.back().timeSeconds < key.timeSeconds) compact.back() = key;
        else compact.push_back(key);
    }
    lip.frames = std::move(compact);
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

LipFile phnToLip(const PhnFile& phn, LipTailRestOptions tailRest, LipQualityOptions post) {
    auto spans = phonemeSpans(phn, post);
    const int audioMs = phn.intervals.empty() ? 125 : std::max(1, phn.intervals.back().endMs);
    const int tailMs = resolveTailRestMs(audioMs, tailRest);
    if (audioMs > std::numeric_limits<int>::max() - tailMs)
        throw std::runtime_error("LIP duration exceeds supported range.");
    const double duration = (audioMs + tailMs) / 1000.0;
    LipFile lip;
    lip.durationSeconds = static_cast<float>(duration);
    if (spans.empty()) spans.push_back({0, duration, 0});
    if (spans.front().start > 0 || spans.front().shape != 0)
        spans.insert(spans.begin(), {0, spans.front().start, 0});
    if (tailMs > 0) {
        if (spans.back().shape == 0) spans.back().end = duration;
        else spans.push_back({audioMs / 1000.0, duration, 0});
    }

    std::vector<double> arrivals(spans.size(), 0.0);
    for (std::size_t i = 1; i < spans.size(); ++i) {
        const auto& previous = spans[i - 1];
        const auto& current = spans[i];
        const double desired = current.start - anticipationMsForShape(current.shape, post) / 1000.0;
        // Any opt-in anticipation is bounded by the predecessor's midpoint and
        // target arrival. It can never reorder adjacent articulations.
        const double lower = std::max(arrivals[i - 1], (previous.start + previous.end) / 2.0);
        arrivals[i] = std::max(desired, lower);
        if (arrivals[i] <= arrivals[i - 1]) {
            const double available = current.end - arrivals[i - 1];
            arrivals[i] = arrivals[i - 1] + std::min(0.005, available / 2.0);
        }
        if (!(arrivals[i] < current.end)) throw std::runtime_error("No time remains for a mouth pose target.");
    }

    // These are authoring policies, not numeric constants from the
    // game. Both presets preserve every distinct pose and both hold endpoints.
    // Natural blends over at most 40ms; Minimal over at most 10ms. A transition
    // never consumes more than half the time between neighboring arrivals.
    const double transition = (post.maxBlendMs > 0 ? post.maxBlendMs : 10) / 1000.0;
    for (std::size_t i = 0; i < spans.size(); ++i) {
        appendGeneratedKey(lip, arrivals[i], spans[i].shape);
        if (i + 1 < spans.size()) {
            const double gap = arrivals[i + 1] - arrivals[i];
            const double holdEnd = arrivals[i + 1] - std::min(transition, gap / 2.0);
            appendGeneratedKey(lip, holdEnd, spans[i].shape);
        } else {
            // Preserve the final spoken pose when there is no trailing silence.
            appendGeneratedKey(lip, lip.durationSeconds, spans[i].shape);
        }
    }
    simplifyLipHolds(lip);
    const auto validation = validateLipForKotorRuntime(lip);
    if (!validation.ok) throw std::runtime_error("Generated invalid LIP: " + validation.errors.front());
    return lip;
}

void writePhnAsLip(const std::filesystem::path& path, const PhnFile& phn,
                   LipTailRestOptions tailRest, LipQualityOptions quality) {
    writeLip(path, phnToLip(phn, tailRest, quality));
}

} // namespace cslu
