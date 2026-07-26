#pragma once

#include "phn.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cslu {

struct LipKeyframe {
    float timeSeconds = 0.0f;
    std::uint8_t shape = 0;
};

struct LipFile {
    float durationSeconds = 0.0f;
    std::vector<LipKeyframe> frames;
};

struct KotorStockKeyframe {
    std::uint32_t position = 0;
    std::uint8_t shape = 0;
};

struct KotorStockLip {
    std::uint32_t durationPosition = 0;
    std::vector<KotorStockKeyframe> entries;
};

// Editor-facing view of the visible stock LIP entries. The implicit runtime
// zero/rest frame and stock byte layout stay inside the core implementation.
struct KotorVisibleKeyframe {
    double timeSeconds = 0.0;
    std::uint8_t shape = 0;
};

struct LipValidation {
    bool ok = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

struct LipShapeInfo {
    std::uint8_t id = 0;
    const char* name = "";
    const char* phones = "";
};

struct LipTailRestOptions {
    bool automatic = false;
    int fixedMs = 0;
};

enum class LipSmoothingPreset {
    None,
    Light,
    Medium,
    Strong
};

struct LipQualityOptions {
    bool closeOnPause = true;
    int closeOnPauseMs = 160;
    LipSmoothingPreset smoothing = LipSmoothingPreset::Light;
    int minKeyMs = 40;
    bool preserveClosures = true;
    int anticipateClosuresMs = 40;
    int anticipateFricativesMs = 25;
    int anticipateStopsMs = 20;
};

int resolveTailRestMs(int phnDurationMs, const LipTailRestOptions& options);
LipSmoothingPreset parseLipSmoothingPreset(const std::string& value);
const char* lipSmoothingPresetName(LipSmoothingPreset preset);
void applyLipSmoothingPreset(LipQualityOptions& options, LipSmoothingPreset preset);

const std::vector<LipShapeInfo>& kotorLipShapes();
const char* kotorLipShapeName(std::uint8_t shape);

LipFile readLip(const std::filesystem::path& path);
void writeLipRuntimeLayout(const std::filesystem::path& path, const LipFile& lip);
LipValidation validateLipForKotorRuntime(const LipFile& lip, std::uintmax_t fileSize = 0);

std::uint32_t kotorStockTimeToPos(double seconds);
double kotorStockPosToTime(std::uint32_t position);
float kotorStockPosToFloat(std::uint32_t position);
std::uint32_t kotorStockFloatToPos(float f);

KotorStockLip readKotorStockLip(const std::filesystem::path& path);
void writeKotorStockLip(const std::filesystem::path& path, const KotorStockLip& lip);
LipFile kotorStockRuntimeView(const KotorStockLip& lip);
std::vector<KotorVisibleKeyframe> readKotorVisibleKeyframes(
    const std::filesystem::path& path, double* durationSeconds = nullptr);
void normalizeKotorVisibleKeyframes(std::vector<KotorVisibleKeyframe>& entries);
void writeKotorVisibleKeyframes(const std::filesystem::path& path,
                                std::vector<KotorVisibleKeyframe> entries);
KotorStockLip phnToKotorStockLip(const PhnFile& phn, LipTailRestOptions tailRest = {}, LipQualityOptions quality = {});

std::uint8_t csluPhoneToKotorShape(const std::string& phone);
std::int16_t csluPhoneToKotorShapeOrSkip(const std::string& phone);
void writePhnAsLip(const std::filesystem::path& path, const PhnFile& phn, LipTailRestOptions tailRest = {}, LipQualityOptions quality = {});

} // namespace cslu
