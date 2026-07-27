#include "lip.hpp"
#include "phn.hpp"
#include "raw_frontend.hpp"
#include "wav.hpp"

#include <chrono>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <cstdlib>
#endif

namespace fs = std::filesystem;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void requireThrows(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

std::uint32_t readU32Le(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

void testUint32LipRecordCount(const fs::path& root) {
    std::vector<cslu::KotorVisibleKeyframe> keys;
    keys.reserve(300);
    for (std::size_t i = 0; i < 300; ++i) {
        keys.push_back({0.010 * static_cast<double>(i + 1U),
                        static_cast<std::uint8_t>((i % 15U) + 1U)});
    }
    keys.back().shape = 0;

    const fs::path path = root / "uint32-record-count.lip";
    cslu::writeKotorVisibleKeyframes(path, keys);

    std::ifstream in(path, std::ios::binary);
    const std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    require(bytes.size() >= 21U, "high-count LIP output is unexpectedly short");
    const std::uint32_t totalRecordCount = readU32Le(bytes.data() + 12);
    require(totalRecordCount == 301U,
            "LIP writer did not store the total record count as uint32");
    require(bytes.size() == 16U + 5U * static_cast<std::size_t>(totalRecordCount),
            "LIP size does not match the uint32 record count");
    require(readU32Le(bytes.data() + 16) == 0U && bytes[20] == 0U,
            "LIP writer did not store the explicit initial rest record");

    const cslu::LipFile runtime = cslu::readLip(path);
    require(runtime.frames.size() == 301U,
            "canonical LIP reader truncated a record count above 255");
    const auto visible = cslu::readKotorVisibleKeyframes(path);
    require(visible.size() == 300U,
            "editor-visible LIP reader truncated a record count above 255");
    require(std::fabs(visible.back().timeSeconds - 3.0) < 0.0001,
            "high-count LIP duration changed during round trip");
}

void testTransactionalLipWrites(const fs::path& root) {
    const std::vector<cslu::KotorVisibleKeyframe> keys = {
        {0.250, 3}, {0.750, 8}, {1.000, 0}};

    const fs::path directoryTarget = root / "directory-target.lip";
    fs::create_directory(directoryTarget);
    requireThrows([&] { cslu::writeKotorVisibleKeyframes(directoryTarget, keys); },
                  "LIP writer accepted a directory output target");
    require(fs::is_directory(directoryTarget),
            "failed LIP write damaged the destination directory");

#ifndef _WIN32
    const fs::path original = root / "symlink-original.lip";
    const std::string originalBytes = "preserve me";
    {
        std::ofstream out(original, std::ios::binary);
        out << originalBytes;
    }
    const fs::path link = root / "symlink-output.lip";
    std::error_code linkError;
    fs::create_symlink(original.filename(), link, linkError);
    if (!linkError) {
        requireThrows([&] { cslu::writeKotorVisibleKeyframes(link, keys); },
                      "LIP writer followed an output symlink");
        std::ifstream in(original, std::ios::binary);
        const std::string after((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        require(after == originalBytes, "failed LIP write changed a symlink target");
    }

    if (fs::exists("/dev/full")) {
        requireThrows([&] { cslu::writeKotorVisibleKeyframes("/dev/full", keys); },
                      "LIP writer reported success for /dev/full");
    }
#endif

    for (const auto& entry : fs::directory_iterator(root)) {
        require(entry.path().filename().string().rfind(".neolip-save-", 0) != 0,
                "LIP writer left a temporary save directory behind");
    }
}

void testDecoderArgumentsAreNotShellCommands(const fs::path& root) {
#ifndef _WIN32
    const fs::path marker = root / "neolip_shell_injection_marker";
    const fs::path malicious = root / "$(touch neolip_shell_injection_marker).mp3";
    {
        std::ofstream out(malicious, std::ios::binary);
        out.write("ID3", 3);
        out.write("not-a-real-mp3", 14);
    }

    const char* previousValue = std::getenv("CSLU_AUDIO_DECODER");
    const std::string previous = previousValue ? previousValue : "";
    const bool hadPrevious = previousValue != nullptr;
    const fs::path previousCwd = fs::current_path();
    try {
        fs::current_path(root);
        if (::setenv("CSLU_AUDIO_DECODER", "/bin/false", 1) != 0) {
            throw std::runtime_error("could not set decoder test environment");
        }
        requireThrows([&] { (void)cslu::readPcm16Mono(malicious); },
                      "invalid MP3 unexpectedly decoded");
        fs::current_path(previousCwd);
        if (hadPrevious) ::setenv("CSLU_AUDIO_DECODER", previous.c_str(), 1);
        else ::unsetenv("CSLU_AUDIO_DECODER");
    } catch (...) {
        fs::current_path(previousCwd);
        if (hadPrevious) ::setenv("CSLU_AUDIO_DECODER", previous.c_str(), 1);
        else ::unsetenv("CSLU_AUDIO_DECODER");
        throw;
    }
    require(!fs::exists(marker), "compressed-audio filename was interpreted by a shell");
#else
    (void)root;
#endif
}

} // namespace

int main() {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
                          ("neolip_core_io_test_" + std::to_string(stamp));
    try {
        fs::create_directories(root);

        std::vector<cslu::KotorVisibleKeyframe> keys = {
            {0.250, 3},
            {0.750, 8},
            {1.000, 0},
        };
        const fs::path lipPath = root / "roundtrip.lip";
        cslu::writeKotorVisibleKeyframes(lipPath, keys);
        double duration = 0.0;
        const auto readKeys = cslu::readKotorVisibleKeyframes(lipPath, &duration);
        require(readKeys.size() == keys.size(), "visible LIP key count changed during round trip");
        require(std::fabs(duration - 1.0) < 0.0001, "visible LIP duration changed during round trip");
        require(readKeys[1].shape == 8 && readKeys.back().shape == 0,
                "visible LIP shapes changed during round trip");
        require(fs::file_size(lipPath) == 16U + 5U * (keys.size() + 1U),
                "visible LIP writer did not use the canonical uint32-count layout");
        testUint32LipRecordCount(root);
        testTransactionalLipWrites(root);
        testDecoderArgumentsAreNotShellCommands(root);

        cslu::PhnFile phn;
        phn.intervals = {{0, 120, ".pau"}, {120, 360, "A"}};
        const fs::path phnPath = root / "roundtrip.phn";
        cslu::writePhn(phnPath, phn);
        const auto readPhn = cslu::readPhn(phnPath);
        require(readPhn.intervals.size() == 2 && readPhn.intervals[1].label == "A",
                "PHN core round trip failed");

        cslu::DecodedWaveform waveform;
        waveform.sampleRate = 8000;
        waveform.mono.resize(800);
        for (std::size_t i = 0; i < waveform.mono.size(); ++i) {
            waveform.mono[i] = static_cast<float>(0.5 * std::sin(
                2.0 * 3.14159265358979323846 * 220.0 * static_cast<double>(i) /
                waveform.sampleRate));
        }
        const fs::path wavPath = root / "segment.wav";
        cslu::writePcm16WaveformSegment(wavPath, waveform, 0.020, 0.050);
        const auto decoded = cslu::readMonoWaveform(wavPath);
        require(decoded.sampleRate == waveform.sampleRate, "WAV sample rate changed during round trip");
        require(decoded.mono.size() == 400, "WAV segment length is incorrect");
        require(std::fabs(decoded.mono[40] - waveform.mono[200]) < 0.001,
                "WAV segment samples changed beyond PCM16 tolerance");

        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return 1;
    }
}
