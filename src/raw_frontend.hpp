#pragma once

#include "tsv.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace cslu {

struct Pcm16Mono {
    std::uint32_t sampleRate = 0;
    std::vector<double> samples; // 16-bit PCM units, not normalized to [-1,1]
};

Pcm16Mono readPcm16Mono(const std::filesystem::path& path);

// Reproduces logenergy.dll's Logenergy_compute for the FA path:
// squared samples, squared Hamming window, centered frames, denominator=sum(Hamming),
// floor at 1.0, output=10*log10(energy).
Matrix computeLogEnergy(const std::vector<double>& samples,
                        int sampleRate,
                        double winMs,
                        double frameMs,
                        std::size_t outputRows = 0);

// Reproduces intensity.dll's Intensity_normenergy for the FA path after prep !dc:
// 40ms logenergy every 5ms, normalized by min and the 10th-highest log-energy.
Matrix computeIntensityEnergyCol0(const std::vector<double>& samples,
                                  int sampleRate,
                                  double frameMs = 5.0,
                                  double smallWinMs = 40.0,
                                  std::size_t outputRows = 0,
                                  int robustTopRank = 10);

struct ValidatedEnergyFeatures {
    Matrix specEnergy24;
    Matrix specEnergyBigwin100;
    Matrix specEnergyNorm24;
    Matrix intensityEnergyCol0;
};

ValidatedEnergyFeatures computeValidatedEnergyFeatures16k(const Pcm16Mono& wav16k);

// Reproduces intensity.dll's Intensity_discrimination for the 16 kHz / 5 ms
// forced-alignment path after prep !dc. Output is the 640x16 feature matrix
// dumped as features/intensity_0_15.tsv in the Phase 2 traces.
Matrix computeIntensityDiscrimination16k(const std::vector<double>& samples,
                                         std::size_t outputRows = 0);

// Reconstructs burst.dll's Burst_compute path for the FA 8 kHz post-DC waveform.
// Output is the 8-column matrix dumped as features/burst_mx.tsv; column 7 is
// the burst-pulse stream used by the downstream feature combiner.
Matrix computeBurstMx8k(const std::vector<double>& samples,
                        std::size_t outputRows = 0);
Matrix computeBurstPulsesCol7(const Matrix& burstMx);


// Phase 6: recovered prep.dll DC-removal path.
Pcm16Mono applyPrepDcrmIir(const Pcm16Mono& wav, double tauMs = 300.0);
Matrix pcmSamplesColumn(const Pcm16Mono& wav);
void writePcm16Mono(const Pcm16Mono& wav, const std::filesystem::path& path);

// Phase 7: recovered feature.dll Bark/cepstral path for the CSLU FA setup.
// The post-processing geometry/CMS/energy-replacement/delta plumbing follows
// features_new_16k.tcl. The initial Bark cepstral extractor uses feature.dll's
// default finite support [center-2.5, center+1.3] Bark filterbank.
Matrix computeCsluDeltaOrder2(const Matrix& m);

struct PlpApproxFeatures {
    Matrix initFeat;
    Matrix featMfccPadded;
    Matrix featCms;
    Matrix featNoDelta;
    Matrix featDelta;
    Matrix featWithDelta;
    std::size_t cmsBegin = 0;
    std::size_t cmsEnd = 0;
    std::size_t minEnergyFrame = 0;
};

Matrix computeApproxFeatureInit16k(const std::vector<double>& samples,
                                   double frameMs = 5.0,
                                   double winMs = 24.0,
                                   int filters = 40,
                                   int outputs = 13);

PlpApproxFeatures computePlpApprox16k(const Pcm16Mono& wav16k,
                                      std::size_t outputRows = 0);

// Phase 6: exact Tcl-side PLP post-processing once feature.dll's per-frame
// init_feat matrix is available. This covers padding, silence-window CMS,
// normalized-energy replacement, delta computation, and column join.

// Phase 8: full raw acoustic frontend composition. These functions no longer
// consume traced feature component matrices; they build the complete acoustic
// evidence path from WAV data. For exact trace validation, pass CSLU's
// wave_16k_before_dc.wav and wave_8k_before_dc.wav into
// computeFullFrontendFromPreDcWavs(). computeFullFrontendFromOriginalWav()
// includes a portable linear resampler as a temporary replacement for
// wave.dll sampleconvert. Phase 13 replaces the temporary resampler with a
// disassembly-guided implementation of Wave_sampleConvert's FIR/polyphase path.
struct FullFrontendFeatures {
    Pcm16Mono wave16BeforeDc;
    Pcm16Mono wave8BeforeDc;
    Pcm16Mono wave16AfterDc;
    Pcm16Mono wave8AfterDc;

    PlpApproxFeatures plp;
    Matrix plpFeat16k;
    Matrix burstMx;
    Matrix burstPulsesCol7;
    Matrix intensity0To15;
    Matrix intensityEnergyCol0;
    Matrix allFeat;
    Matrix netin;
};

Pcm16Mono sampleConvertWaveDllCompatible(const Pcm16Mono& wav, std::uint32_t targetRate);
Pcm16Mono resamplePcm16MonoLinear(const Pcm16Mono& wav, std::uint32_t targetRate);
FullFrontendFeatures computeFullFrontendFromPreDcWavs(const Pcm16Mono& wave16PreDc,
                                                      const Pcm16Mono& wave8PreDc);
FullFrontendFeatures computeFullFrontendFromOriginalWav(const Pcm16Mono& original);
void writeFullFrontendFeatures(const FullFrontendFeatures& f,
                               const std::filesystem::path& outDir);

PlpApproxFeatures postprocessPlpFromInitAndEnergy(const Matrix& initFeat,
                                                  const Matrix& energyNorm,
                                                  const Matrix& energyBigwin,
                                                  std::size_t outputRows = 0);

} // namespace cslu
