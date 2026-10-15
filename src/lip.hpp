#pragma once

#include "phn.hpp"
#include <neoshared/lip/LipFile.hpp>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace cslu {

using LipKeyframe = neoshared::lip::LipKeyframe;
using LipFile = neoshared::lip::LipFile;

// Editor view of ALL counted records, including the real key at time zero.
// The header duration is stored separately and must not be inferred on save.
struct KotorKeyframe {
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
    // Maximum blend window; never a rule for deleting nearby keys.
    // Every distinct mapped pose and both hold anchors are retained.
    int maxBlendMs = 40;
    int anticipateClosuresMs = 0;
    int anticipateFricativesMs = 0;
    int anticipateStopsMs = 0;
};

int resolveTailRestMs(int phnDurationMs, const LipTailRestOptions& options);
LipSmoothingPreset parseLipSmoothingPreset(const std::string& value);
const char* lipSmoothingPresetName(LipSmoothingPreset preset);
void applyLipSmoothingPreset(LipQualityOptions& options, LipSmoothingPreset preset);

const std::vector<LipShapeInfo>& kotorLipShapes();
const char* kotorLipShapeName(std::uint8_t shape);

LipFile readLip(const std::filesystem::path& path);
void writeLip(const std::filesystem::path& path, const LipFile& lip);
LipValidation validateLipForKotorRuntime(const LipFile& lip, std::uintmax_t fileSize = 0);

std::uint32_t kotorStockTimeToPos(double seconds);
double kotorStockPosToTime(std::uint32_t position);
float kotorStockPosToFloat(std::uint32_t position);
std::uint32_t kotorStockFloatToPos(float f);

std::vector<KotorKeyframe> readKotorKeyframes(
    const std::filesystem::path& path, double* durationSeconds = nullptr);
LipFile lipFromKeyframes(const std::vector<KotorKeyframe>& entries, double durationSeconds);
void writeKotorKeyframes(const std::filesystem::path& path,
                        const std::vector<KotorKeyframe>& entries, double durationSeconds);
// Explicit, user-confirmed repair only. Never called by ordinary saving.
// Keeps clip duration and final pose; same-time collisions keep the last key.
void repairKotorKeyframes(std::vector<KotorKeyframe>& entries, double durationSeconds);

struct LipPoseBlend {
    std::size_t fromIndex = 0, toIndex = 0;
    std::uint8_t fromShape = 0, toShape = 0;
    double weight = 0.0; // Weight of the second sampled pose, not an interpolated ID.
    double fromTalkPosition = 1.0 / 16.0, toTalkPosition = 1.0 / 16.0;
    bool completed = false;
};
// Recovered KotOR interior interpolation. Endpoint display is clamped; the
// engine's separate blend-out is not a new rest key. Duplicate timestamps use
// the last key at that instant (a defensive preview policy, not a claim about
// malformed-input playback in the game). Rejects descending/non-finite curves.
LipPoseBlend sampleLipPose(const LipFile& lip, double timeSeconds);
// Only removes the middle of three equal-pose keys. Both hold endpoints survive.
void simplifyLipHolds(LipFile& lip);
LipFile phnToLip(const PhnFile& phn, LipTailRestOptions tailRest = {},
                 LipQualityOptions quality = {});

std::uint8_t csluPhoneToKotorShape(const std::string& phone);
std::int16_t csluPhoneToKotorShapeOrSkip(const std::string& phone);
void writePhnAsLip(const std::filesystem::path& path, const PhnFile& phn, LipTailRestOptions tailRest = {}, LipQualityOptions quality = {});

} // namespace cslu
