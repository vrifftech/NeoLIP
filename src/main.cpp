#include "backend.hpp"
#include "feature_context.hpp"
#include "graph_build.hpp"
#include "lip.hpp"
#include "nnet45.hpp"
#include "phn.hpp"
#include "raw_frontend.hpp"
#include "tsv.hpp"
#include "text_ntp.hpp"
#include "wav.hpp"

#include <neotlk/TlkFile.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <limits.h>
#elif defined(__linux__)
#include <unistd.h>
#include <limits.h>
#endif

namespace fs = std::filesystem;

#ifndef CSLU_BUNDLED_DICTIONARY_DIR
#define CSLU_BUNDLED_DICTIONARY_DIR ""
#endif

#ifndef CSLU_SOURCE_DOCS_DIR
#define CSLU_SOURCE_DOCS_DIR ""
#endif

#ifndef CSLU_APP_VERSION
#define CSLU_APP_VERSION "0.3.7-neolip-qa-candidate"
#endif

#ifndef CSLU_APP_PHASE
#define CSLU_APP_PHASE "54"
#endif

using namespace cslu;

static void usage() {
    std::cerr <<
R"(neolip_backend - private NeoLIP GUI worker

This executable is an internal companion to neolip_gui. It is not a supported
user-facing command-line application.

Internal commands:
  version | --version | -V
      Print application version and release phase.

  validate-trace <dump_dir> [phn_file]
      Verify that a Phase 2 trace has the required files and print core dimensions.

  matrix-stats <dump_dir> <logical_matrix_name> [max_rows]
      Load a numeric dump matrix and print dimensions/statistics.
      Example logical names: features/all_feat, netin/manner, prob/phnProb

  inspect-model <model.45>
      Parse a CSLU .45 neural-net model and print its dimensions.

  compare-nnet <model.45> <input.tsv> <expected_output.tsv> [max_rows]
      Run .45 inference in C++ and compare against a CSLU nnet/*_orig_out.tsv dump.
      The implementation appends an implicit +1 bias when the input has 158 columns.

  parse-phn <file.phn>
      Print PHN interval count, duration, and first/last labels.

  lip-info <file.lip>
      Parse a KOTOR LIP file and report header, keyframe count, runtime checks, and warnings.

  lip-dump <file.lip> [output.tsv]
      Dump LIP keyframes as TSV: index, time_seconds, shape_id, shape_name, controller_value.

  lip-export-table <file.lip> <output.tsv>
      Export visible KOTOR mouth keys to an editable TSV table.

  lip-import-table <input.tsv> <output.lip>
      Import an editable TSV table and write a normalized stock KOTOR LIP file.

  lip-normalize <input.lip> <output.lip>
      Re-sort visible mouth keys and ensure a final shape-0 rest key.

  lip-shapes
      Print the 16 conventional KOTOR mouth-shape ids and the controller value used by the engine.

  phn-to-lip <input.phn> <output.lip> [lip quality flags]
      Convert CSLU-style PHN intervals to a stock KOTOR LIP file.
      Quality flags include --tail-rest-ms, --tail-rest auto, --close-on-pause-ms,
      --no-close-on-pause, --min-key-ms, --smoothing none|light|medium|strong,
      --preserve-closures, --no-preserve-closures, --anticipate-closures-ms,
      --anticipate-fricatives-ms, --anticipate-stops-ms, and --no-anticipation.

  align-wav-text-lip <input.wav> <input.txt-or-literal-text> <output.lip> --models <models_dir> [--work-dir dir] [lip quality flags] [--phn-output file.phn|--phn-out file.phn|--keep-phn] [text flags]
      Generate .ntp from text, align audio, and write a stock KOTOR .lip directly.
      A PHN intermediate is retained only when --phn-output/--phn-out/--keep-phn is supplied or when a work dir is used.

  align-wav-ntp-lip <input.wav> <input.ntp> <output.lip> --models <models_dir> [--work-dir dir] [lip quality flags] [--phn-output file.phn|--phn-out file.phn|--keep-phn]
      Align audio from an existing .ntp file and write a stock KOTOR .lip directly.

  wav-info <file.wav>
      Probe WAV/KOTOR audio metadata, chunk layout, decode class, and sample stats.

  audio-info <file.wav>
      Alias for wav-info.

  decode-audio-wav <input-audio.wav/mp3> <output_pcm16_mono.wav>
      Decode supported audio input to mono PCM16 WAV using the native parser or,
      for KOTOR MP3-backed stream WAVs, ffmpeg/CSLU_FFMPEG/CSLU_AUDIO_DECODER.

  compose-all-feat <dump_dir> <output.tsv>
      Rebuild features/all_feat.tsv from dumped component matrices:
      plp_feat_16k + burst_pulses_col7 + intensity_0_15 + intensity_energy_col0.

  compare-all-feat <dump_dir>
      Rebuild all_feat from dumped components and compare to features/all_feat.tsv.

  compose-netin <all_feat.tsv> <output.tsv>
      Rebuild the 158-column CSLU neural-net input matrix from all_feat.tsv.

  compare-netin <dump_dir> [manner|place|height|trans|all]
      Rebuild netin from features/all_feat.tsv and compare to dumped netin/*.tsv.
      All four CSLU PHN net inputs are identical for the 16 kHz path.

  compose-feature-stage <dump_dir> <out_dir>
      Write reconstructed features/all_feat.tsv and netin/{manner,place,height,trans}.tsv.

  compose-logenergy <16k_post_dc.wav> <output.tsv> <win_ms> [rows]
      Compute CSLU-compatible logenergy.dll output for a processed 16 kHz WAV.
      Examples: win_ms=24 for spec energy, win_ms=100 for big-window energy.

  compare-logenergy-trace <dump_dir>
      Compare C++ logenergy outputs against dumped spec energy, big-window energy,
      normalized spec energy, and intensity_energy_col0.

  compose-intensity-energy <16k_post_dc.wav> <output.tsv> [rows]
      Reproduce CSLU features/intensity_energy_col0.tsv from wave_16k_after_dc.wav.

  compare-intensity-energy-trace <dump_dir>
      Convenience wrapper using dump/audio/wave_16k_after_dc.wav and
      dump/features/intensity_energy_col0.tsv.

  compose-intensity-discrimination <16k_post_dc.wav> <output.tsv> [rows]
      Reproduce CSLU features/intensity_0_15.tsv from wave_16k_after_dc.wav.

  compare-intensity-discrimination-trace <dump_dir>
      Compare C++ Intensity_discrimination against dump/features/intensity_0_15.tsv.

  compose-burst <8k_post_dc.wav> <burst_mx.tsv> <burst_pulses_col7.tsv> [rows]
      Reconstruct burst.dll Burst_compute for the 8 kHz FA branch and write
      the full 8-column burst_mx matrix plus the column-7 pulse stream.

  compare-burst-trace <dump_dir>
      Compare C++ burst_mx and burst_pulses_col7 against the Phase 2 trace.


  compose-dc <pre_dc.wav> <output_samples.tsv>
      Apply the recovered prep.dll dcrm DC-removal filter and write samples as one-column TSV.

  compose-dc-wav <pre_dc.wav> <post_dc.wav>
      Apply the recovered prep.dll dcrm DC-removal filter and write a PCM16 WAV.

  compare-dc-trace <dump_dir>
      Compare recovered prep !dc against audio/wave_{8k,16k}_{before,after}_dc.wav.

  compose-plp-frontend <16k_post_dc.wav> <out_dir> [rows]
      Write the recovered feature.dll Bark/cepstral front end plus CSLU PLP post-processing.

  compose-plp-approx <16k_post_dc.wav> <out_dir> [rows]
      Backward-compatible alias for compose-plp-frontend.

  compare-plp-frontend-trace <dump_dir>
      Compare the recovered PLP/cepstral front end against dumped feature.dll outputs.

  compare-plp-approx-trace <dump_dir>
      Backward-compatible alias for compare-plp-frontend-trace.

  compare-delta-trace <dump_dir>
      Validate recovered feature delta against features/spec_16000_24.0/feat_delta.tsv.

  compare-plp-post-trace <dump_dir>
      Validate exact Tcl-side PLP post-processing from dumped init_feat + energy matrices.

  compose-plp-post-trace <dump_dir> <out_dir>
      Write exact post-processed PLP matrices using dumped init_feat + energy inputs.


  compose-full-frontend <16k_pre_dc.wav> <8k_pre_dc.wav> <out_dir> [models_root]
      Run the integrated C++ frontend from raw/pre-DC 16 kHz and 8 kHz WAVs.
      Writes post-DC audio, PLP, burst, intensity, all_feat, netin, and optionally six nnet outputs.

  compose-full-frontend-from-wav <input.wav> <out_dir> [models_root]
      Run the integrated frontend from one input WAV using the recovered wave.dll
      Wave_sampleConvert-compatible resampler.

  compare-sampleconvert-trace <dump_dir>
      Starting from audio/input_original_wave.wav, reproduce wave.dll sampleconvert
      to 16 kHz and 8 kHz and compare against audio/wave_*_before_dc.wav.

  compare-full-frontend-trace <dump_dir> [models_root]
      Validate the integrated frontend by starting only from the trace pre-DC WAVs and
      comparing generated matrices/nnet outputs to the CSLU trace.


  text-to-ntp <input.txt-or-literal-text> <output.ntp> --models <models_dir> [text flags]
      Generate an .ntp phone transcript from text. The default mode is simple.
      Useful text flags: --mode simple|cslu, --dict built_in_or_custom.txt,
      --override-dict overrides.txt, --use-ogi, --no-ogi, --ogi-lex ogi_lex.out,
      --ogi-addenda ogi_lex_addenda.scm, --source-report report.tsv,
      --unknown-report unknown.tsv. OGI/Festival lexicon fallback is enabled by default
      when the files are present. TLK cleanup is enabled by default; add
      --no-tlk-cleanup to preserve bracket/brace markup, or --keep-stage-directions
      to preserve text inside {...} while still removing the braces.

  lookup-word <word> --models <models_dir> [text flags]
      Show the pronunciation and dictionary source selected for one word.

  compare-text-to-ntp-test <fa_root> [out_dir]
      Generate NTP from fa_root/test.txt, compare --mode cslu against fa_root/test_automatic.ntp
      after whitespace normalization, and optionally write generated simple/cslu NTP files.

  align-wav-text <input.wav> <input.txt-or-literal-text> <output.phn> --models <models_dir> [--work-dir dir] [text flags]
      Generate .ntp from text, then run the end-to-end WAV+NTP -> PHN path.
      simple mode is deterministic; cslu mode preserves alternatives and optional pauses.
      OGI/Festival dictionary fallback and override dictionaries are supported.

  batch-align <input_dir> <output_dir> --models <models_dir> [batch/text flags]
      Add --output phn|lip|both to choose whether batch writes .phn, .lip, or both.
      Add --tail-rest-ms ms or --tail-rest auto to extend generated .lip final rest/duration.
      Batch process a folder of WAV/TXT or WAV/NTP pairs. By default, each .wav
      is paired with a same-stem .txt or .lab transcript and produces .ntp/.phn.
      Batch flags: --input text|ntp|auto, --recursive, --no-workdirs, --fail-fast,
      --jobs N|auto, --resume, --decode-cache-dir dir, --cancel-file file,
      --batch-report file.tsv, --unknown-report file.tsv. Text flags such as
      --mode simple|cslu, --override-dict path, --use-ogi, and --no-ogi are supported.

  scan-unknowns <txt-file-or-dir> <unknown_report.tsv> --models <models_dir> [text/scan flags]
      Scan one transcript file or a folder of .txt/.lab files and write a TSV of
      unknown words plus ready-to-edit dictionary stubs. Scan flags: --recursive,
      --source-report file.tsv. Text flags such as --override-dict, --use-ogi, and
      --no-ogi are supported.

  scan-tlk-unknowns <tlk-file-or-dir> <output_dir> --models <models_dir> [text/scan flags]
      Parse KOTOR TLK V3.0 files directly and write combined/per-TLK unknown-word
      reports, instance reports, stubs, and a markdown scan summary. Scan flags:
      --recursive, --source-report file.tsv. Uses the same dictionary, cleanup,
      apostrophe, roman-numeral, HK-47/T3-M4, and compositional lookup path as
      text-to-ntp.

  append-dict-stubs <unknown_report.tsv> <override_dictionary.dict>
      Append missing dictionary stub lines from a scan-unknowns or batch-align
      unknown_words.tsv report to an override dictionary. Existing entries are skipped.

  align-wav-ntp <input.wav> <input.ntp> <output.phn> --models <models_dir> [--work-dir dir]
      Run the merged Phase 8 + Phase 11 pipeline from one WAV and one .ntp file.
      Uses the recovered wave.dll Wave_sampleConvert-compatible resampler for
      16 kHz/8 kHz paths. Writes CSLU-style .phn.

  align-predc-ntp <16k_pre_dc.wav> <8k_pre_dc.wav> <input.ntp> <output.phn> --models <models_dir> [--work-dir dir]
      Run the same merged pipeline starting from already sample-converted pre-DC 16 kHz
      and 8 kHz WAVs. This is the exact trace-parity entry point.

  compare-end-to-end-trace <dump_dir> <fa_root> <output.phn> [work_dir]
      Start from the Phase 2 trace's pre-DC WAVs and .ntp, run the full C++ frontend,
      neural nets, backend probabilities, graph builder, Viterbi, and PHN writer, then
      compare the resulting .phn to the traced CSLU output.

  raw-front-end-status
      Print validated versus remaining raw acoustic front-end pieces.


  compare-garbage-median-trace <dump_dir>
      Reproduce garbage.dll median -N 20 for all six nnet orig-output matrices
      and compare against dumped nnet/*_median_out.tsv matrices.

  compose-garbage-median <orig_output.tsv> <out.tsv> [N]
      Append the CSLU garbage median column to one nnet orig-output matrix.
      Default N is 20, matching fa_new_16k.tcl.

  compare-phnfeat-trace <dump_dir>
      Reproduce phnfeat.dll's phn and trans probability combination from the
      dumped median nnet matrices and mapping lists. Compares to prob/*.tsv.

  compose-backend-prob-from-trace <dump_dir> <out_dir> [orig|median]
      Write prob/phnProb.tsv and prob/transProb.tsv using the recovered C++
      garbage-median + phnfeat path. The default input mode is orig.

  inspect-backend-graph <dump_dir>
      Parse the traced statenet graph dumps and print node/arc/category counts.

  write-phn-from-trace-answer <dump_dir> <output.phn>
      Rebuild a .phn file from the traced viterbi/phn_labels_after_undo dump.
      This validates the PHN writer and Tcl-list parsing boundary.

  simple-viterbi-trace <dump_dir> <output.phn>
      Backward-compatible alias for align-viterbi-trace.

  align-viterbi-trace <dump_dir> <output.phn>
      Run the recovered CSLU FA Viterbi path from traced graph + prob/*.tsv and write .phn.

  compare-viterbi-trace <dump_dir> <output.phn>
      Run Viterbi from traced graph/probabilities, write .phn, and compare to traced PHN labels.

  align-viterbi-backend <backend_dir> <output.phn>
      Run Viterbi from a generated backend folder containing graph/*.list.tsv and prob/*.tsv.

  align-backend-from-ntp-trace <dump_dir> <fa_root> <output.phn> [orig|median]
      Build graph/mappings from the trace NTP, compose probabilities from trace nnet outputs,
      run Viterbi, and write .phn without consuming traced graph/mapping/prob matrices.

  compose-graph-from-ntp <ntp_file> <fa_root> <out_dir>
      Build the CSLU FA state graph and mapping lists from .ntp. Phase 15 supports
      optional brackets and parenthesized/top-level alternatives.

  compare-graph-from-ntp-trace <dump_dir> <fa_root>
      Build graph+mapping lists from dump/text/ntp_original.txt and compare semantically
      against traced statenet/mapping/probability outputs.

  compose-backend-from-ntp-trace <dump_dir> <fa_root> <out_dir> [orig|median]
      Build graph+mapping lists from the trace NTP, use trace nnet outputs, and write
      generated graph/mapping/prob/*.tsv without consuming traced mapping lists.

  bundle-assets | package-assets <source_cslu_or_apps_fa> <package_dir> [--backend neolip_backend.exe] [--gui neolip_gui.exe] [--kotor-dict dictionaries/kotor_override.dict]
      Copy only the required CSLU FA models and dictionaries into a standalone package layout.

  check-bundle | validate-package <package_dir_or_models_dir>
      Check that a standalone package has the required model and dictionary assets.

Notes:
  Standalone package commands can use --models <package/models>; legacy commands can still use Toolkit/2.0/apps/fa.
  Use bundle-assets/package-assets to copy the minimal model/dictionary assets into a redistributable layout.
)";
}

static void printMatrixCompare(const std::string& label, const MatrixCompareStats& s,
                               double meanThreshold = 1e-6, double maxThreshold = 1e-4) {
    std::cout << std::setprecision(17);
    std::cout << "matrix\t" << label << "\n";
    std::cout << "rows\t" << s.rows << "\n";
    std::cout << "cols\t" << s.cols << "\n";
    std::cout << "mean_abs_error\t" << s.meanAbsError << "\n";
    std::cout << "max_abs_error\t" << s.maxAbsError << "\n";
    std::cout << "max_error_row\t" << s.maxRow << "\n";
    std::cout << "max_error_col\t" << s.maxCol << "\n";
    std::cout << "verdict\t" << ((s.meanAbsError <= meanThreshold && s.maxAbsError <= maxThreshold) ? "PASS" : "CHECK") << "\n";
}

static Matrix readDumpMatrix(const fs::path& dump, const std::string& logical) {
    return readNumericTsv(tracePath(dump, logical));
}

static Matrix composeAllFeatFromDump(const fs::path& dump) {
    const Matrix plp = readDumpMatrix(dump, "features/plp_feat_16k");
    const Matrix burst = readDumpMatrix(dump, "features/burst_pulses_col7");
    const Matrix inten = readDumpMatrix(dump, "features/intensity_0_15");
    const Matrix energy = readDumpMatrix(dump, "features/intensity_energy_col0");
    return composeAllFeat(plp, burst, inten, energy);
}

static bool phnFilesEqual(const PhnFile& a, const PhnFile& b, std::string* reason = nullptr) {
    if (a.intervals.size() != b.intervals.size()) {
        if (reason) *reason = "interval count differs";
        return false;
    }
    for (std::size_t i = 0; i < a.intervals.size(); ++i) {
        const auto& x = a.intervals[i];
        const auto& y = b.intervals[i];
        if (x.startMs != y.startMs || x.endMs != y.endMs || x.label != y.label) {
            if (reason) {
                std::ostringstream ss;
                ss << "interval " << i << " differs: got "
                   << x.startMs << ' ' << x.endMs << ' ' << x.label
                   << " expected " << y.startMs << ' ' << y.endMs << ' ' << y.label;
                *reason = ss.str();
            }
            return false;
        }
    }
    return true;
}

static void printPhnSummary(const PhnFile& phn) {
    std::cout << "intervals\t" << phn.intervals.size() << "\n";
    if (!phn.intervals.empty()) {
        std::cout << "duration_ms\t" << phn.intervals.back().endMs << "\n";
        std::cout << "first\t" << phn.intervals.front().startMs << '\t' << phn.intervals.front().endMs << '\t' << phn.intervals.front().label << "\n";
        std::cout << "last\t" << phn.intervals.back().startMs << '\t' << phn.intervals.back().endMs << '\t' << phn.intervals.back().label << "\n";
    }
}

static ViterbiOptions readViterbiOptionsFromTrace(const fs::path& dump) {
    ViterbiOptions options;
    const fs::path paramsPath = dump / "viterbi" / "params.tsv";
    if (!fs::exists(paramsPath)) {
        return options;
    }
    const auto kv = readKeyValueTsv(paramsPath);
    auto setDouble = [&](const std::string& key, double& dst) {
        auto it = kv.find(key);
        if (it != kv.end() && !it->second.empty()) {
            dst = std::stod(it->second);
        }
    };
    setDouble("transProbFactor", options.transProbFactor);
    setDouble("durationShortPenalty", options.durationShortPenalty);
    setDouble("durationLongPenalty", options.durationLongPenalty);
    return options;
}

static void printViterbiOptions(const ViterbiOptions& options) {
    std::cout << "frame_ms\t" << options.frameMs << "\n";
    std::cout << "duration_model\t" << options.durationModel << "\n";
    std::cout << "transProbFactor\t" << options.transProbFactor << "\n";
    std::cout << "durationShortPenalty\t" << options.durationShortPenalty << "\n";
    std::cout << "durationLongPenalty\t" << options.durationLongPenalty << "\n";
}

static int cmdValidateTrace(const std::vector<std::string>& args) {
    if (args.size() < 2 || args.size() > 3) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const fs::path phnPath = (args.size() >= 3) ? fs::path(args[2]) : (dump / "output" / "phn.txt");

    std::vector<std::string> required = {
        "audio/wave_info.tsv",
        "features/plp_feat_16k.meta.tsv",
        "features/burst_mx.meta.tsv",
        "features/burst_pulses_col7.meta.tsv",
        "features/intensity_discrimination_raw.meta.tsv",
        "features/intensity_0_15.meta.tsv",
        "features/intensity_energy_col0.meta.tsv",
        "features/all_feat.meta.tsv",
        "netin/manner.meta.tsv",
        "netin/place.meta.tsv",
        "netin/height.meta.tsv",
        "netin/trans.meta.tsv",
        "nnet/bc_m_orig_out.meta.tsv",
        "nnet/bc_p_orig_out.meta.tsv",
        "nnet/bc_h_orig_out.meta.tsv",
        "nnet/trans_m_orig_out.meta.tsv",
        "nnet/trans_p_orig_out.meta.tsv",
        "nnet/trans_h_orig_out.meta.tsv",
        "prob/phnProb.meta.tsv",
        "prob/transProb.meta.tsv",
        "viterbi/answer_raw.txt",
        "output/phn.txt"
    };
    for (const auto& r : required) {
        requireFile(dump / r, r);
    }

    auto wave = readKeyValueTsv(dump / "audio" / "wave_info.tsv");
    std::cout << "trace_dir\t" << dump.string() << "\n";
    std::cout << "wave_orig_rate\t" << wave["orig_rate"] << "\n";
    std::cout << "wave_8k_rate\t" << wave["wave_8k_rate"] << "\n";
    std::cout << "wave_16k_rate\t" << wave["wave_16k_rate"] << "\n";

    std::vector<std::string> metas = {
        "features/plp_feat_16k",
        "features/burst_mx",
        "features/burst_pulses_col7",
        "features/intensity_discrimination_raw",
        "features/intensity_normenergy_raw",
        "features/intensity_0_15",
        "features/intensity_energy_col0",
        "features/all_feat",
        "netin/manner",
        "netin/place",
        "netin/height",
        "netin/trans",
        "nnet/bc_m_orig_out",
        "nnet/bc_p_orig_out",
        "nnet/bc_h_orig_out",
        "nnet/trans_m_orig_out",
        "nnet/trans_p_orig_out",
        "nnet/trans_h_orig_out",
        "prob/phnProb",
        "prob/transProb"
    };

    std::cout << "\nname\trows\tcols\tdumped_rows\tdumped_cols\ttruncated\n";
    for (const auto& name : metas) {
        auto info = readMatrixMeta(traceMetaPath(dump, name));
        std::cout << name << '\t'
                  << info.rows << '\t' << info.cols << '\t'
                  << info.dumped_rows << '\t' << info.dumped_cols << '\t'
                  << (info.truncated_rows || info.truncated_cols ? "yes" : "no") << "\n";
    }

    auto phn = readPhn(phnPath);
    int duration = phn.intervals.empty() ? 0 : phn.intervals.back().endMs;
    std::cout << "\nphn_file\t" << phnPath.string() << "\n";
    std::cout << "phn_intervals\t" << phn.intervals.size() << "\n";
    std::cout << "phn_duration_ms\t" << duration << "\n";
    if (!phn.intervals.empty()) {
        std::cout << "phn_first\t" << phn.intervals.front().label << "\n";
        std::cout << "phn_last\t" << phn.intervals.back().label << "\n";
    }
    return 0;
}

static int cmdMatrixStats(const std::vector<std::string>& args) {
    if (args.size() < 3 || args.size() > 4) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const std::string logical = args[2];
    std::optional<std::size_t> maxRows;
    if (args.size() == 4) {
        maxRows = static_cast<std::size_t>(std::stoull(args[3]));
    }

    auto path = tracePath(dump, logical);
    auto m = readNumericTsv(path, maxRows);
    auto s = stats(m);
    std::cout << "matrix\t" << logical << "\n";
    std::cout << "path\t" << path.string() << "\n";
    std::cout << "rows\t" << s.rows << "\n";
    std::cout << "cols\t" << s.cols << "\n";
    std::cout << std::setprecision(17);
    std::cout << "min\t" << s.min << "\n";
    std::cout << "max\t" << s.max << "\n";
    std::cout << "mean\t" << s.mean << "\n";
    if (m.rows > 0) {
        std::cout << "first_row_first_values";
        const std::size_t n = std::min<std::size_t>(m.cols, 8);
        for (std::size_t i = 0; i < n; ++i) {
            std::cout << '\t' << m(0, i);
        }
        std::cout << "\n";
    }
    return 0;
}

static int cmdInspectModel(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path model = args[1];
    auto net = NNet45::load(model);
    std::uintmax_t fileSize = fs::file_size(model);
    std::cout << "model\t" << model.string() << "\n";
    std::cout << "file_size\t" << fileSize << "\n";
    std::cout << "version\t" << net.version << "\n";
    std::cout << "layers\t" << net.layers << "\n";
    std::cout << "inputs\t" << net.inputCount << "\n";
    std::cout << "hidden\t" << net.hiddenCount << "\n";
    std::cout << "outputs\t" << net.outputCount << "\n";
    std::cout << "float_count\t" << net.expectedFloatCount() << "\n";
    std::cout << "layout\tW1 hidden-major HxI, W2 output-major OxH\n";
    std::cout << "activation\tsigmoid hidden, sigmoid output\n";
    return 0;
}

static int cmdCompareNNet(const std::vector<std::string>& args) {
    if (args.size() < 4 || args.size() > 5) {
        usage();
        return 2;
    }
    const fs::path model = args[1];
    const fs::path inputPath = args[2];
    const fs::path expectedPath = args[3];
    std::optional<std::size_t> maxRows;
    if (args.size() == 5) {
        maxRows = static_cast<std::size_t>(std::stoull(args[4]));
    }

    auto net = NNet45::load(model);
    auto input = readNumericTsv(inputPath, maxRows);
    auto expected = readNumericTsv(expectedPath, maxRows);
    auto actual = net.forwardMatrix(input);
    auto c = compareMatrices(actual, expected);

    std::cout << std::setprecision(17);
    std::cout << "model\t" << model.string() << "\n";
    std::cout << "input\t" << inputPath.string() << "\n";
    std::cout << "expected\t" << expectedPath.string() << "\n";
    std::cout << "rows\t" << c.rows << "\n";
    std::cout << "cols\t" << c.cols << "\n";
    std::cout << "mean_abs_error\t" << c.meanAbsError << "\n";
    std::cout << "max_abs_error\t" << c.maxAbsError << "\n";
    std::cout << "max_error_row\t" << c.maxRow << "\n";
    std::cout << "max_error_col\t" << c.maxCol << "\n";
    std::cout << "top1_matches\t" << c.top1Matches << "\n";
    std::cout << "top1_total\t" << c.rows << "\n";
    if (c.meanAbsError < 1e-6 && c.maxAbsError < 1e-4) {
        std::cout << "verdict\tPASS\n";
    } else {
        std::cout << "verdict\tCHECK\n";
    }
    return 0;
}

static int cmdParsePhn(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path path = args[1];
    auto phn = readPhn(path);
    std::cout << "file\t" << path.string() << "\n";
    std::cout << "milliseconds_per_frame\t" << phn.millisecondsPerFrame << "\n";
    std::cout << "intervals\t" << phn.intervals.size() << "\n";
    if (!phn.intervals.empty()) {
        std::cout << "duration_ms\t" << phn.intervals.back().endMs << "\n";
        std::cout << "first\t" << phn.intervals.front().startMs << '\t' << phn.intervals.front().endMs << '\t' << phn.intervals.front().label << "\n";
        std::cout << "last\t" << phn.intervals.back().startMs << '\t' << phn.intervals.back().endMs << '\t' << phn.intervals.back().label << "\n";
    }
    return 0;
}


static void writeLipDumpRows(std::ostream& out, const LipFile& lip) {
    out << "index\tengine_time_float\tkotor_pos_hex\tdecoded_kotor_time_seconds\tnormalized_engine_time\tshape_id\tshape_name\tcontroller_value\n";
    out << std::setprecision(9);
    for (std::size_t i = 0; i < lip.frames.size(); ++i) {
        const auto& k = lip.frames[i];
        const std::uint32_t pos = kotorStockFloatToPos(k.timeSeconds);
        const double decoded = (i == 0 && k.timeSeconds == 0.0f) ? 0.0 : kotorStockPosToTime(pos);
        const double norm = (lip.durationSeconds != 0.0f) ? (static_cast<double>(k.timeSeconds) / static_cast<double>(lip.durationSeconds)) : 0.0;
        std::ostringstream hx;
        hx << "0x" << std::hex << std::setw(8) << std::setfill('0') << pos;
        out << i << '\t'
            << k.timeSeconds << '\t'
            << hx.str() << '\t'
            << decoded << '\t'
            << norm << '\t'
            << static_cast<unsigned>(k.shape) << '\t'
            << kotorLipShapeName(k.shape) << '\t'
            << ((static_cast<double>(k.shape) + 1.0) / 16.0) << "\n";
    }
}

static int cmdLipShapes(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        usage();
        return 2;
    }
    std::cout << "id\tname\tcontroller_value\tphones\n";
    std::cout << std::setprecision(9);
    for (const auto& s : kotorLipShapes()) {
        std::cout << static_cast<unsigned>(s.id) << '\t'
                  << s.name << '\t'
                  << ((static_cast<double>(s.id) + 1.0) / 16.0) << '\t'
                  << s.phones << "\n";
    }
    return 0;
}

static int cmdLipInfo(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path path = args[1];
    const auto lip = readLip(path);
    const auto size = fs::file_size(path);
    const auto v = validateLipForKotorRuntime(lip, size);
    std::cout << std::setprecision(9);
    std::cout << "file\t" << path.string() << "\n";
    std::cout << "magic\tLIP V1.0\n";
    std::cout << "file_size\t" << size << "\n";
    std::cout << "expected_size\t" << (16 + 5 * lip.frames.size()) << "\n";
    std::cout << "engine_duration_float\t" << lip.durationSeconds << "\n";
    std::cout << "engine_duration_pos_hex\t0x" << std::hex << std::setw(8) << std::setfill('0') << kotorStockFloatToPos(lip.durationSeconds) << std::dec << std::setfill(' ') << "\n";
    std::cout << "decoded_kotor_duration_seconds\t" << kotorStockPosToTime(kotorStockFloatToPos(lip.durationSeconds)) << "\n";
    std::cout << "entry_count\t" << lip.frames.size() << "\n";
    if (!lip.frames.empty()) {
        const auto& first = lip.frames.front();
        const auto& last = lip.frames.back();
        std::cout << "first\t" << first.timeSeconds << '\t' << static_cast<unsigned>(first.shape) << '\t' << kotorLipShapeName(first.shape) << "\n";
        std::cout << "last\t" << last.timeSeconds << '\t' << static_cast<unsigned>(last.shape) << '\t' << kotorLipShapeName(last.shape) << "\n";
        std::cout << "first_decoded_kotor_time\t" << ((first.timeSeconds == 0.0f) ? 0.0 : kotorStockPosToTime(kotorStockFloatToPos(first.timeSeconds))) << "\n";
        std::cout << "last_decoded_kotor_time\t" << kotorStockPosToTime(kotorStockFloatToPos(last.timeSeconds)) << "\n";
        std::cout << "first_normalized\t" << (first.timeSeconds / lip.durationSeconds) << "\n";
        std::cout << "last_normalized\t" << (last.timeSeconds / lip.durationSeconds) << "\n";
    }
    std::cout << "runtime_verdict\t" << (v.ok ? "PASS" : "FAIL") << "\n";
    for (const auto& e : v.errors) std::cout << "error\t" << e << "\n";
    for (const auto& w : v.warnings) std::cout << "warning\t" << w << "\n";
    return v.ok ? 0 : 1;
}

static int cmdLipDump(const std::vector<std::string>& args) {
    if (args.size() != 2 && args.size() != 3) {
        usage();
        return 2;
    }
    const auto lip = readLip(args[1]);
    if (args.size() == 3) {
        std::ofstream out(args[2], std::ios::binary);
        if (!out) throw std::runtime_error("cannot write LIP dump: " + args[2]);
        writeLipDumpRows(out, lip);
        std::cout << "wrote\t" << args[2] << "\n";
    } else {
        writeLipDumpRows(std::cout, lip);
    }
    return 0;
}


static std::string shapeNameForTsv(std::uint8_t shape) {
    return kotorLipShapeName(shape);
}

static void normalizeStockLip(KotorStockLip& lip) {
    std::sort(lip.entries.begin(), lip.entries.end(), [](const KotorStockKeyframe& a, const KotorStockKeyframe& b) {
        return a.position < b.position;
    });
    std::vector<KotorStockKeyframe> unique;
    for (const auto& e : lip.entries) {
        KotorStockKeyframe k = e;
        if (k.shape > 15) k.shape = 15;
        if (!unique.empty() && unique.back().position == k.position) {
            unique.back() = k;
        } else {
            unique.push_back(k);
        }
    }
    lip.entries = std::move(unique);
    if (lip.entries.empty()) lip.entries.push_back({kotorStockTimeToPos(0.125), 0});
    lip.entries.back().shape = 0;
    lip.durationPosition = lip.entries.back().position;
}

static int cmdLipExportTable(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const fs::path inPath = args[1];
    const fs::path outPath = args[2];
    KotorStockLip lip = readKotorStockLip(inPath);
    std::ofstream out(outPath, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write LIP table: " + outPath.string());
    out << "index\ttime_seconds\tposition_hex\tshape_id\tshape_name\n";
    out << std::setprecision(9);
    for (std::size_t i = 0; i < lip.entries.size(); ++i) {
        const auto& e = lip.entries[i];
        out << i << '\t'
            << kotorStockPosToTime(e.position) << '\t'
            << "0x" << std::hex << std::setw(8) << std::setfill('0') << e.position << std::dec << std::setfill(' ') << '\t'
            << static_cast<unsigned>(e.shape) << '\t'
            << shapeNameForTsv(e.shape) << "\n";
    }
    std::cout << "input_lip\t" << inPath.string() << "\n";
    std::cout << "output_tsv\t" << outPath.string() << "\n";
    std::cout << "visible_keys\t" << lip.entries.size() << "\n";
    return 0;
}

static int cmdLipImportTable(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const fs::path inPath = args[1];
    const fs::path outPath = args[2];
    std::ifstream in(inPath);
    if (!in) throw std::runtime_error("cannot read LIP table: " + inPath.string());
    KotorStockLip lip;
    std::string line;
    std::size_t row = 0;
    while (std::getline(in, line)) {
        ++row;
        if (line.empty()) continue;
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, '\t')) cols.push_back(cell);
        if (cols.empty()) continue;
        if (row == 1 && cols[0] == "index") continue;
        if (cols.size() < 4) throw std::runtime_error("LIP table row has fewer than 4 columns at line " + std::to_string(row));
        const double t = std::stod(cols[1]);
        int shape = std::stoi(cols[3]);
        if (shape < 0) shape = 0;
        if (shape > 15) shape = 15;
        lip.entries.push_back({kotorStockTimeToPos(std::max(0.0, t)), static_cast<std::uint8_t>(shape)});
    }
    normalizeStockLip(lip);
    if (!outPath.parent_path().empty()) ensureDirectory(outPath.parent_path());
    writeKotorStockLip(outPath, lip);
    std::cout << "input_tsv\t" << inPath.string() << "\n";
    std::cout << "output_lip\t" << outPath.string() << "\n";
    std::cout << "visible_keys\t" << lip.entries.size() << "\n";
    std::cout << "duration_seconds\t" << std::setprecision(9) << kotorStockPosToTime(lip.durationPosition) << "\n";
    return 0;
}

static int cmdLipNormalize(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const fs::path inPath = args[1];
    const fs::path outPath = args[2];
    KotorStockLip lip = readKotorStockLip(inPath);
    normalizeStockLip(lip);
    if (!outPath.parent_path().empty()) ensureDirectory(outPath.parent_path());
    writeKotorStockLip(outPath, lip);
    auto runtime = readLip(outPath);
    auto v = validateLipForKotorRuntime(runtime, fs::file_size(outPath));
    std::cout << "input_lip\t" << inPath.string() << "\n";
    std::cout << "output_lip\t" << outPath.string() << "\n";
    std::cout << "visible_keys\t" << lip.entries.size() << "\n";
    std::cout << "runtime_verdict\t" << (v.ok ? "PASS" : "FAIL") << "\n";
    return v.ok ? 0 : 1;
}

static bool consumeLipTailRestFlag(const std::vector<std::string>& args, std::size_t& i, LipTailRestOptions& opt);
static std::string lipTailRestModeName(const LipTailRestOptions& opt);


static int parseNonNegativeMsForLip(const std::string& value, const std::string& flagName) {
    std::string v = value;
    if (v.size() > 2) {
        std::string suffix = v.substr(v.size() - 2);
        std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (suffix == "ms") v = v.substr(0, v.size() - 2);
    }
    std::size_t pos = 0;
    int out = 0;
    try { out = std::stoi(v, &pos, 10); }
    catch (...) { throw std::runtime_error(flagName + " requires a non-negative integer millisecond value or 'auto'"); }
    if (pos != v.size() || out < 0) throw std::runtime_error(flagName + " requires a non-negative integer millisecond value or 'auto'");
    if (out > 60000) throw std::runtime_error(flagName + " is unreasonably large (>60000 ms)");
    return out;
}

static bool consumeLipTailRestFlag(const std::vector<std::string>& args, std::size_t& i, LipTailRestOptions& opt) {
    const std::string& a = args[i];
    if (a != "--tail-rest-ms" && a != "--tail-rest") return false;
    if (i + 1 >= args.size()) throw std::runtime_error(a + " requires a value");
    std::string value = args[++i];
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (a == "--tail-rest" && value == "auto") {
        opt.automatic = true;
        opt.fixedMs = 0;
    } else if (value == "none" || value == "off") {
        opt.automatic = false;
        opt.fixedMs = 0;
    } else {
        opt.automatic = false;
        opt.fixedMs = parseNonNegativeMsForLip(value, a);
    }
    return true;
}

static std::string lipTailRestModeName(const LipTailRestOptions& opt) {
    return opt.automatic ? std::string("auto") : std::string("fixed");
}

static bool consumeLipQualityFlag(const std::vector<std::string>& args, std::size_t& i, LipQualityOptions& opt) {
    const std::string& a = args[i];
    if (a == "--close-on-pause-ms" || a == "--close-on-pause") {
        if (i + 1 >= args.size()) throw std::runtime_error(a + " requires a millisecond value");
        opt.closeOnPause = true;
        opt.closeOnPauseMs = parseNonNegativeMsForLip(args[++i], a);
        return true;
    }
    if (a == "--no-close-on-pause") {
        opt.closeOnPause = false;
        return true;
    }
    if (a == "--min-key-ms") {
        if (i + 1 >= args.size()) throw std::runtime_error("--min-key-ms requires a millisecond value");
        opt.minKeyMs = parseNonNegativeMsForLip(args[++i], a);
        if (opt.minKeyMs == 0) opt.smoothing = LipSmoothingPreset::None;
        return true;
    }
    if (a == "--smoothing" || a == "--mouth-smoothing") {
        if (i + 1 >= args.size()) throw std::runtime_error(a + " requires none, light, medium, or strong");
        applyLipSmoothingPreset(opt, parseLipSmoothingPreset(args[++i]));
        return true;
    }
    if (a == "--preserve-closures") {
        opt.preserveClosures = true;
        return true;
    }
    if (a == "--no-preserve-closures") {
        opt.preserveClosures = false;
        return true;
    }
    if (a == "--anticipate-closures-ms" || a == "--anticipate-closure-ms") {
        if (i + 1 >= args.size()) throw std::runtime_error(a + " requires a millisecond value");
        opt.anticipateClosuresMs = parseNonNegativeMsForLip(args[++i], a);
        return true;
    }
    if (a == "--anticipate-fricatives-ms" || a == "--anticipate-fricative-ms") {
        if (i + 1 >= args.size()) throw std::runtime_error(a + " requires a millisecond value");
        opt.anticipateFricativesMs = parseNonNegativeMsForLip(args[++i], a);
        return true;
    }
    if (a == "--anticipate-stops-ms" || a == "--anticipate-stop-ms" || a == "--anticipate-plosives-ms") {
        if (i + 1 >= args.size()) throw std::runtime_error(a + " requires a millisecond value");
        opt.anticipateStopsMs = parseNonNegativeMsForLip(args[++i], a);
        return true;
    }
    if (a == "--no-anticipation" || a == "--no-consonant-anticipation") {
        opt.anticipateClosuresMs = 0;
        opt.anticipateFricativesMs = 0;
        opt.anticipateStopsMs = 0;
        return true;
    }
    return false;
}

static std::string lipCloseOnPauseSetting(const LipQualityOptions& opt) {
    return opt.closeOnPause ? std::to_string(opt.closeOnPauseMs) : std::string("off");
}

static void printLipAnticipationSummary(const LipQualityOptions& quality) {
    std::cout << "anticipate_closures_ms\t" << quality.anticipateClosuresMs << "\n";
    std::cout << "anticipate_fricatives_ms\t" << quality.anticipateFricativesMs << "\n";
    std::cout << "anticipate_stops_ms\t" << quality.anticipateStopsMs << "\n";
}

static int cmdPhnToLip(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        usage();
        return 2;
    }
    const fs::path phnPath = args[1];
    const fs::path lipPath = args[2];
    LipTailRestOptions tail;
    LipQualityOptions quality;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (consumeLipTailRestFlag(args, i, tail)) continue;
        if (consumeLipQualityFlag(args, i, quality)) continue;
        throw std::runtime_error("unknown phn-to-lip option: " + args[i]);
    }
    auto phn = readPhn(phnPath);
    const int phnDurationMs = phn.intervals.empty() ? 0 : std::max(0, phn.intervals.back().endMs);
    const int tailMs = resolveTailRestMs(phnDurationMs, tail);
    writePhnAsLip(lipPath, phn, tail, quality);
    auto lip = readLip(lipPath);
    auto v = validateLipForKotorRuntime(lip, fs::file_size(lipPath));
    std::cout << "input_phn\t" << phnPath.string() << "\n";
    std::cout << "output_lip\t" << lipPath.string() << "\n";
    std::cout << "tail_rest_mode\t" << lipTailRestModeName(tail) << "\n";
    std::cout << "tail_rest_ms\t" << tailMs << "\n";
    std::cout << "close_on_pause_ms\t" << lipCloseOnPauseSetting(quality) << "\n";
    std::cout << "smoothing\t" << lipSmoothingPresetName(quality.smoothing) << "\n";
    std::cout << "min_key_ms\t" << quality.minKeyMs << "\n";
    std::cout << "preserve_closures\t" << (quality.preserveClosures ? "yes" : "no") << "\n";
    printLipAnticipationSummary(quality);
    std::cout << "engine_duration_float\t" << lip.durationSeconds << "\n";
    std::cout << "entry_count\t" << lip.frames.size() << "\n";
    std::cout << "runtime_verdict\t" << (v.ok ? "PASS" : "FAIL") << "\n";
    for (const auto& e : v.errors) std::cout << "error\t" << e << "\n";
    for (const auto& w : v.warnings) std::cout << "warning\t" << w << "\n";
    return v.ok ? 0 : 1;
}

static LipFile writeLipFromPhnToPath(const PhnFile& phn, const fs::path& lipPath, LipTailRestOptions tail = {}, LipQualityOptions quality = {}) {
    if (!lipPath.parent_path().empty()) ensureDirectory(lipPath.parent_path());
    writePhnAsLip(lipPath, phn, tail, quality);
    LipFile lip = readLip(lipPath);
    LipValidation v = validateLipForKotorRuntime(lip, fs::file_size(lipPath));
    if (!v.ok) {
        std::ostringstream ss;
        ss << "generated LIP failed KOTOR runtime validation";
        for (const auto& e : v.errors) ss << "; " << e;
        throw std::runtime_error(ss.str());
    }
    return lip;
}

static void printLipSummary(const LipFile& lip, const fs::path& lipPath, const LipTailRestOptions& tail = {}, int effectiveTailRestMs = 0, const LipQualityOptions& quality = {}) {
    LipValidation v = validateLipForKotorRuntime(lip, 0);
    std::cout << "output_lip\t" << lipPath.string() << "\n";
    std::cout << "tail_rest_mode\t" << lipTailRestModeName(tail) << "\n";
    std::cout << "tail_rest_ms\t" << effectiveTailRestMs << "\n";
    std::cout << "close_on_pause_ms\t" << lipCloseOnPauseSetting(quality) << "\n";
    std::cout << "smoothing\t" << lipSmoothingPresetName(quality.smoothing) << "\n";
    std::cout << "min_key_ms\t" << quality.minKeyMs << "\n";
    std::cout << "preserve_closures\t" << (quality.preserveClosures ? "yes" : "no") << "\n";
    printLipAnticipationSummary(quality);
    std::cout << "lip_engine_duration_float\t" << std::setprecision(17) << lip.durationSeconds << "\n";
    std::cout << "lip_entry_count\t" << lip.frames.size() << "\n";
    std::cout << "lip_runtime_verdict\t" << (v.ok ? "PASS" : "FAIL") << "\n";
    for (const auto& e : v.errors) std::cout << "lip_error\t" << e << "\n";
    for (const auto& w : v.warnings) std::cout << "lip_warning\t" << w << "\n";
}

static fs::path defaultIntermediatePhnForLip(const fs::path& lipPath, const fs::path& workDir) {
    if (!workDir.empty()) return workDir / "output" / "phn.txt";
    fs::path p = lipPath;
    p += ".generated.phn";
    return p;
}

static void writePhnEnsuringParent(const fs::path& path, const PhnFile& phn) {
    if (!path.parent_path().empty()) ensureDirectory(path.parent_path());
    writePhn(path, phn);
}

static int cmdWavInfo(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path path = args[1];
    const auto info = readWavInfo(path);
    std::cout << std::setprecision(17);
    std::cout << "file\t" << path.string() << "\n";
    std::cout << "container\t" << toString(info.containerKind) << "\n";
    std::cout << "encoding\t" << toString(info.encodingKind) << "\n";
    std::cout << "description\t" << info.description << "\n";
    std::cout << "format_tag\t" << info.formatTag << "\n";
    std::cout << "effective_format_tag\t" << info.effectiveFormatTag << "\n";
    std::cout << "channels\t" << info.channels << "\n";
    std::cout << "sample_rate\t" << info.sampleRate << "\n";
    std::cout << "byte_rate\t" << info.byteRate << "\n";
    std::cout << "block_align\t" << info.blockAlign << "\n";
    std::cout << "bits_per_sample\t" << info.bitsPerSample << "\n";
    std::cout << "valid_bits_per_sample\t" << info.validBitsPerSample << "\n";
    std::cout << "samples_per_block\t" << info.samplesPerBlock << "\n";
    std::cout << "channel_mask\t" << info.channelMask << "\n";
    std::cout << "data_bytes\t" << info.dataBytes << "\n";
    std::cout << "frames\t" << info.frames << "\n";
    std::cout << "duration_seconds\t" << info.durationSeconds << "\n";
    std::cout << "can_decode_natively\t" << (info.canDecodeNatively ? "yes" : "no") << "\n";
    std::cout << "requires_external_decoder\t" << (info.requiresExternalDecoder ? "yes" : "no") << "\n";
    std::cout << "kotor_stream_candidate\t" << (info.isKotorStreamCandidate ? "yes" : "no") << "\n";
    std::cout << "min_sample\t" << info.minSample << "\n";
    std::cout << "max_sample\t" << info.maxSample << "\n";
    std::cout << "mean_sample\t" << info.meanSample << "\n";
    if (!info.warning.empty()) std::cout << "warning\t" << info.warning << "\n";
    std::cout << "chunks\t" << info.chunks.size() << "\n";
    for (std::size_t i = 0; i < info.chunks.size(); ++i) {
        std::cout << "chunk\t" << i << "\t" << info.chunks[i].id << "\t" << info.chunks[i].offset << "\t" << info.chunks[i].size << "\n";
    }
    return 0;
}

static int cmdDecodeAudioWav(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const fs::path input = args[1];
    const fs::path output = args[2];
    const Pcm16Mono pcm = readPcm16Mono(input);
    writePcm16Mono(pcm, output);
    std::cout << "wrote\t" << output.string() << "\n";
    std::cout << "sample_rate\t" << pcm.sampleRate << "\n";
    std::cout << "samples\t" << pcm.samples.size() << "\n";
    std::cout << "duration_seconds\t" << (pcm.sampleRate ? static_cast<double>(pcm.samples.size()) / static_cast<double>(pcm.sampleRate) : 0.0) << "\n";
    return 0;
}

static int cmdComposeAllFeat(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const fs::path outPath = args[2];
    const Matrix allFeat = composeAllFeatFromDump(dump);
    writeNumericTsv(allFeat, outPath);
    std::cout << "wrote\t" << outPath.string() << "\n";
    std::cout << "rows\t" << allFeat.rows << "\n";
    std::cout << "cols\t" << allFeat.cols << "\n";
    return 0;
}

static int cmdCompareAllFeat(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const Matrix actual = composeAllFeatFromDump(dump);
    const Matrix expected = readDumpMatrix(dump, "features/all_feat");
    printMatrixCompare("features/all_feat", compareMatrixValues(actual, expected));
    return 0;
}

static int cmdComposeNetIn(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const fs::path allFeatPath = args[1];
    const fs::path outPath = args[2];
    const Matrix allFeat = readNumericTsv(allFeatPath);
    const Matrix netin = composeNetInFromAllFeat(allFeat);
    writeNumericTsv(netin, outPath);
    std::cout << "wrote\t" << outPath.string() << "\n";
    std::cout << "rows\t" << netin.rows << "\n";
    std::cout << "cols\t" << netin.cols << "\n";
    return 0;
}

static int compareOneNetIn(const fs::path& dump, const std::string& which) {
    const Matrix allFeat = readDumpMatrix(dump, "features/all_feat");
    const Matrix actual = composeNetInFromAllFeat(allFeat);
    const Matrix expected = readDumpMatrix(dump, "netin/" + which);
    printMatrixCompare("netin/" + which, compareMatrixValues(actual, expected));
    return 0;
}

static int cmdCompareNetIn(const std::vector<std::string>& args) {
    if (args.size() < 2 || args.size() > 3) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const std::string which = (args.size() == 3) ? args[2] : "all";
    if (which == "all") {
        for (const char* name : {"manner", "place", "height", "trans"}) {
            compareOneNetIn(dump, name);
            std::cout << "\n";
        }
    } else if (which == "manner" || which == "place" || which == "height" || which == "trans") {
        compareOneNetIn(dump, which);
    } else {
        throw std::runtime_error("unknown netin name: " + which);
    }
    return 0;
}


static int cmdComposeLogEnergy(const std::vector<std::string>& args) {
    if (args.size() < 4 || args.size() > 5) {
        usage();
        return 2;
    }
    const fs::path wavPath = args[1];
    const fs::path outPath = args[2];
    const double winMs = std::stod(args[3]);
    std::size_t rows = 0;
    if (args.size() == 5) {
        rows = static_cast<std::size_t>(std::stoull(args[4]));
    }
    const Pcm16Mono wav = readPcm16Mono(wavPath);
    if (wav.sampleRate != 16000) {
        throw std::runtime_error("compose-logenergy currently expects a 16 kHz PCM WAV");
    }
    const Matrix m = computeLogEnergy(wav.samples, 16000, winMs, 5.0, rows);
    writeNumericTsv(m, outPath);
    std::cout << "wrote\t" << outPath.string() << "\n";
    std::cout << "rows\t" << m.rows << "\n";
    std::cout << "cols\t" << m.cols << "\n";
    return 0;
}

static int cmdComposeIntensityEnergy(const std::vector<std::string>& args) {
    if (args.size() < 3 || args.size() > 4) {
        usage();
        return 2;
    }
    const fs::path wavPath = args[1];
    const fs::path outPath = args[2];
    std::size_t rows = 0;
    if (args.size() == 4) {
        rows = static_cast<std::size_t>(std::stoull(args[3]));
    }
    const Pcm16Mono wav = readPcm16Mono(wavPath);
    if (wav.sampleRate != 16000) {
        throw std::runtime_error("compose-intensity-energy currently expects a 16 kHz PCM WAV");
    }
    const Matrix m = computeIntensityEnergyCol0(wav.samples, 16000, 5.0, 40.0, rows, 10);
    writeNumericTsv(m, outPath);
    std::cout << "wrote\t" << outPath.string() << "\n";
    std::cout << "rows\t" << m.rows << "\n";
    std::cout << "cols\t" << m.cols << "\n";
    return 0;
}

static int cmdCompareIntensityEnergyTrace(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const fs::path wavPath = dump / "audio" / "wave_16k_after_dc.wav";
    const fs::path expectedPath = dump / "features" / "intensity_energy_col0.tsv";
    const Matrix expected = readNumericTsv(expectedPath);
    const Pcm16Mono wav = readPcm16Mono(wavPath);
    const Matrix actual = computeIntensityEnergyCol0(wav.samples, 16000, 5.0, 40.0, expected.rows, 10);
    printMatrixCompare("features/intensity_energy_col0", compareMatrixValues(actual, expected), 1e-6, 1e-4);
    return 0;
}



static int cmdComposeIntensityDiscrimination(const std::vector<std::string>& args) {
    if (args.size() < 3 || args.size() > 4) {
        usage();
        return 2;
    }
    const fs::path wavPath = args[1];
    const fs::path outPath = args[2];
    std::size_t rows = 0;
    if (args.size() == 4) {
        rows = static_cast<std::size_t>(std::stoull(args[3]));
    }
    const Pcm16Mono wav = readPcm16Mono(wavPath);
    if (wav.sampleRate != 16000) {
        throw std::runtime_error("compose-intensity-discrimination currently expects a 16 kHz PCM WAV");
    }
    const Matrix m = computeIntensityDiscrimination16k(wav.samples, rows);
    writeNumericTsv(m, outPath);
    std::cout << "wrote\t" << outPath.string() << "\n";
    std::cout << "rows\t" << m.rows << "\n";
    std::cout << "cols\t" << m.cols << "\n";
    return 0;
}

static int cmdCompareIntensityDiscriminationTrace(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const Matrix expected = readDumpMatrix(dump, "features/intensity_0_15");
    const Pcm16Mono wav = readPcm16Mono(dump / "audio" / "wave_16k_after_dc.wav");
    if (wav.sampleRate != 16000) {
        throw std::runtime_error("trace wave_16k_after_dc.wav is not 16 kHz");
    }
    const Matrix actual = computeIntensityDiscrimination16k(wav.samples, expected.rows);
    printMatrixCompare("features/intensity_0_15", compareMatrixValues(actual, expected), 1e-6, 1e-4);
    return 0;
}

static int cmdComposeBurst(const std::vector<std::string>& args) {
    if (args.size() < 4 || args.size() > 5) {
        usage();
        return 2;
    }
    const fs::path wavPath = args[1];
    const fs::path burstPath = args[2];
    const fs::path pulsesPath = args[3];
    std::size_t rows = 0;
    if (args.size() == 5) {
        rows = static_cast<std::size_t>(std::stoull(args[4]));
    }
    const Pcm16Mono wav = readPcm16Mono(wavPath);
    if (wav.sampleRate != 8000) {
        throw std::runtime_error("compose-burst currently expects an 8 kHz PCM WAV");
    }
    const Matrix burst = computeBurstMx8k(wav.samples, rows);
    const Matrix pulses = computeBurstPulsesCol7(burst);
    writeNumericTsv(burst, burstPath);
    writeNumericTsv(pulses, pulsesPath);
    std::cout << "wrote_burst_mx\t" << burstPath.string() << "\n";
    std::cout << "wrote_burst_pulses_col7\t" << pulsesPath.string() << "\n";
    std::cout << "rows\t" << burst.rows << "\n";
    std::cout << "cols\t" << burst.cols << "\n";
    return 0;
}

static int cmdCompareBurstTrace(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const Matrix expectedBurst = readDumpMatrix(dump, "features/burst_mx");
    const Matrix expectedPulses = readDumpMatrix(dump, "features/burst_pulses_col7");
    const Pcm16Mono wav = readPcm16Mono(dump / "audio" / "wave_8k_after_dc.wav");
    if (wav.sampleRate != 8000) {
        throw std::runtime_error("trace wave_8k_after_dc.wav is not 8 kHz");
    }
    const Matrix actualBurst = computeBurstMx8k(wav.samples, expectedBurst.rows);
    const Matrix actualPulses = computeBurstPulsesCol7(actualBurst);
    printMatrixCompare("features/burst_mx", compareMatrixValues(actualBurst, expectedBurst), 1e-7, 1e-6);
    std::cout << "note\tburst_mx now matches CSLU at trace precision, including diagnostic band columns 0..6.\n\n";
    printMatrixCompare("features/burst_pulses_col7", compareMatrixValues(actualPulses, expectedPulses), 1e-7, 1e-6);
    std::cout << "note\tburst_pulses_col7 now matches CSLU at trace precision after recovering burst.dll partial-edge delta, non-causal FIR preload/flush, and 12 ms post-compute delay.\n";
    return 0;
}

static int cmdCompareLogEnergyTrace(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const Pcm16Mono wav = readPcm16Mono(dump / "audio" / "wave_16k_after_dc.wav");
    ValidatedEnergyFeatures e = computeValidatedEnergyFeatures16k(wav);
    printMatrixCompare("features/spec_16000_24.0/energy", compareMatrixValues(e.specEnergy24, readDumpMatrix(dump, "features/spec_16000_24.0/energy")), 1e-4, 1e-3);
    printMatrixCompare("features/spec_16000_24.0/energy_bigwin", compareMatrixValues(e.specEnergyBigwin100, readDumpMatrix(dump, "features/spec_16000_24.0/energy_bigwin")), 1e-4, 1e-3);
    printMatrixCompare("features/spec_16000_24.0/energy_norm", compareMatrixValues(e.specEnergyNorm24, readDumpMatrix(dump, "features/spec_16000_24.0/energy_norm")), 1e-6, 1e-4);
    printMatrixCompare("features/intensity_energy_col0", compareMatrixValues(e.intensityEnergyCol0, readDumpMatrix(dump, "features/intensity_energy_col0")), 1e-6, 1e-4);
    return 0;
}


static void printSampleCompare(const std::string& label, const Pcm16Mono& actual, const Pcm16Mono& expected) {
    if (actual.sampleRate != expected.sampleRate) {
        std::cout << "sample_compare\t" << label << "\nverdict\tFAIL\nreason\tsample-rate mismatch\n\n";
        return;
    }
    const std::size_t n = std::min(actual.samples.size(), expected.samples.size());
    double sumAbs = 0.0;
    double maxAbs = 0.0;
    std::size_t maxIdx = 0;
    std::size_t exact = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double e = std::abs(actual.samples[i] - expected.samples[i]);
        sumAbs += e;
        if (e > maxAbs) { maxAbs = e; maxIdx = i; }
        if (e == 0.0) ++exact;
    }
    std::cout << std::setprecision(17);
    std::cout << "sample_compare\t" << label << "\n";
    std::cout << "sample_rate\t" << actual.sampleRate << "\n";
    std::cout << "samples_compared\t" << n << "\n";
    std::cout << "actual_samples\t" << actual.samples.size() << "\n";
    std::cout << "expected_samples\t" << expected.samples.size() << "\n";
    std::cout << "exact_samples\t" << exact << "\n";
    std::cout << "exact_fraction\t" << (n ? static_cast<double>(exact) / static_cast<double>(n) : 0.0) << "\n";
    std::cout << "mean_abs_error\t" << (n ? sumAbs / static_cast<double>(n) : 0.0) << "\n";
    std::cout << "max_abs_error\t" << maxAbs << "\n";
    std::cout << "max_error_sample\t" << maxIdx << "\n";
    std::cout << "verdict\t" << ((maxAbs <= 1.0) ? "PASS" : "CHECK") << "\n\n";
}

static int cmdComposeDc(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const Pcm16Mono wav = readPcm16Mono(args[1]);
    const Pcm16Mono dc = applyPrepDcrmIir(wav);
    writeNumericTsv(pcmSamplesColumn(dc), args[2], 0);
    std::cout << "wrote\t" << args[2] << "\n";
    std::cout << "sample_rate\t" << dc.sampleRate << "\n";
    std::cout << "samples\t" << dc.samples.size() << "\n";
    return 0;
}

static int cmdComposeDcWav(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const Pcm16Mono wav = readPcm16Mono(args[1]);
    const Pcm16Mono dc = applyPrepDcrmIir(wav);
    writePcm16Mono(dc, args[2]);
    std::cout << "wrote\t" << args[2] << "\n";
    std::cout << "sample_rate\t" << dc.sampleRate << "\n";
    std::cout << "samples\t" << dc.samples.size() << "\n";
    return 0;
}

static int cmdCompareDcTrace(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    for (const auto& stem : {std::string("wave_8k"), std::string("wave_16k")}) {
        const Pcm16Mono before = readPcm16Mono(dump / "audio" / (stem + "_before_dc.wav"));
        const Pcm16Mono expected = readPcm16Mono(dump / "audio" / (stem + "_after_dc.wav"));
        const Pcm16Mono actual = applyPrepDcrmIir(before);
        printSampleCompare(stem + " prep_!dc", actual, expected);
    }
    std::cout << "note\tprep.dll dcrm was recovered by disassembly and matches both traced post-DC WAVs exactly.\n";
    return 0;
}

static int cmdComposePlpApprox(const std::vector<std::string>& args) {
    if (args.size() < 3 || args.size() > 4) {
        usage();
        return 2;
    }
    std::size_t rows = 0;
    if (args.size() == 4) rows = static_cast<std::size_t>(std::stoull(args[3]));
    const Pcm16Mono wav = readPcm16Mono(args[1]);
    const fs::path out = args[2];
    const PlpApproxFeatures f = computePlpApprox16k(wav, rows);
    writeNumericTsv(f.initFeat, out / "init_feat.approx.tsv");
    writeNumericTsv(f.featMfccPadded, out / "feat_mfcc_padded.approx.tsv");
    writeNumericTsv(f.featCms, out / "feat_cms.approx.tsv");
    writeNumericTsv(f.featNoDelta, out / "feat_nodelta.approx.tsv");
    writeNumericTsv(f.featDelta, out / "feat_delta.approx.tsv");
    writeNumericTsv(f.featWithDelta, out / "feat_with_delta.approx.tsv");
    writeNumericTsv(f.featWithDelta, out / "plp_feat_16k.approx.tsv");
    std::cout << "out_dir\t" << out.string() << "\n";
    std::cout << "init_rows\t" << f.initFeat.rows << "\n";
    std::cout << "final_rows\t" << f.featWithDelta.rows << "\n";
    std::cout << "final_cols\t" << f.featWithDelta.cols << "\n";
    std::cout << "cms_begin\t" << f.cmsBegin << "\n";
    std::cout << "cms_end\t" << f.cmsEnd << "\n";
    std::cout << "min_energy_frame\t" << f.minEnergyFrame << "\n";
    std::cout << "verdict\tAPPROX_NOT_TRACE_EXACT\n";
    return 0;
}

static int cmdComparePlpApproxTrace(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const Pcm16Mono wav = readPcm16Mono(dump / "audio" / "wave_16k_after_dc.wav");
    const Matrix expectedPlp = readDumpMatrix(dump, "features/plp_feat_16k");
    const PlpApproxFeatures f = computePlpApprox16k(wav, expectedPlp.rows);
    printMatrixCompare("features/spec_16000_24.0/init_feat.frontend", compareMatrixValues(f.initFeat, readDumpMatrix(dump, "features/spec_16000_24.0/init_feat")), 1e-5, 1e-4);
    printMatrixCompare("features/spec_16000_24.0/feat_mfcc_padded.frontend", compareMatrixValues(f.featMfccPadded, readDumpMatrix(dump, "features/spec_16000_24.0/feat_mfcc_padded")), 1e-5, 1e-4);
    printMatrixCompare("features/spec_16000_24.0/feat_nodelta.frontend", compareMatrixValues(f.featNoDelta, readDumpMatrix(dump, "features/spec_16000_24.0/feat_nodelta")), 1e-5, 1e-4);
    printMatrixCompare("features/plp_feat_16k.frontend", compareMatrixValues(f.featWithDelta, expectedPlp), 1e-5, 1e-4);
    std::cout << "cms_begin\t" << f.cmsBegin << "\n";
    std::cout << "cms_end\t" << f.cmsEnd << "\n";
    std::cout << "min_energy_frame\t" << f.minEnergyFrame << "\n";
    std::cout << "note\tFeature.dll Bark filterbank truncation recovered: each filter sums FFT bins over [center-2.5, center+1.3] Bark before the asymmetric default weighting. Current trace matches to TSV/float precision.\n";
    return 0;
}


static int cmdCompareDeltaTrace(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const Matrix nodelta = readDumpMatrix(dump, "features/spec_16000_24.0/feat_nodelta");
    const Matrix expected = readDumpMatrix(dump, "features/spec_16000_24.0/feat_delta");
    const Matrix actual = computeCsluDeltaOrder2(nodelta);
    printMatrixCompare("features/spec_16000_24.0/feat_delta", compareMatrixValues(actual, expected), 1e-6, 1e-4);
    std::cout << "formula\t(x[t+1]-x[t-1] + 2*(x[t+2]-x[t-2])) / 10, edge-frame replication\n";
    return 0;
}

static PlpApproxFeatures buildPlpPostFromTrace(const fs::path& dump) {
    const Matrix initFeat = readDumpMatrix(dump, "features/spec_16000_24.0/init_feat");
    const Matrix energyNorm = readDumpMatrix(dump, "features/spec_16000_24.0/energy_norm");
    const Matrix energyBigwin = readDumpMatrix(dump, "features/spec_16000_24.0/energy_bigwin");
    const Matrix expectedFinal = readDumpMatrix(dump, "features/plp_feat_16k");
    return postprocessPlpFromInitAndEnergy(initFeat, energyNorm, energyBigwin, expectedFinal.rows);
}

static int cmdComparePlpPostTrace(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const PlpApproxFeatures f = buildPlpPostFromTrace(dump);
    std::cout << "cms_min_energy_frame\t" << f.minEnergyFrame << "\n";
    std::cout << "cms_begin\t" << f.cmsBegin << "\n";
    std::cout << "cms_end\t" << f.cmsEnd << "\n\n";
    printMatrixCompare("features/spec_16000_24.0/feat_mfcc_padded", compareMatrixValues(f.featMfccPadded, readDumpMatrix(dump, "features/spec_16000_24.0/feat_mfcc_padded")), 1e-6, 1e-4);
    std::cout << "\n";
    printMatrixCompare("features/spec_16000_24.0/feat_cms", compareMatrixValues(f.featCms, readDumpMatrix(dump, "features/spec_16000_24.0/feat_cms")), 1e-5, 1e-4);
    std::cout << "\n";
    printMatrixCompare("features/spec_16000_24.0/feat_nodelta", compareMatrixValues(f.featNoDelta, readDumpMatrix(dump, "features/spec_16000_24.0/feat_nodelta")), 1e-6, 1e-4);
    std::cout << "\n";
    printMatrixCompare("features/spec_16000_24.0/feat_delta", compareMatrixValues(f.featDelta, readDumpMatrix(dump, "features/spec_16000_24.0/feat_delta")), 1e-6, 1e-4);
    std::cout << "\n";
    printMatrixCompare("features/spec_16000_24.0/feat_with_delta", compareMatrixValues(f.featWithDelta, readDumpMatrix(dump, "features/spec_16000_24.0/feat_with_delta")), 1e-6, 1e-4);
    std::cout << "\n";
    printMatrixCompare("features/plp_feat_16k", compareMatrixValues(f.featWithDelta, readDumpMatrix(dump, "features/plp_feat_16k")), 1e-6, 1e-4);
    return 0;
}

static int cmdComposePlpPostTrace(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const fs::path out = args[2];
    const PlpApproxFeatures f = buildPlpPostFromTrace(dump);
    writeNumericTsv(f.featMfccPadded, out / "feat_mfcc_padded.tsv");
    writeNumericTsv(f.featCms, out / "feat_cms.tsv");
    writeNumericTsv(f.featNoDelta, out / "feat_nodelta.tsv");
    writeNumericTsv(f.featDelta, out / "feat_delta.tsv");
    writeNumericTsv(f.featWithDelta, out / "feat_with_delta.tsv");
    writeNumericTsv(f.featWithDelta, out / "plp_feat_16k.tsv");
    std::cout << "out_dir\t" << out.string() << "\n";
    std::cout << "rows\t" << f.featWithDelta.rows << "\n";
    std::cout << "cols\t" << f.featWithDelta.cols << "\n";
    std::cout << "cms_begin\t" << f.cmsBegin << "\n";
    std::cout << "cms_end\t" << f.cmsEnd << "\n";
    return 0;
}



static std::vector<std::pair<std::string, fs::path>> csluModelList(const fs::path& modelRoot) {
    return {
        {"bc_m_orig_out", modelRoot / "bc_16k" / "manner_nnet.45"},
        {"bc_p_orig_out", modelRoot / "bc_16k" / "place_nnet.45"},
        {"bc_h_orig_out", modelRoot / "bc_16k" / "height_nnet.45"},
        {"trans_m_orig_out", modelRoot / "trans_16k" / "manner_nnet.45"},
        {"trans_p_orig_out", modelRoot / "trans_16k" / "place_nnet.45"},
        {"trans_h_orig_out", modelRoot / "trans_16k" / "height_nnet.45"}
    };
}

static void writeNnetOutputsFromModels(const fs::path& modelRoot,
                                       const Matrix& netin,
                                       const fs::path& outDir) {
    for (const auto& item : csluModelList(modelRoot)) {
        if (!fs::exists(item.second)) {
            throw std::runtime_error("missing model: " + item.second.string());
        }
        const NNet45 net = NNet45::load(item.second);
        const Matrix out = net.forwardMatrix(netin);
        writeNumericTsv(out, outDir / "nnet" / (item.first + ".tsv"));
    }
}

static void printNNetCompare(const std::string& label, const NNetCompareStats& c) {
    std::cout << std::setprecision(17);
    std::cout << "matrix\t" << label << "\n";
    std::cout << "rows\t" << c.rows << "\n";
    std::cout << "cols\t" << c.cols << "\n";
    std::cout << "mean_abs_error\t" << c.meanAbsError << "\n";
    std::cout << "max_abs_error\t" << c.maxAbsError << "\n";
    std::cout << "max_error_row\t" << c.maxRow << "\n";
    std::cout << "max_error_col\t" << c.maxCol << "\n";
    std::cout << "top1_matches\t" << c.top1Matches << "\n";
    std::cout << "top1_total\t" << c.rows << "\n";
    std::cout << "verdict\t" << ((c.meanAbsError < 1e-5 && c.maxAbsError < 5e-4) ? "PASS" : "CHECK") << "\n";
}

static FullFrontendFeatures buildFullFrontendFromTracePreDcWavs(const fs::path& dump) {
    const Pcm16Mono wave16 = readPcm16Mono(dump / "audio" / "wave_16k_before_dc.wav");
    const Pcm16Mono wave8 = readPcm16Mono(dump / "audio" / "wave_8k_before_dc.wav");
    return computeFullFrontendFromPreDcWavs(wave16, wave8);
}
static int cmdCompareSampleConvertTrace(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const Pcm16Mono original = readPcm16Mono(dump / "audio" / "input_original_wave.wav");
    const Pcm16Mono wave16 = sampleConvertWaveDllCompatible(original, 16000);
    const Pcm16Mono wave8 = sampleConvertWaveDllCompatible(original, 8000);
    const Pcm16Mono expected16 = readPcm16Mono(dump / "audio" / "wave_16k_before_dc.wav");
    const Pcm16Mono expected8 = readPcm16Mono(dump / "audio" / "wave_8k_before_dc.wav");

    std::cout << "original_rate\t" << original.sampleRate << "\n";
    std::cout << "original_samples\t" << original.samples.size() << "\n";
    std::cout << "resampler\twave_dll_sampleconvert_compatible\n\n";
    printMatrixCompare("audio/wave_16k_before_dc.sampleconvert", compareMatrixValues(pcmSamplesColumn(wave16), pcmSamplesColumn(expected16)), 0.0, 0.0);
    std::cout << "\n";
    printMatrixCompare("audio/wave_8k_before_dc.sampleconvert", compareMatrixValues(pcmSamplesColumn(wave8), pcmSamplesColumn(expected8)), 0.0, 0.0);

    const MatrixCompareStats c16 = compareMatrixValues(pcmSamplesColumn(wave16), pcmSamplesColumn(expected16));
    const MatrixCompareStats c8 = compareMatrixValues(pcmSamplesColumn(wave8), pcmSamplesColumn(expected8));
    const bool ok16 = (wave16.sampleRate == expected16.sampleRate && wave16.samples.size() == expected16.samples.size() && c16.maxAbsError == 0.0);
    const bool ok8 = (wave8.sampleRate == expected8.sampleRate && wave8.samples.size() == expected8.samples.size() && c8.maxAbsError == 0.0);
    std::cout << "\nverdict\t" << ((ok16 && ok8) ? "PASS" : "CHECK") << "\n";
    return (ok16 && ok8) ? 0 : 1;
}


static int cmdComposeFullFrontend(const std::vector<std::string>& args) {
    if (args.size() < 4 || args.size() > 5) {
        usage();
        return 2;
    }
    const fs::path wave16Path = args[1];
    const fs::path wave8Path = args[2];
    const fs::path outDir = args[3];
    const Pcm16Mono wave16 = readPcm16Mono(wave16Path);
    const Pcm16Mono wave8 = readPcm16Mono(wave8Path);
    const FullFrontendFeatures f = computeFullFrontendFromPreDcWavs(wave16, wave8);
    writeFullFrontendFeatures(f, outDir);
    if (args.size() == 5) {
        writeNnetOutputsFromModels(args[4], f.netin, outDir);
    }
    std::cout << "out_dir\t" << outDir.string() << "\n";
    std::cout << "all_feat_rows\t" << f.allFeat.rows << "\n";
    std::cout << "all_feat_cols\t" << f.allFeat.cols << "\n";
    std::cout << "netin_rows\t" << f.netin.rows << "\n";
    std::cout << "netin_cols\t" << f.netin.cols << "\n";
    std::cout << "plp_cms_begin\t" << f.plp.cmsBegin << "\n";
    std::cout << "plp_cms_end\t" << f.plp.cmsEnd << "\n";
    if (args.size() == 5) {
        std::cout << "nnet_outputs\twritten\n";
    }
    return 0;
}

static int cmdComposeFullFrontendFromWav(const std::vector<std::string>& args) {
    if (args.size() < 3 || args.size() > 4) {
        usage();
        return 2;
    }
    const fs::path wavPath = args[1];
    const fs::path outDir = args[2];
    const Pcm16Mono original = readPcm16Mono(wavPath);
    const FullFrontendFeatures f = computeFullFrontendFromOriginalWav(original);
    writeFullFrontendFeatures(f, outDir);
    if (args.size() == 4) {
        writeNnetOutputsFromModels(args[3], f.netin, outDir);
    }
    std::cout << "out_dir\t" << outDir.string() << "\n";
    std::cout << "original_rate\t" << original.sampleRate << "\n";
    std::cout << "original_samples\t" << original.samples.size() << "\n";
    std::cout << "resampler\twave_dll_sampleconvert_compatible\n";
    std::cout << "wave16_samples\t" << f.wave16BeforeDc.samples.size() << "\n";
    std::cout << "wave8_samples\t" << f.wave8BeforeDc.samples.size() << "\n";
    std::cout << "all_feat_rows\t" << f.allFeat.rows << "\n";
    std::cout << "all_feat_cols\t" << f.allFeat.cols << "\n";
    std::cout << "netin_rows\t" << f.netin.rows << "\n";
    std::cout << "netin_cols\t" << f.netin.cols << "\n";
    if (args.size() == 4) {
        std::cout << "nnet_outputs\twritten\n";
    }
    std::cout << "note\tSingle-WAV mode now uses the recovered wave.dll Wave_sampleConvert-compatible FIR/polyphase resampler.\n";
    return 0;
}

static int cmdCompareFullFrontendTrace(const std::vector<std::string>& args) {
    if (args.size() < 2 || args.size() > 3) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const FullFrontendFeatures f = buildFullFrontendFromTracePreDcWavs(dump);

    printMatrixCompare("audio/wave_16k_after_dc.samples", compareMatrixValues(pcmSamplesColumn(f.wave16AfterDc), pcmSamplesColumn(readPcm16Mono(dump / "audio" / "wave_16k_after_dc.wav"))), 0.0, 0.0);
    std::cout << "\n";
    printMatrixCompare("audio/wave_8k_after_dc.samples", compareMatrixValues(pcmSamplesColumn(f.wave8AfterDc), pcmSamplesColumn(readPcm16Mono(dump / "audio" / "wave_8k_after_dc.wav"))), 0.0, 0.0);
    std::cout << "\n";
    printMatrixCompare("features/plp_feat_16k.full_frontend", compareMatrixValues(f.plpFeat16k, readDumpMatrix(dump, "features/plp_feat_16k")), 1e-5, 1e-4);
    std::cout << "\n";
    printMatrixCompare("features/burst_mx.full_frontend", compareMatrixValues(f.burstMx, readDumpMatrix(dump, "features/burst_mx")), 1e-3, 2e-1);
    std::cout << "\n";
    printMatrixCompare("features/burst_pulses_col7.full_frontend", compareMatrixValues(f.burstPulsesCol7, readDumpMatrix(dump, "features/burst_pulses_col7")), 1e-3, 2e-1);
    std::cout << "\n";
    printMatrixCompare("features/intensity_0_15.full_frontend", compareMatrixValues(f.intensity0To15, readDumpMatrix(dump, "features/intensity_0_15")), 1e-6, 1e-4);
    std::cout << "\n";
    printMatrixCompare("features/intensity_energy_col0.full_frontend", compareMatrixValues(f.intensityEnergyCol0, readDumpMatrix(dump, "features/intensity_energy_col0")), 1e-6, 1e-4);
    std::cout << "\n";
    printMatrixCompare("features/all_feat.full_frontend", compareMatrixValues(f.allFeat, readDumpMatrix(dump, "features/all_feat")), 1e-4, 2e-1);
    std::cout << "\n";
    printMatrixCompare("netin/manner.full_frontend", compareMatrixValues(f.netin, readDumpMatrix(dump, "netin/manner")), 1e-4, 2e-1);

    if (args.size() == 3) {
        const fs::path modelRoot = args[2];
        std::cout << "\n";
        for (const auto& item : csluModelList(modelRoot)) {
            const NNet45 net = NNet45::load(item.second);
            const Matrix actual = net.forwardMatrix(f.netin);
            const Matrix expected = readDumpMatrix(dump, "nnet/" + item.first);
            printNNetCompare("nnet/" + item.first + ".full_frontend", compareMatrices(actual, expected));
            std::cout << "\n";
        }
    }

    std::cout << "frontend_rows\t" << f.allFeat.rows << "\n";
    std::cout << "frontend_cols\t" << f.allFeat.cols << "\n";
    std::cout << "netin_rows\t" << f.netin.rows << "\n";
    std::cout << "netin_cols\t" << f.netin.cols << "\n";
    std::cout << "note\tThis comparison starts from pre-DC WAVs only. It does not read traced feature component matrices except as expected outputs for validation.\n";
    return 0;
}
static int cmdRawFrontEndStatus(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        usage();
        return 2;
    }
    std::cout << "raw_front_end_phase\t13_sampleconvert_exact\n";
    std::cout << "implemented_exact\twave.dll Wave_sampleConvert for FA sample-rate branches; prep.dll dcrm DC-removal; Logenergy_compute; Intensity_normenergy energy column; Intensity_discrimination 16-column matrix; feature.dll Bark/cepstral init_feat for the FA path; feature delta; PLP Tcl-side post-processing; integrated full frontend from pre-DC 16k/8k WAVs to all_feat/netin; garbage.dll median -N 20; phnfeat.dll phn/trans probability combiners\n";
    std::cout << "implemented_near_exact\tnone in the FA acoustic frontend for the supplied trace; Burst_compute diagnostic columns are now trace-precision\n";
    std::cout << "validated_inputs\tdump/audio/wave_16k_before_dc.wav; dump/audio/wave_8k_before_dc.wav; dump/audio/wave_16k_after_dc.wav; dump/audio/wave_8k_after_dc.wav\n";
    std::cout << "validated_outputs\tprep post-DC WAVs; spec energy; spec big-window energy; normalized spec energy; intensity_energy_col0; intensity_0_15; burst_mx; burst_pulses_col7; feature init_feat; feat_delta; PLP matrices; integrated all_feat/netin; optional .45 nnet outputs\n";
    std::cout << "remaining_exact\tfull diagnostic burst_mx is trace-precision for the supplied trace; remaining validation work is broader corpus/platform testing\n";
    std::cout << "next_recommended\tbatch validation, dictionary workflow, and Windows packaging\n";
    return 0;
}


static int cmdCompareGarbageMedianTrace(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const BackendTraceData d = readBackendTraceData(dump);
    struct Item { const char* label; const Matrix* orig; const Matrix* expected; };
    const std::vector<Item> items = {
        {"nnet/bc_m_median_out", &d.bcMOrig, &d.bcMMedian},
        {"nnet/bc_p_median_out", &d.bcPOrig, &d.bcPMedian},
        {"nnet/bc_h_median_out", &d.bcHOrig, &d.bcHMedian},
        {"nnet/trans_m_median_out", &d.transMOrig, &d.transMMedian},
        {"nnet/trans_p_median_out", &d.transPOrig, &d.transPMedian},
        {"nnet/trans_h_median_out", &d.transHOrig, &d.transHMedian}
    };
    for (const auto& item : items) {
        const Matrix actual = appendGarbageMedianColumn(*item.orig, 20);
        printMatrixCompare(item.label, compareMatrixValues(actual, *item.expected), 0.0, 0.0);
        std::cout << "\n";
    }
    std::cout << "formula\tcopy original columns; append zero-based rank min(N,cols)/2 after descending row sort; N=20\n";
    return 0;
}

static int cmdComposeGarbageMedian(const std::vector<std::string>& args) {
    if (args.size() < 3 || args.size() > 4) {
        usage();
        return 2;
    }
    const fs::path inPath = args[1];
    const fs::path outPath = args[2];
    const int n = (args.size() == 4) ? std::stoi(args[3]) : 20;
    const Matrix orig = readNumericTsv(inPath);
    const Matrix out = appendGarbageMedianColumn(orig, n);
    writeNumericTsv(out, outPath);
    std::cout << "input\t" << inPath.string() << "\n";
    std::cout << "output\t" << outPath.string() << "\n";
    std::cout << "N\t" << n << "\n";
    std::cout << "rows\t" << out.rows << "\n";
    std::cout << "cols\t" << out.cols << "\n";
    return 0;
}

static int cmdComparePhnfeatTrace(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const BackendTraceData d = readBackendTraceData(dump);
    const BackendProbabilities actualFromMedian = composeBackendProbabilitiesFromMedian(d);
    const BackendProbabilities actualFromOrig = composeBackendProbabilitiesFromOrig(d, 20);
    const Matrix expectedPhn = readDumpMatrix(dump, "prob/phnProb");
    const Matrix expectedTrans = readDumpMatrix(dump, "prob/transProb");

    printMatrixCompare("prob/phnProb.from_median", compareMatrixValues(actualFromMedian.phnProb, expectedPhn), 1e-7, 1e-5);
    std::cout << "\n";
    printMatrixCompare("prob/transProb.from_median", compareMatrixValues(actualFromMedian.transProb, expectedTrans), 1e-7, 1e-5);
    std::cout << "\n";
    printMatrixCompare("prob/phnProb.from_orig_plus_garbage", compareMatrixValues(actualFromOrig.phnProb, expectedPhn), 1e-7, 1e-5);
    std::cout << "\n";
    printMatrixCompare("prob/transProb.from_orig_plus_garbage", compareMatrixValues(actualFromOrig.transProb, expectedTrans), 1e-7, 1e-5);
    std::cout << "formula_phn\trow-normalize feature matrices over mapped states; clamp to [0.0001,0.9999]; dependent states average M/P/H; nondependent states exponent-adjust around uniform category base then multiply; renormalize nondependent mass to 1-dependent_mass, cap dependent_mass at 0.95\n";
    std::cout << "formula_trans\tclamp; exponent-adjust around uniform category base; multiply M/P/H; normalize each row\n";
    return 0;
}

static int cmdComposeBackendProbFromTrace(const std::vector<std::string>& args) {
    if (args.size() < 3 || args.size() > 4) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const fs::path outDir = args[2];
    const std::string mode = (args.size() == 4) ? args[3] : "orig";
    const BackendTraceData d = readBackendTraceData(dump);
    BackendProbabilities p;
    if (mode == "orig") {
        p = composeBackendProbabilitiesFromOrig(d, 20);
    } else if (mode == "median") {
        p = composeBackendProbabilitiesFromMedian(d);
    } else {
        throw std::runtime_error("mode must be orig or median");
    }
    writeBackendProbabilities(p, outDir);
    std::cout << "out_dir\t" << outDir.string() << "\n";
    std::cout << "mode\t" << mode << "\n";
    std::cout << "phnProb_rows\t" << p.phnProb.rows << "\n";
    std::cout << "phnProb_cols\t" << p.phnProb.cols << "\n";
    std::cout << "transProb_rows\t" << p.transProb.rows << "\n";
    std::cout << "transProb_cols\t" << p.transProb.cols << "\n";
    return 0;
}

static int cmdInspectBackendGraph(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const TraceGraph g = readTraceGraph(dump);
    std::size_t selfLoops = 0;
    std::size_t arcsWithIndex = 0;
    int maxTrans = -1;
    for (const auto& row : g.stateToFromIdx) {
        if (row.size() >= 3) {
            ++arcsWithIndex;
            if (row[0] == row[1]) ++selfLoops;
            maxTrans = std::max(maxTrans, row[2]);
        }
    }
    std::cout << "states_initial\t" << g.statesInitial.size() << "\n";
    std::cout << "unique_states\t" << g.uniqueStates.size() << "\n";
    std::cout << "phn_to_from_rows\t" << g.phnToFrom.size() << "\n";
    std::cout << "state_to_from_rows\t" << g.stateToFrom.size() << "\n";
    std::cout << "state_to_from_idx_rows\t" << g.stateToFromIdx.size() << "\n";
    std::cout << "arcs_with_index\t" << arcsWithIndex << "\n";
    std::cout << "self_loops\t" << selfLoops << "\n";
    std::cout << "transition_columns_inferred\t" << (maxTrans + 1) << "\n";
    if (!g.statesInitial.empty()) {
        std::cout << "first_state\t" << g.statesInitial.front() << "\n";
        std::cout << "last_state\t" << g.statesInitial.back() << "\n";
    }
    return 0;
}

static int cmdWritePhnFromTraceAnswer(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const fs::path outPath = args[2];
    const PhnFile phn = phnFromTraceLabelList(dump / "viterbi" / "phn_labels_after_undo.list.tsv");
    writePhn(outPath, phn);
    std::cout << "output\t" << outPath.string() << "\n";
    std::cout << "intervals\t" << phn.intervals.size() << "\n";
    if (!phn.intervals.empty()) {
        std::cout << "duration_ms\t" << phn.intervals.back().endMs << "\n";
        std::cout << "first\t" << phn.intervals.front().startMs << '\t' << phn.intervals.front().endMs << '\t' << phn.intervals.front().label << "\n";
        std::cout << "last\t" << phn.intervals.back().startMs << '\t' << phn.intervals.back().endMs << '\t' << phn.intervals.back().label << "\n";
    }
    return 0;
}

static int cmdAlignViterbiTrace(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const fs::path outPath = args[2];
    const Matrix phnProb = readDumpMatrix(dump, "prob/phnProb");
    const Matrix transProb = readDumpMatrix(dump, "prob/transProb");
    const ViterbiOptions options = readViterbiOptionsFromTrace(dump);
    const ViterbiAlignment alignment = csluViterbiAlign(readTraceGraph(dump), phnProb, transProb, options);
    writePhn(outPath, alignment.phn);
    std::cout << std::setprecision(17);
    std::cout << "output\t" << outPath.string() << "\n";
    printViterbiOptions(options);
    printPhnSummary(alignment.phn);
    std::cout << "state_segments\t" << alignment.stateSegments.size() << "\n";
    std::cout << "path_score_recomputed\t" << alignment.pathScore << "\n";
    std::cout << "verdict\tWROTE_PHN\n";
    return 0;
}

static int cmdCompareViterbiTrace(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const fs::path outPath = args[2];
    const Matrix phnProb = readDumpMatrix(dump, "prob/phnProb");
    const Matrix transProb = readDumpMatrix(dump, "prob/transProb");
    const ViterbiOptions options = readViterbiOptionsFromTrace(dump);
    const ViterbiAlignment alignment = csluViterbiAlign(readTraceGraph(dump), phnProb, transProb, options);
    writePhn(outPath, alignment.phn);
    const PhnFile expected = phnFromTraceLabelList(dump / "viterbi" / "phn_labels_after_undo.list.tsv");
    std::string reason;
    const bool match = phnFilesEqual(alignment.phn, expected, &reason);
    std::cout << std::setprecision(17);
    std::cout << "output\t" << outPath.string() << "\n";
    printViterbiOptions(options);
    printPhnSummary(alignment.phn);
    std::cout << "state_segments\t" << alignment.stateSegments.size() << "\n";
    std::cout << "path_score_recomputed\t" << alignment.pathScore << "\n";
    std::cout << "match_trace_phn\t" << (match ? "yes" : "no") << "\n";
    if (!match) {
        std::cout << "mismatch_reason\t" << reason << "\n";
    }
    std::cout << "verdict\t" << (match ? "PASS" : "CHECK") << "\n";
    std::cout << "note\tThe phone-boundary path is validated here. CSLU's displayed answer score includes additional internal accounting and is not used for KOTOR .phn generation.\n";
    return match ? 0 : 1;
}

static int cmdAlignViterbiBackend(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const fs::path backendDir = args[1];
    const fs::path outPath = args[2];
    const Matrix phnProb = readNumericTsv(backendDir / "prob" / "phnProb.tsv");
    const Matrix transProb = readNumericTsv(backendDir / "prob" / "transProb.tsv");
    const ViterbiAlignment alignment = csluViterbiAlign(readTraceGraph(backendDir), phnProb, transProb);
    writePhn(outPath, alignment.phn);
    std::cout << std::setprecision(17);
    std::cout << "output\t" << outPath.string() << "\n";
    printPhnSummary(alignment.phn);
    std::cout << "state_segments\t" << alignment.stateSegments.size() << "\n";
    std::cout << "path_score_recomputed\t" << alignment.pathScore << "\n";
    std::cout << "verdict\tWROTE_PHN\n";
    return 0;
}

static int cmdAlignBackendFromNtpTrace(const std::vector<std::string>& args) {
    if (args.size() < 4 || args.size() > 5) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const fs::path faRoot = args[2];
    const fs::path outPath = args[3];
    const std::string mode = (args.size() == 5) ? args[4] : "orig";

    const GeneratedGraphBundle bundle = buildGraphBundleFromNtpText(readTextFile(dump / "text" / "ntp_original.txt"), faRoot);
    BackendTraceData data = applyGeneratedMappingsToTraceNnets(readBackendTraceData(dump), bundle.mappings);
    BackendProbabilities probs;
    if (mode == "orig") {
        probs = composeBackendProbabilitiesFromOrig(data, 20);
    } else if (mode == "median") {
        probs = composeBackendProbabilitiesFromMedian(data);
    } else {
        throw std::runtime_error("mode must be orig or median");
    }

    const ViterbiOptions options = readViterbiOptionsFromTrace(dump);
    const ViterbiAlignment alignment = csluViterbiAlign(bundle.graph, probs.phnProb, probs.transProb, options);
    writePhn(outPath, alignment.phn);
    const PhnFile expected = phnFromTraceLabelList(dump / "viterbi" / "phn_labels_after_undo.list.tsv");
    std::string reason;
    const bool match = phnFilesEqual(alignment.phn, expected, &reason);
    std::cout << std::setprecision(17);
    std::cout << "output\t" << outPath.string() << "\n";
    std::cout << "mode\t" << mode << "\n";
    printViterbiOptions(options);
    std::cout << "phones\t" << bundle.phones.size() << "\n";
    std::cout << "unique_states\t" << bundle.graph.uniqueStates.size() << "\n";
    std::cout << "transition_columns\t" << bundle.graph.phnToFrom.size() << "\n";
    printPhnSummary(alignment.phn);
    std::cout << "state_segments\t" << alignment.stateSegments.size() << "\n";
    std::cout << "path_score_recomputed\t" << alignment.pathScore << "\n";
    std::cout << "match_trace_phn\t" << (match ? "yes" : "no") << "\n";
    if (!match) {
        std::cout << "mismatch_reason\t" << reason << "\n";
    }
    std::cout << "verdict\t" << (match ? "PASS" : "CHECK") << "\n";
    return match ? 0 : 1;
}

static int cmdSimpleViterbiTrace(const std::vector<std::string>& args) {
    return cmdAlignViterbiTrace(args);
}


static std::vector<std::string> indexListsToStrings(const IndexLists& lists) {
    std::vector<std::string> out;
    out.reserve(lists.size());
    for (const auto& row : lists) {
        std::string s;
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i) s.push_back(' ');
            s += std::to_string(row[i]);
        }
        out.push_back(s);
    }
    return out;
}

static std::vector<std::string> intsToStrings(const std::vector<int>& values) {
    std::vector<std::string> out;
    out.reserve(values.size());
    for (int v : values) out.push_back(std::to_string(v));
    return out;
}

static void writeListTsvLocal(const std::vector<std::string>& values, const fs::path& path) {
    ensureDirectory(path.parent_path());
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write list TSV: " + path.string());
    out << "index\tvalue\n";
    for (std::size_t i = 0; i < values.size(); ++i) {
        out << i << '\t' << values[i] << '\n';
    }
}

static std::map<std::string, int> multisetCounts(const std::vector<std::string>& v) {
    std::map<std::string, int> c;
    for (const auto& x : v) ++c[x];
    return c;
}

static int multisetDifferenceCount(const std::vector<std::string>& actual, const std::vector<std::string>& expected) {
    auto a = multisetCounts(actual);
    auto e = multisetCounts(expected);
    int diff = 0;
    for (const auto& kv : e) {
        const int av = a[kv.first];
        if (av < kv.second) diff += (kv.second - av);
    }
    for (const auto& kv : a) {
        const int ev = e[kv.first];
        if (ev < kv.second) diff += (kv.second - ev);
    }
    return diff;
}

static void printListSemanticCompare(const std::string& label,
                                     const std::vector<std::string>& actual,
                                     const std::vector<std::string>& expected) {
    const bool exact = (actual == expected);
    const int diff = multisetDifferenceCount(actual, expected);
    std::cout << label << "_actual_count\t" << actual.size() << "\n";
    std::cout << label << "_expected_count\t" << expected.size() << "\n";
    std::cout << label << "_exact_order\t" << (exact ? "yes" : "no") << "\n";
    std::cout << label << "_multiset_diff\t" << diff << "\n";
}

static int compareMappingByState(const std::string& label,
                                 const std::vector<std::string>& actualStates,
                                 const std::vector<std::string>& actualRows,
                                 const std::vector<std::string>& expectedStates,
                                 const std::vector<std::string>& expectedRows) {
    std::map<std::string, std::string> a;
    std::map<std::string, std::string> e;
    for (std::size_t i = 0; i < actualStates.size() && i < actualRows.size(); ++i) a[actualStates[i]] = actualRows[i];
    for (std::size_t i = 0; i < expectedStates.size() && i < expectedRows.size(); ++i) e[expectedStates[i]] = expectedRows[i];
    int mismatches = 0;
    for (const auto& kv : e) {
        auto it = a.find(kv.first);
        if (it == a.end() || it->second != kv.second) ++mismatches;
    }
    for (const auto& kv : a) {
        if (e.find(kv.first) == e.end()) ++mismatches;
    }
    std::cout << label << "_state_keyed_mismatches\t" << mismatches << "\n";
    return mismatches;
}

static int compareMappingByTransitionMultiset(const std::string& label,
                                              const std::vector<std::string>& actualTransitions,
                                              const std::vector<std::string>& actualRows,
                                              const std::vector<std::string>& expectedTransitions,
                                              const std::vector<std::string>& expectedRows) {
    std::vector<std::string> a;
    std::vector<std::string> e;
    for (std::size_t i = 0; i < actualTransitions.size() && i < actualRows.size(); ++i) {
        a.push_back(actualTransitions[i] + "\t" + actualRows[i]);
    }
    for (std::size_t i = 0; i < expectedTransitions.size() && i < expectedRows.size(); ++i) {
        e.push_back(expectedTransitions[i] + "\t" + expectedRows[i]);
    }
    const int diff = multisetDifferenceCount(a, e);
    std::cout << label << "_transition_keyed_multiset_diff\t" << diff << "\n";
    return diff;
}

static Matrix remapMatrixColumnsByLabels(const Matrix& src,
                                         const std::vector<std::string>& srcLabels,
                                         const std::vector<std::string>& dstLabels) {
    if (src.cols != srcLabels.size()) {
        throw std::runtime_error("source label count does not match matrix columns");
    }
    Matrix out;
    out.rows = src.rows;
    out.cols = dstLabels.size();
    out.data.assign(out.rows * out.cols, 0.0);
    std::vector<unsigned char> used(srcLabels.size(), 0);
    for (std::size_t dc = 0; dc < dstLabels.size(); ++dc) {
        std::size_t sc = srcLabels.size();
        for (std::size_t i = 0; i < srcLabels.size(); ++i) {
            if (!used[i] && srcLabels[i] == dstLabels[dc]) {
                sc = i;
                used[i] = 1;
                break;
            }
        }
        if (sc == srcLabels.size()) {
            throw std::runtime_error("cannot remap matrix column label: " + dstLabels[dc]);
        }
        for (std::size_t r = 0; r < src.rows; ++r) {
            out(r, dc) = src(r, sc);
        }
    }
    return out;
}

static int cmdComposeGraphFromNtp(const std::vector<std::string>& args) {
    if (args.size() != 4) {
        usage();
        return 2;
    }
    const fs::path ntp = args[1];
    const fs::path faRoot = args[2];
    const fs::path outDir = args[3];
    const GeneratedGraphBundle bundle = buildGraphBundleFromNtpFile(ntp, faRoot);
    writeGraphBundle(bundle, outDir, faRoot);
    std::cout << "output\t" << outDir.string() << "\n";
    std::cout << "phones\t" << bundle.phones.size() << "\n";
    std::cout << "states_initial\t" << bundle.graph.statesInitial.size() << "\n";
    std::cout << "unique_states\t" << bundle.graph.uniqueStates.size() << "\n";
    std::cout << "phn_to_from\t" << bundle.graph.phnToFrom.size() << "\n";
    std::cout << "state_to_from\t" << bundle.graph.stateToFrom.size() << "\n";
    std::cout << "note\tPhase 15 graph builder supports optional brackets and alternatives; phones is a diagnostic first-path count.\n";
    return 0;
}

static int cmdCompareGraphFromNtpTrace(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const fs::path faRoot = args[2];
    const std::string ntp = readTextFile(dump / "text" / "ntp_original.txt");
    const GeneratedGraphBundle bundle = buildGraphBundleFromNtpText(ntp, faRoot);
    const TraceGraph expectedGraph = readTraceGraph(dump);

    std::cout << "phones\t" << bundle.phones.size() << "\n";
    printListSemanticCompare("graph_states_initial", bundle.graph.statesInitial, expectedGraph.statesInitial);
    printListSemanticCompare("graph_unique_states", bundle.graph.uniqueStates, expectedGraph.uniqueStates);
    printListSemanticCompare("graph_phn_to_from", bundle.graph.phnToFrom, expectedGraph.phnToFrom);
    printListSemanticCompare("graph_state_to_from", bundle.graph.stateToFrom, expectedGraph.stateToFrom);

    int semanticErrors = 0;
    semanticErrors += compareMappingByState("feat_states_idx_m", bundle.graph.uniqueStates, indexListsToStrings(bundle.mappings.featStatesM),
                                            expectedGraph.uniqueStates, readIndexValueList(dump / "mapping" / "feat_states_idx_m.list.tsv"));
    semanticErrors += compareMappingByState("feat_states_idx_p", bundle.graph.uniqueStates, indexListsToStrings(bundle.mappings.featStatesP),
                                            expectedGraph.uniqueStates, readIndexValueList(dump / "mapping" / "feat_states_idx_p.list.tsv"));
    semanticErrors += compareMappingByState("feat_states_idx_h", bundle.graph.uniqueStates, indexListsToStrings(bundle.mappings.featStatesH),
                                            expectedGraph.uniqueStates, readIndexValueList(dump / "mapping" / "feat_states_idx_h.list.tsv"));
    semanticErrors += compareMappingByState("dep_list", bundle.graph.uniqueStates, intsToStrings(bundle.mappings.depList),
                                            expectedGraph.uniqueStates, readIndexValueList(dump / "mapping" / "dep_list.list.tsv"));
    semanticErrors += compareMappingByState("vc_list", bundle.graph.uniqueStates, intsToStrings(bundle.mappings.vcList),
                                            expectedGraph.uniqueStates, readIndexValueList(dump / "mapping" / "vc_list.list.tsv"));
    semanticErrors += compareMappingByTransitionMultiset("trans_states_m", bundle.graph.phnToFrom, indexListsToStrings(bundle.mappings.transStatesM),
                                                         expectedGraph.phnToFrom, readIndexValueList(dump / "mapping" / "trans_states_m.list.tsv"));
    semanticErrors += compareMappingByTransitionMultiset("trans_states_p", bundle.graph.phnToFrom, indexListsToStrings(bundle.mappings.transStatesP),
                                                         expectedGraph.phnToFrom, readIndexValueList(dump / "mapping" / "trans_states_p.list.tsv"));
    semanticErrors += compareMappingByTransitionMultiset("trans_states_h", bundle.graph.phnToFrom, indexListsToStrings(bundle.mappings.transStatesH),
                                                         expectedGraph.phnToFrom, readIndexValueList(dump / "mapping" / "trans_states_h.list.tsv"));

    BackendTraceData data = applyGeneratedMappingsToTraceNnets(readBackendTraceData(dump), bundle.mappings);
    const BackendProbabilities probs = composeBackendProbabilitiesFromOrig(data, 20);
    const Matrix expectedPhn = readDumpMatrix(dump, "prob/phnProb");
    const Matrix expectedTrans = readDumpMatrix(dump, "prob/transProb");
    const Matrix phnRemap = remapMatrixColumnsByLabels(probs.phnProb, bundle.graph.uniqueStates, expectedGraph.uniqueStates);
    const Matrix transRemap = remapMatrixColumnsByLabels(probs.transProb, bundle.graph.phnToFrom, expectedGraph.phnToFrom);
    printMatrixCompare("prob/phnProb.from_ntp_graph.remapped_to_trace_order", compareMatrixValues(phnRemap, expectedPhn), 1e-7, 1e-5);
    std::cout << "\n";
    printMatrixCompare("prob/transProb.from_ntp_graph.remapped_to_trace_order", compareMatrixValues(transRemap, expectedTrans), 1e-7, 1e-5);

    std::cout << "semantic_mapping_errors\t" << semanticErrors << "\n";
    std::cout << "verdict\t" << (semanticErrors == 0 ? "PASS" : "CHECK") << "\n";
    std::cout << "note\tGenerated node order is chronological and intentionally differs from CSLU statenet's internal allocation order; graph/mapping/probabilities match semantically after label remapping.\n";
    return 0;
}

static int cmdComposeBackendFromNtpTrace(const std::vector<std::string>& args) {
    if (args.size() < 4 || args.size() > 5) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const fs::path faRoot = args[2];
    const fs::path outDir = args[3];
    const std::string mode = (args.size() == 5) ? args[4] : "orig";
    const GeneratedGraphBundle bundle = buildGraphBundleFromNtpText(readTextFile(dump / "text" / "ntp_original.txt"), faRoot);
    writeGraphBundle(bundle, outDir, faRoot);

    BackendTraceData data = applyGeneratedMappingsToTraceNnets(readBackendTraceData(dump), bundle.mappings);
    BackendProbabilities probs;
    if (mode == "orig") {
        probs = composeBackendProbabilitiesFromOrig(data, 20);
    } else if (mode == "median") {
        probs = composeBackendProbabilitiesFromMedian(data);
    } else {
        throw std::runtime_error("mode must be orig or median");
    }
    writeBackendProbabilities(probs, outDir);
    writeListTsvLocal(bundle.graph.uniqueStates, outDir / "prob" / "phnProb.labels.tsv");
    writeListTsvLocal(bundle.graph.phnToFrom, outDir / "prob" / "transProb.labels.tsv");

    std::cout << "out_dir\t" << outDir.string() << "\n";
    std::cout << "mode\t" << mode << "\n";
    std::cout << "phones\t" << bundle.phones.size() << "\n";
    std::cout << "unique_states\t" << bundle.graph.uniqueStates.size() << "\n";
    std::cout << "transition_columns\t" << bundle.graph.phnToFrom.size() << "\n";
    std::cout << "phnProb\t" << probs.phnProb.rows << "x" << probs.phnProb.cols << "\n";
    std::cout << "transProb\t" << probs.transProb.rows << "x" << probs.transProb.cols << "\n";
    return 0;
}


struct EndToEndAlignmentResult {
    FullFrontendFeatures frontend;
    BackendTraceData backendData;
    GeneratedGraphBundle graphBundle;
    BackendProbabilities probabilities;
    ViterbiAlignment alignment;
};

static Matrix runOneCsluModel(const fs::path& modelPath, const Matrix& netin) {
    if (!fs::exists(modelPath)) {
        throw std::runtime_error("missing model: " + modelPath.string());
    }
    const NNet45 net = NNet45::load(modelPath);
    return net.forwardMatrix(netin);
}

struct LoadedCsluModelSet {
    fs::path root;
    NNet45 bcM;
    NNet45 bcP;
    NNet45 bcH;
    NNet45 transM;
    NNet45 transP;
    NNet45 transH;

    static LoadedCsluModelSet load(const fs::path& faRoot) {
        LoadedCsluModelSet m;
        m.root = faRoot;
        auto loadOne = [&](const char* rel) {
            const fs::path p = faRoot / rel;
            if (!fs::exists(p)) throw std::runtime_error("missing model: " + p.string());
            return NNet45::load(p);
        };
        m.bcM = loadOne("bc_16k/manner_nnet.45");
        m.bcP = loadOne("bc_16k/place_nnet.45");
        m.bcH = loadOne("bc_16k/height_nnet.45");
        m.transM = loadOne("trans_16k/manner_nnet.45");
        m.transP = loadOne("trans_16k/place_nnet.45");
        m.transH = loadOne("trans_16k/height_nnet.45");
        return m;
    }
};

static BackendTraceData fillBackendDataMappings(const GeneratedMappings& mappings) {
    BackendTraceData d;
    d.featStatesM = mappings.featStatesM;
    d.featStatesP = mappings.featStatesP;
    d.featStatesH = mappings.featStatesH;
    d.depList = mappings.depList;
    d.vcList = mappings.vcList;
    d.transStatesM = mappings.transStatesM;
    d.transStatesP = mappings.transStatesP;
    d.transStatesH = mappings.transStatesH;
    return d;
}

static BackendTraceData buildBackendDataFromFrontendNnets(const fs::path& faRoot,
                                                          const Matrix& netin,
                                                          const GeneratedMappings& mappings) {
    BackendTraceData d = fillBackendDataMappings(mappings);
    d.bcMOrig = runOneCsluModel(faRoot / "bc_16k" / "manner_nnet.45", netin);
    d.bcPOrig = runOneCsluModel(faRoot / "bc_16k" / "place_nnet.45", netin);
    d.bcHOrig = runOneCsluModel(faRoot / "bc_16k" / "height_nnet.45", netin);
    d.transMOrig = runOneCsluModel(faRoot / "trans_16k" / "manner_nnet.45", netin);
    d.transPOrig = runOneCsluModel(faRoot / "trans_16k" / "place_nnet.45", netin);
    d.transHOrig = runOneCsluModel(faRoot / "trans_16k" / "height_nnet.45", netin);
    return d;
}

static BackendTraceData buildBackendDataFromFrontendNnets(const LoadedCsluModelSet& models,
                                                          const Matrix& netin,
                                                          const GeneratedMappings& mappings) {
    BackendTraceData d = fillBackendDataMappings(mappings);
    d.bcMOrig = models.bcM.forwardMatrix(netin);
    d.bcPOrig = models.bcP.forwardMatrix(netin);
    d.bcHOrig = models.bcH.forwardMatrix(netin);
    d.transMOrig = models.transM.forwardMatrix(netin);
    d.transPOrig = models.transP.forwardMatrix(netin);
    d.transHOrig = models.transH.forwardMatrix(netin);
    return d;
}

static void writeBackendNnetDebug(const BackendTraceData& d, const fs::path& outDir) {
    ensureDirectory(outDir / "nnet");
    writeNumericTsv(d.bcMOrig, outDir / "nnet" / "bc_m_orig_out.tsv");
    writeNumericTsv(d.bcPOrig, outDir / "nnet" / "bc_p_orig_out.tsv");
    writeNumericTsv(d.bcHOrig, outDir / "nnet" / "bc_h_orig_out.tsv");
    writeNumericTsv(d.transMOrig, outDir / "nnet" / "trans_m_orig_out.tsv");
    writeNumericTsv(d.transPOrig, outDir / "nnet" / "trans_p_orig_out.tsv");
    writeNumericTsv(d.transHOrig, outDir / "nnet" / "trans_h_orig_out.tsv");

    writeNumericTsv(appendGarbageMedianColumn(d.bcMOrig, 20), outDir / "nnet" / "bc_m_median_out.tsv");
    writeNumericTsv(appendGarbageMedianColumn(d.bcPOrig, 20), outDir / "nnet" / "bc_p_median_out.tsv");
    writeNumericTsv(appendGarbageMedianColumn(d.bcHOrig, 20), outDir / "nnet" / "bc_h_median_out.tsv");
    writeNumericTsv(appendGarbageMedianColumn(d.transMOrig, 20), outDir / "nnet" / "trans_m_median_out.tsv");
    writeNumericTsv(appendGarbageMedianColumn(d.transPOrig, 20), outDir / "nnet" / "trans_p_median_out.tsv");
    writeNumericTsv(appendGarbageMedianColumn(d.transHOrig, 20), outDir / "nnet" / "trans_h_median_out.tsv");
}

static EndToEndAlignmentResult runEndToEndFromFrontend(const FullFrontendFeatures& frontend,
                                                       const fs::path& ntpPath,
                                                       const fs::path& faRoot,
                                                       const ViterbiOptions& options = {}) {
    EndToEndAlignmentResult r;
    r.frontend = frontend;
    r.graphBundle = buildGraphBundleFromNtpFile(ntpPath, faRoot);
    r.backendData = buildBackendDataFromFrontendNnets(faRoot, r.frontend.netin, r.graphBundle.mappings);
    r.probabilities = composeBackendProbabilitiesFromOrig(r.backendData, 20);
    r.alignment = csluViterbiAlign(r.graphBundle.graph, r.probabilities.phnProb, r.probabilities.transProb, options);
    return r;
}

static EndToEndAlignmentResult runEndToEndFromFrontend(const FullFrontendFeatures& frontend,
                                                       const fs::path& ntpPath,
                                                       const fs::path& faRoot,
                                                       const LoadedCsluModelSet& models,
                                                       const ViterbiOptions& options = {}) {
    EndToEndAlignmentResult r;
    r.frontend = frontend;
    r.graphBundle = buildGraphBundleFromNtpFile(ntpPath, faRoot);
    r.backendData = buildBackendDataFromFrontendNnets(models, r.frontend.netin, r.graphBundle.mappings);
    r.probabilities = composeBackendProbabilitiesFromOrig(r.backendData, 20);
    r.alignment = csluViterbiAlign(r.graphBundle.graph, r.probabilities.phnProb, r.probabilities.transProb, options);
    return r;
}

static void writeEndToEndWorkDir(const EndToEndAlignmentResult& r,
                                 const fs::path& outDir,
                                 const fs::path& faRoot,
                                 const fs::path& phnPath) {
    if (outDir.empty()) {
        return;
    }
    writeFullFrontendFeatures(r.frontend, outDir);
    writeBackendNnetDebug(r.backendData, outDir);
    writeGraphBundle(r.graphBundle, outDir, faRoot);
    writeBackendProbabilities(r.probabilities, outDir);
    writeListTsvLocal(r.graphBundle.graph.uniqueStates, outDir / "prob" / "phnProb.labels.tsv");
    writeListTsvLocal(r.graphBundle.graph.phnToFrom, outDir / "prob" / "transProb.labels.tsv");
    ensureDirectory(outDir / "output");
    writePhn(outDir / "output" / "phn.txt", r.alignment.phn);
    if (!phnPath.empty()) {
        std::ofstream manifest(outDir / "phase13_manifest.tsv");
        manifest << "key\tvalue\n";
        manifest << "phase\t13_sampleconvert_end_to_end\n";
        manifest << "fa_root\t" << faRoot.string() << "\n";
        manifest << "output_phn\t" << phnPath.string() << "\n";
        manifest << "frames\t" << r.frontend.netin.rows << "\n";
        manifest << "unique_states\t" << r.graphBundle.graph.uniqueStates.size() << "\n";
        manifest << "transition_columns\t" << r.graphBundle.graph.phnToFrom.size() << "\n";
        manifest << "phn_intervals\t" << r.alignment.phn.intervals.size() << "\n";
    }
}

static void printEndToEndSummary(const EndToEndAlignmentResult& r,
                                 const fs::path& outPath,
                                 const fs::path& workDir,
                                 const char* inputMode) {
    std::cout << std::setprecision(17);
    std::cout << "phase\t13_sampleconvert_end_to_end\n";
    std::cout << "input_mode\t" << inputMode << "\n";
    std::cout << "output\t" << outPath.string() << "\n";
    if (!workDir.empty()) {
        std::cout << "work_dir\t" << workDir.string() << "\n";
    }
    std::cout << "frontend_frames\t" << r.frontend.netin.rows << "\n";
    std::cout << "frontend_netin_cols\t" << r.frontend.netin.cols << "\n";
    std::cout << "phones_from_ntp\t" << r.graphBundle.phones.size() << "\n";
    std::cout << "unique_states\t" << r.graphBundle.graph.uniqueStates.size() << "\n";
    std::cout << "transition_columns\t" << r.graphBundle.graph.phnToFrom.size() << "\n";
    std::cout << "phnProb\t" << r.probabilities.phnProb.rows << "x" << r.probabilities.phnProb.cols << "\n";
    std::cout << "transProb\t" << r.probabilities.transProb.rows << "x" << r.probabilities.transProb.cols << "\n";
    printPhnSummary(r.alignment.phn);
    std::cout << "state_segments\t" << r.alignment.stateSegments.size() << "\n";
    std::cout << "path_score_recomputed\t" << r.alignment.pathScore << "\n";
}



// Phase 21 standalone-layout helpers are defined below the end-to-end alignment commands.
static bool isFlagToken(const std::string& s);
static fs::path resolveModelRoot(const fs::path& rootOrModels);
static bool hasFlag(const std::vector<std::string>& args, const std::string& flag);
static fs::path readModelsFlag(const std::vector<std::string>& args, std::size_t start);
static bool consumeModelOrWorkFlag(const std::vector<std::string>& args, std::size_t& i, fs::path& modelRoot, fs::path& workDir);
static void requireModelRoot(const fs::path& modelRoot);

static int cmdAlignPreDcNtp(const std::vector<std::string>& args) {
    if (args.size() < 6) {
        usage();
        return 2;
    }
    const fs::path wav16 = args[1];
    const fs::path wav8 = args[2];
    const fs::path ntp = args[3];
    const bool newStyle = hasFlag(args, "--models") || hasFlag(args, "--model-root") || hasFlag(args, "--package");
    fs::path modelRoot;
    fs::path outPath;
    fs::path workDir;
    std::size_t flagStart;
    if (newStyle) {
        outPath = args[4];
        flagStart = 5;
        modelRoot = readModelsFlag(args, flagStart);
    } else {
        modelRoot = resolveModelRoot(args[4]);
        outPath = args[5];
        flagStart = 6;
        if (flagStart < args.size() && !isFlagToken(args[flagStart])) workDir = args[flagStart++];
    }
    if (modelRoot.empty()) throw std::runtime_error("align-predc-ntp requires --models <models> in standalone syntax");
    for (std::size_t i = flagStart; i < args.size(); ++i) {
        if (consumeModelOrWorkFlag(args, i, modelRoot, workDir)) continue;
        if (newStyle && !isFlagToken(args[i]) && workDir.empty()) { workDir = args[i]; continue; }
        throw std::runtime_error("unknown align-predc-ntp option: " + args[i]);
    }
    requireModelRoot(modelRoot);

    FullFrontendFeatures frontend = computeFullFrontendFromPreDcWavs(readPcm16Mono(wav16), readPcm16Mono(wav8));
    EndToEndAlignmentResult r = runEndToEndFromFrontend(frontend, ntp, modelRoot);
    writePhn(outPath, r.alignment.phn);
    writeEndToEndWorkDir(r, workDir, modelRoot, outPath);
    printEndToEndSummary(r, outPath, workDir, "pre_dc_16k_8k");
    std::cout << "models_root\t" << modelRoot.string() << "\n";
    std::cout << "verdict\tWROTE_PHN\n";
    return 0;
}

static int cmdAlignWavNtp(const std::vector<std::string>& args) {
    if (args.size() < 5) {
        usage();
        return 2;
    }
    const fs::path wav = args[1];
    const fs::path ntp = args[2];
    const bool newStyle = hasFlag(args, "--models") || hasFlag(args, "--model-root") || hasFlag(args, "--package");
    fs::path modelRoot;
    fs::path outPath;
    fs::path workDir;
    std::size_t flagStart;
    if (newStyle) {
        outPath = args[3];
        flagStart = 4;
        modelRoot = readModelsFlag(args, flagStart);
    } else {
        modelRoot = resolveModelRoot(args[3]);
        outPath = args[4];
        flagStart = 5;
        if (flagStart < args.size() && !isFlagToken(args[flagStart])) workDir = args[flagStart++];
    }
    if (modelRoot.empty()) throw std::runtime_error("align-wav-ntp requires --models <models> in standalone syntax");
    for (std::size_t i = flagStart; i < args.size(); ++i) {
        if (consumeModelOrWorkFlag(args, i, modelRoot, workDir)) continue;
        if (newStyle && !isFlagToken(args[i]) && workDir.empty()) { workDir = args[i]; continue; }
        throw std::runtime_error("unknown align-wav-ntp option: " + args[i]);
    }
    requireModelRoot(modelRoot);

    const Pcm16Mono original = readPcm16Mono(wav);
    FullFrontendFeatures frontend = computeFullFrontendFromOriginalWav(original);
    EndToEndAlignmentResult r = runEndToEndFromFrontend(frontend, ntp, modelRoot);
    writePhn(outPath, r.alignment.phn);
    writeEndToEndWorkDir(r, workDir, modelRoot, outPath);
    printEndToEndSummary(r, outPath, workDir, "single_wav_wave_dll_sampleconvert");
    std::cout << "models_root\t" << modelRoot.string() << "\n";
    std::cout << "original_rate\t" << original.sampleRate << "\n";
    std::cout << "resampler\twave_dll_sampleconvert_compatible\n";
    std::cout << "note\tSingle-WAV mode uses the recovered wave.dll Wave_sampleConvert-compatible FIR/polyphase resampler.\n";
    std::cout << "verdict\tWROTE_PHN\n";
    return 0;
}

static int cmdAlignWavNtpLip(const std::vector<std::string>& args) {
    if (args.size() < 5) { usage(); return 2; }
    const fs::path wav = args[1];
    const fs::path ntp = args[2];
    const bool newStyle = hasFlag(args, "--models") || hasFlag(args, "--model-root") || hasFlag(args, "--package");
    fs::path modelRoot;
    fs::path lipPath;
    fs::path workDir;
    fs::path phnOut;
    LipTailRestOptions tail;
    LipQualityOptions quality;
    std::size_t flagStart;
    if (newStyle) {
        lipPath = args[3];
        flagStart = 4;
        modelRoot = readModelsFlag(args, flagStart);
    } else {
        modelRoot = resolveModelRoot(args[3]);
        lipPath = args[4];
        flagStart = 5;
        if (flagStart < args.size() && !isFlagToken(args[flagStart])) workDir = args[flagStart++];
    }
    if (modelRoot.empty()) throw std::runtime_error("align-wav-ntp-lip requires --models <models> in standalone syntax");
    for (std::size_t i = flagStart; i < args.size(); ++i) {
        if (consumeModelOrWorkFlag(args, i, modelRoot, workDir)) continue;
        if (consumeLipTailRestFlag(args, i, tail)) continue;
        if (consumeLipQualityFlag(args, i, quality)) continue;
        if (args[i] == "--phn-out" || args[i] == "--phn-output") {
            if (i + 1 >= args.size()) throw std::runtime_error(args[i] + " requires a path");
            phnOut = args[++i];
        } else if (args[i] == "--keep-phn") {
            if (phnOut.empty()) { fs::path p = lipPath; p.replace_extension(".phn"); phnOut = p; }
        } else if (args[i] == "--no-phn") {
            phnOut.clear();
        } else if (newStyle && !isFlagToken(args[i]) && workDir.empty()) {
            workDir = args[i];
        } else {
            throw std::runtime_error("unknown align-wav-ntp-lip option: " + args[i]);
        }
    }
    requireModelRoot(modelRoot);
    if (phnOut.empty() && !workDir.empty()) phnOut = defaultIntermediatePhnForLip(lipPath, workDir);

    const Pcm16Mono original = readPcm16Mono(wav);
    FullFrontendFeatures frontend = computeFullFrontendFromOriginalWav(original);
    EndToEndAlignmentResult r = runEndToEndFromFrontend(frontend, ntp, modelRoot);
    if (!phnOut.empty()) writePhnEnsuringParent(phnOut, r.alignment.phn);
    writeEndToEndWorkDir(r, workDir, modelRoot, phnOut);
    const int alignedDurationMs = r.alignment.phn.intervals.empty() ? 0 : std::max(0, r.alignment.phn.intervals.back().endMs);
    const int tailMs = resolveTailRestMs(alignedDurationMs, tail);
    LipFile lip = writeLipFromPhnToPath(r.alignment.phn, lipPath, tail, quality);
    printEndToEndSummary(r, lipPath, workDir, "single_wav_ntp_to_lip");
    std::cout << "models_root\t" << modelRoot.string() << "\n";
    if (!phnOut.empty()) std::cout << "intermediate_phn\t" << phnOut.string() << "\n";
    printLipSummary(lip, lipPath, tail, tailMs, quality);
    std::cout << "verdict\tWROTE_LIP\n";
    return 0;
}



static std::string normalizeWhitespaceLocal(const std::string& s) {
    std::istringstream in(s);
    std::string tok;
    std::vector<std::string> toks;
    while (in >> tok) toks.push_back(tok);
    std::string out;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        if (i) out.push_back(' ');
        out += toks[i];
    }
    return out;
}

struct TextCommandExtraOptions {
    fs::path sourceReport;
    fs::path unknownReport;
};

static bool isFlagToken(const std::string& s) {
    return s.rfind("--", 0) == 0;
}

static fs::path canonicalIfPossible(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p : c;
}

static fs::path executableDirIfKnown() {
#if defined(_WIN32)
    std::vector<char> buf(MAX_PATH);
    DWORD len = 0;
    for (;;) {
        len = GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) return fs::path();
        if (len < buf.size() - 1) break;
        buf.resize(buf.size() * 2);
    }
    return fs::path(std::string(buf.data(), len)).parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) return fs::path();
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return fs::path();
    return canonicalIfPossible(fs::path(buf.data())).parent_path();
#elif defined(__linux__)
    std::vector<char> buf(PATH_MAX);
    ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len <= 0) return fs::path();
    buf[static_cast<std::size_t>(len)] = '\0';
    return fs::path(buf.data()).parent_path();
#else
    return fs::path();
#endif
}

static fs::path findBundledKotorOverrideDictionary() {
    std::vector<fs::path> candidates;
    const fs::path exeDir = executableDirIfKnown();
    if (!exeDir.empty()) {
        candidates.push_back(exeDir / ".." / "dictionaries" / "kotor_override.dict");
        candidates.push_back(exeDir / "dictionaries" / "kotor_override.dict");
    }
    candidates.push_back(fs::path(CSLU_BUNDLED_DICTIONARY_DIR) / "kotor_override.dict");
    candidates.push_back(fs::current_path() / "dictionaries" / "kotor_override.dict");
    candidates.push_back(fs::current_path() / ".." / "dictionaries" / "kotor_override.dict");
    for (const auto& c : candidates) {
        if (!c.empty() && fs::exists(c) && fs::is_regular_file(c)) return canonicalIfPossible(c);
    }
    return fs::path();
}

static bool looksLikeModelRoot(const fs::path& p) {
    return fs::exists(p / "phonemes_new_16k.train.spec") &&
           fs::exists(p / "phonemes_new_16k.parts") &&
           fs::exists(p / "bc_16k") &&
           fs::exists(p / "trans_16k");
}

static fs::path resolveModelRoot(const fs::path& rootOrModels) {
    if (looksLikeModelRoot(rootOrModels)) return canonicalIfPossible(rootOrModels);
    if (looksLikeModelRoot(rootOrModels / "models")) return canonicalIfPossible(rootOrModels / "models");
    return rootOrModels;
}

static int cmdVersion(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        usage();
        return 2;
    }
    const fs::path bundledDict = findBundledKotorOverrideDictionary();
    std::cout << "app\tNeoLIP\n";
    std::cout << "version\t" << CSLU_APP_VERSION << "\n";
    std::cout << "phase\t" << CSLU_APP_PHASE << "\n";
    std::cout << "cxx_standard\t" << __cplusplus << "\n";
#if defined(_WIN32)
    std::cout << "platform\twindows\n";
#elif defined(__APPLE__)
    std::cout << "platform\tmacos\n";
#elif defined(__linux__)
    std::cout << "platform\tlinux\n";
#else
    std::cout << "platform\tunknown\n";
#endif
#ifdef NDEBUG
    std::cout << "build_type\trelease\n";
#else
    std::cout << "build_type\tdebug_or_unset\n";
#endif
    std::cout << "bundled_dictionary_lookup\tenabled\n";
    std::cout << "bundled_kotor_dictionary\t" << (bundledDict.empty() ? std::string("not_found") : bundledDict.string()) << "\n";
    std::cout << "verdict\tVERSION\n";
    return 0;
}
static bool hasFlag(const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

static fs::path readModelsFlag(const std::vector<std::string>& args, std::size_t start) {
    for (std::size_t i = start; i < args.size(); ++i) {
        if (args[i] == "--models" || args[i] == "--model-root" || args[i] == "--package") {
            if (i + 1 >= args.size()) throw std::runtime_error(args[i] + " requires a path");
            return resolveModelRoot(args[i + 1]);
        }
    }
    return fs::path();
}

static bool consumeModelOrWorkFlag(const std::vector<std::string>& args,
                                   std::size_t& i,
                                   fs::path& modelRoot,
                                   fs::path& workDir) {
    if (args[i] == "--models" || args[i] == "--model-root" || args[i] == "--package") {
        if (i + 1 >= args.size()) throw std::runtime_error(args[i] + " requires a path");
        modelRoot = resolveModelRoot(args[++i]);
        return true;
    }
    if (args[i] == "--work-dir" || args[i] == "--workdir") {
        if (i + 1 >= args.size()) throw std::runtime_error(args[i] + " requires a path");
        workDir = args[++i];
        return true;
    }
    return false;
}



static fs::path inferFaRootFromSource(const fs::path& source) {
    if (looksLikeModelRoot(source)) return canonicalIfPossible(source);
    const std::vector<fs::path> candidates = {
        source / "Toolkit" / "2.0" / "apps" / "fa",
        source / "CSLU Toolkit" / "Toolkit" / "2.0" / "apps" / "fa"
    };
    for (const fs::path& c : candidates) {
        if (looksLikeModelRoot(c)) return canonicalIfPossible(c);
    }
    throw std::runtime_error("could not locate CSLU apps/fa model root under: " + source.string());
}

static fs::path inferCsluInstallRootFromFa(const fs::path& faRoot) {
    fs::path p = canonicalIfPossible(faRoot);
    for (int i = 0; i < 4 && p.has_parent_path(); ++i) p = p.parent_path();
    return p;
}

static void copyFileIfExists(const fs::path& src, const fs::path& dst, bool required = true) {
    if (!fs::exists(src)) {
        if (required) throw std::runtime_error("required asset missing: " + src.string());
        return;
    }
    ensureDirectory(dst.parent_path());
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
}

static bool isIgnoredBundleDirectoryName(const fs::path& namePath) {
    const std::string name = namePath.filename().string();
    return name == "CVS" || name == ".git" || name == ".svn" || name == "__pycache__";
}

static void copyDirectoryRecursiveIfExists(const fs::path& src, const fs::path& dst, bool required = true) {
    if (!fs::exists(src) || !fs::is_directory(src)) {
        if (required) throw std::runtime_error("required directory missing: " + src.string());
        return;
    }
    fs::recursive_directory_iterator it(src);
    const fs::recursive_directory_iterator end;
    for (; it != end; ++it) {
        const fs::path rel = fs::relative(it->path(), src);
        if (it->is_directory()) {
            if (isIgnoredBundleDirectoryName(it->path())) {
                it.disable_recursion_pending();
                continue;
            }
            ensureDirectory(dst / rel);
        } else if (it->is_regular_file()) {
            copyFileIfExists(it->path(), dst / rel, true);
        }
    }
}

static void requireModelRoot(const fs::path& modelRoot) {
    std::vector<fs::path> required = {
        modelRoot / "phonemes_new_16k.train.spec",
        modelRoot / "phonemes_new_16k.parts",
        modelRoot / "bc_16k" / "manner_nnet.45",
        modelRoot / "bc_16k" / "place_nnet.45",
        modelRoot / "bc_16k" / "height_nnet.45",
        modelRoot / "trans_16k" / "manner_nnet.45",
        modelRoot / "trans_16k" / "place_nnet.45",
        modelRoot / "trans_16k" / "height_nnet.45"
    };
    for (const auto& path : required) {
        if (!fs::exists(path)) {
            throw std::runtime_error("model root is missing required file: " + path.string());
        }
    }
}

static void applyStandaloneDictionaryRoot(TextToNtpOptions& opts, const fs::path& dictRoot) {
    opts.customDictionary = dictRoot / "custom_dictionary.txt";
    opts.cmuDictionaryDir = dictRoot / "cmu";
    opts.ogiLexiconPath = dictRoot / "ogi_lex.out";
    opts.ogiAddendaPath = dictRoot / "ogi_lex_addenda.scm";
}

static TextToNtpOptions retargetTextOptionsPreservingFlags(const TextToNtpOptions& current,
                                                             const fs::path& modelRoot) {
    TextToNtpOptions fresh = makeDefaultTextToNtpOptions(modelRoot);
    fresh.mode = current.mode;
    fresh.overrideDictionaries = current.overrideDictionaries;
    fresh.customFirst = current.customFirst;
    fresh.includeInitialPause = current.includeInitialPause;
    fresh.includeFinalPause = current.includeFinalPause;
    fresh.useOgiLexicon = current.useOgiLexicon;
    fresh.useOgiAddenda = current.useOgiAddenda;
    fresh.failOnUnknown = current.failOnUnknown;
    fresh.useCompositionalPronunciations = current.useCompositionalPronunciations;
    fresh.useInflectionPronunciations = current.useInflectionPronunciations;
    fresh.cleanupTlkMarkup = current.cleanupTlkMarkup;
    fresh.stripBraceStageDirections = current.stripBraceStageDirections;
    return fresh;
}

static void applyTextToNtpFlag(TextToNtpOptions& opts,
                               TextCommandExtraOptions& extra,
                               const std::vector<std::string>& args,
                               std::size_t& i) {
    if (args[i] == "--mode") {
        if (i + 1 >= args.size()) throw std::runtime_error("--mode requires simple or cslu");
        const std::string mode = args[++i];
        if (mode == "simple") opts.mode = NtpOutputMode::SimpleLinear;
        else if (mode == "cslu" || mode == "cslu-format" || mode == "automatic") opts.mode = NtpOutputMode::CsluAutomatic;
        else throw std::runtime_error("--mode must be simple or cslu");
    } else if (args[i] == "--dict") {
        if (i + 1 >= args.size()) throw std::runtime_error("--dict requires a path");
        opts.customDictionary = args[++i];
    } else if (args[i] == "--override-dict") {
        if (i + 1 >= args.size()) throw std::runtime_error("--override-dict requires a path");
        // User-specified overrides should take precedence over the packaged default.
        opts.overrideDictionaries.insert(opts.overrideDictionaries.begin(), args[++i]);
    } else if (args[i] == "--source-report") {
        if (i + 1 >= args.size()) throw std::runtime_error("--source-report requires a path");
        extra.sourceReport = args[++i];
    } else if (args[i] == "--unknown-report") {
        if (i + 1 >= args.size()) throw std::runtime_error("--unknown-report requires a path");
        extra.unknownReport = args[++i];
    } else if (args[i] == "--dictionaries" || args[i] == "--dict-root") {
        if (i + 1 >= args.size()) throw std::runtime_error(args[i] + " requires a path");
        applyStandaloneDictionaryRoot(opts, args[++i]);
    } else if (args[i] == "--models" || args[i] == "--model-root" || args[i] == "--package") {
        if (i + 1 >= args.size()) throw std::runtime_error(args[i] + " requires a path");
        opts = retargetTextOptionsPreservingFlags(opts, resolveModelRoot(args[++i]));
    } else if (args[i] == "--ogi-lex") {
        if (i + 1 >= args.size()) throw std::runtime_error("--ogi-lex requires a path");
        opts.ogiLexiconPath = args[++i];
        opts.useOgiLexicon = true;
    } else if (args[i] == "--ogi-addenda") {
        if (i + 1 >= args.size()) throw std::runtime_error("--ogi-addenda requires a path");
        opts.ogiAddendaPath = args[++i];
        opts.useOgiAddenda = true;
    } else if (args[i] == "--use-ogi") {
        opts.useOgiLexicon = true;
        opts.useOgiAddenda = true;
    } else if (args[i] == "--no-ogi") {
        opts.useOgiLexicon = false;
        opts.useOgiAddenda = false;
    } else if (args[i] == "--no-ogi-lex") {
        opts.useOgiLexicon = false;
    } else if (args[i] == "--no-ogi-addenda") {
        opts.useOgiAddenda = false;
    } else if (args[i] == "--no-initial-pause") {
        opts.includeInitialPause = false;
    } else if (args[i] == "--no-final-pause") {
        opts.includeFinalPause = false;
    } else if (args[i] == "--cslu-lookup-order") {
        opts.customFirst = false;
    } else if (args[i] == "--custom-first") {
        opts.customFirst = true;
    } else if (args[i] == "--no-compositional-dict" || args[i] == "--no-compositional-pronunciations" || args[i] == "--no-compose-compounds") {
        opts.useCompositionalPronunciations = false;
    } else if (args[i] == "--compositional-dict" || args[i] == "--compositional-pronunciations" || args[i] == "--compose-compounds") {
        opts.useCompositionalPronunciations = true;
    } else if (args[i] == "--derive-inflections") {
        opts.useInflectionPronunciations = true;
    } else if (args[i] == "--no-derive-inflections") {
        opts.useInflectionPronunciations = false;
    } else if (args[i] == "--no-tlk-cleanup" || args[i] == "--raw-tlk-markup" || args[i] == "--keep-tlk-markup") {
        opts.cleanupTlkMarkup = false;
    } else if (args[i] == "--tlk-cleanup" || args[i] == "--strip-tlk-markup") {
        opts.cleanupTlkMarkup = true;
        opts.stripBraceStageDirections = true;
    } else if (args[i] == "--keep-stage-directions") {
        opts.stripBraceStageDirections = false;
        opts.cleanupTlkMarkup = true;
    } else if (args[i] == "--strip-stage-directions") {
        opts.stripBraceStageDirections = true;
        opts.cleanupTlkMarkup = true;
    } else {
        throw std::runtime_error("unknown text-to-ntp option: " + args[i]);
    }
}

static int cmdTextToNtp(const std::vector<std::string>& args) {
    if (args.size() < 4) { usage(); return 2; }
    const std::string input = args[1];
    const fs::path outNtp = args[2];
    const bool newStyle = hasFlag(args, "--models") || hasFlag(args, "--model-root") || hasFlag(args, "--package");
    fs::path modelRoot;
    std::size_t flagStart = 3;
    if (newStyle) {
        modelRoot = readModelsFlag(args, flagStart);
        if (modelRoot.empty()) throw std::runtime_error("text-to-ntp requires --models <models>");
    } else {
        modelRoot = resolveModelRoot(args[3]);
        flagStart = 4;
    }
    TextToNtpOptions opts = makeDefaultTextToNtpOptions(modelRoot);
    TextCommandExtraOptions extra;
    for (std::size_t i = flagStart; i < args.size(); ++i) { applyTextToNtpFlag(opts, extra, args, i); }
    opts.failOnUnknown = false;
    const TextToNtpResult r = textToNtpFromSource(input, opts);
    if (!extra.sourceReport.empty()) writeTextToNtpSourceReport(extra.sourceReport, r);
    if (!extra.unknownReport.empty()) writeTextToNtpUnknownReport(extra.unknownReport, r);
    if (!r.unknownWords.empty()) {
        std::cout << "phase\ttext_to_ntp\n";
        std::cout << "word_count\t" << r.words.size() << "\n";
        std::cout << "unknown_count\t" << r.unknownWords.size() << "\n";
        for (const auto& w : r.unknownWords) std::cout << "unknown_word\t" << w << "\n";
        if (!extra.sourceReport.empty()) std::cout << "source_report\t" << extra.sourceReport.string() << "\n";
        if (!extra.unknownReport.empty()) std::cout << "unknown_report\t" << extra.unknownReport.string() << "\n";
        std::cout << "verdict\tUNKNOWN_WORDS\n";
        return 1;
    }
    writeNtpFile(outNtp, r.ntp);
    std::cout << "phase\ttext_to_ntp\n";
    std::cout << "mode\t" << (opts.mode == NtpOutputMode::SimpleLinear ? "simple" : "cslu") << "\n";
    std::cout << "output\t" << outNtp.string() << "\n";
    std::cout << "models_root\t" << opts.faRoot.string() << "\n";
    std::cout << "custom_dictionary\t" << opts.customDictionary.string() << "\n";
    std::cout << "cmu_dictionary_dir\t" << opts.cmuDictionaryDir.string() << "\n";
    std::cout << "use_ogi_lexicon\t" << (opts.useOgiLexicon ? "yes" : "no") << "\n";
    std::cout << "use_ogi_addenda\t" << (opts.useOgiAddenda ? "yes" : "no") << "\n";
    std::cout << "word_count\t" << r.words.size() << "\n";
    std::cout << "unknown_count\t0\n";
    std::cout << "normalized_text\t" << r.normalizedText << "\n";
    std::cout << "ntp\t" << r.ntp << "\n";
    std::map<std::string, int> sourceCounts;
    for (const auto& wi : r.wordInfo) ++sourceCounts[wi.source];
    for (const auto& kv : sourceCounts) std::cout << "source_count\t" << kv.first << "\t" << kv.second << "\n";
    std::cout << "verdict\tWROTE_NTP\n";
    return 0;
}

static int cmdLookupWord(const std::vector<std::string>& args) {
    if (args.size() < 3) { usage(); return 2; }
    const std::string word = args[1];
    const bool newStyle = hasFlag(args, "--models") || hasFlag(args, "--model-root") || hasFlag(args, "--package");
    fs::path modelRoot;
    std::size_t flagStart = 2;
    if (newStyle) { modelRoot = readModelsFlag(args, flagStart); }
    else { modelRoot = resolveModelRoot(args[2]); flagStart = 3; }
    if (modelRoot.empty()) throw std::runtime_error("lookup-word requires --models <models>");
    TextToNtpOptions opts = makeDefaultTextToNtpOptions(modelRoot);
    TextCommandExtraOptions extra;
    for (std::size_t i = flagStart; i < args.size(); ++i) { applyTextToNtpFlag(opts, extra, args, i); }
    opts.failOnUnknown = false;
    std::string literal = word;
    if (literal.empty() || (literal.back() != '.' && literal.back() != '?' && literal.back() != '!')) literal += ".";
    const TextToNtpResult r = textToNtpFromSource(literal, opts);
    if (!extra.sourceReport.empty()) writeTextToNtpSourceReport(extra.sourceReport, r);
    if (!extra.unknownReport.empty()) writeTextToNtpUnknownReport(extra.unknownReport, r);
    std::cout << "phase\tlookup_word\n";
    std::cout << "input\t" << word << "\n";
    std::cout << "models_root\t" << opts.faRoot.string() << "\n";
    if (!r.wordInfo.empty()) {
        const auto& wi = r.wordInfo.front();
        std::cout << "normalized_word\t" << wi.word << "\n";
        std::cout << "source\t" << wi.source << "\n";
        std::cout << "pronunciation\t" << wi.pronunciation << "\n";
        std::cout << "verdict\t" << (wi.source == "UNKNOWN" ? "UNKNOWN_WORD" : "FOUND") << "\n";
        return wi.source == "UNKNOWN" ? 1 : 0;
    }
    std::cout << "verdict\tNO_WORD_AFTER_NORMALIZATION\n";
    return 1;
}

static int cmdCompareTextToNtpTest(const std::vector<std::string>& args) {
    if (args.size() < 2 || args.size() > 3) {
        usage();
        return 2;
    }
    const fs::path faRoot = args[1];
    const fs::path outDir = (args.size() == 3) ? fs::path(args[2]) : fs::path();
    TextToNtpOptions csluOpts = makeDefaultTextToNtpOptions(faRoot);
    csluOpts.mode = NtpOutputMode::CsluAutomatic;
    TextToNtpOptions simpleOpts = makeDefaultTextToNtpOptions(faRoot);
    simpleOpts.mode = NtpOutputMode::SimpleLinear;
    const TextToNtpResult cslu = textToNtpFromSource((faRoot / "test.txt").string(), csluOpts);
    const TextToNtpResult simple = textToNtpFromSource((faRoot / "test.txt").string(), simpleOpts);
    const std::string expected = readTextFile(faRoot / "test_automatic.ntp");
    const bool match = normalizeWhitespaceLocal(cslu.ntp) == normalizeWhitespaceLocal(expected);
    if (!outDir.empty()) {
        writeNtpFile(outDir / "test_generated_cslu.ntp", cslu.ntp);
        writeNtpFile(outDir / "test_generated_simple.ntp", simple.ntp);
    }
    std::cout << "phase\t14_text_to_ntp_validation\n";
    std::cout << "fa_root\t" << faRoot.string() << "\n";
    if (!outDir.empty()) std::cout << "out_dir\t" << outDir.string() << "\n";
    std::cout << "word_count\t" << cslu.words.size() << "\n";
    std::cout << "normalized_text\t" << cslu.normalizedText << "\n";
    std::cout << "cslu_generated\t" << cslu.ntp << "\n";
    std::cout << "simple_generated\t" << simple.ntp << "\n";
    std::cout << "match_test_automatic_whitespace_normalized\t" << (match ? "yes" : "no") << "\n";
    std::cout << "verdict\t" << (match ? "PASS" : "CHECK") << "\n";
    return match ? 0 : 1;
}

static int cmdAlignWavText(const std::vector<std::string>& args) {
    if (args.size() < 5) { usage(); return 2; }
    const fs::path wav = args[1];
    const std::string textSource = args[2];
    const bool newStyle = hasFlag(args, "--models") || hasFlag(args, "--model-root") || hasFlag(args, "--package");
    fs::path modelRoot;
    fs::path outPath;
    fs::path workDir;
    std::size_t flagStart = 0;
    if (newStyle) {
        outPath = args[3];
        flagStart = 4;
        modelRoot = readModelsFlag(args, flagStart);
        if (modelRoot.empty()) throw std::runtime_error("align-wav-text requires --models <models>");
    } else {
        modelRoot = resolveModelRoot(args[3]);
        outPath = args[4];
        flagStart = 5;
        if (flagStart < args.size() && !isFlagToken(args[flagStart])) workDir = args[flagStart++];
    }
    TextToNtpOptions opts = makeDefaultTextToNtpOptions(modelRoot);
    TextCommandExtraOptions extra;
    opts.mode = NtpOutputMode::SimpleLinear;
    for (std::size_t i = flagStart; i < args.size(); ++i) {
        const bool modelFlag = (args[i] == "--models" || args[i] == "--model-root" || args[i] == "--package");
        if (consumeModelOrWorkFlag(args, i, modelRoot, workDir)) {
            if (modelFlag) opts = retargetTextOptionsPreservingFlags(opts, modelRoot);
            continue;
        }
        if (newStyle && !isFlagToken(args[i]) && workDir.empty()) { workDir = args[i]; continue; }
        applyTextToNtpFlag(opts, extra, args, i);
    }
    opts.failOnUnknown = false;
    const TextToNtpResult ntpResult = textToNtpFromSource(textSource, opts);
    if (!extra.sourceReport.empty()) writeTextToNtpSourceReport(extra.sourceReport, ntpResult);
    if (!extra.unknownReport.empty()) writeTextToNtpUnknownReport(extra.unknownReport, ntpResult);
    if (!ntpResult.unknownWords.empty()) {
        std::cout << "phase\talign_wav_text_lip\n";
        std::cout << "unknown_count\t" << ntpResult.unknownWords.size() << "\n";
        for (const auto& w : ntpResult.unknownWords) std::cout << "unknown_word\t" << w << "\n";
        if (!extra.sourceReport.empty()) std::cout << "source_report\t" << extra.sourceReport.string() << "\n";
        if (!extra.unknownReport.empty()) std::cout << "unknown_report\t" << extra.unknownReport.string() << "\n";
        std::cout << "verdict\tUNKNOWN_WORDS\n";
        return 1;
    }
    fs::path ntpPath;
    if (!workDir.empty()) ntpPath = workDir / "text" / "generated.ntp";
    else { ntpPath = outPath; ntpPath += ".generated.ntp"; }
    writeNtpFile(ntpPath, ntpResult.ntp);
    if (!workDir.empty()) {
        ensureDirectory(workDir / "text");
        std::ofstream norm(workDir / "text" / "normalized_text.txt");
        norm << ntpResult.normalizedText << "\n";
        std::ofstream words(workDir / "text" / "words.tsv");
        words << "index\tword\n";
        for (std::size_t i = 0; i < ntpResult.words.size(); ++i) words << i << '\t' << ntpResult.words[i] << '\n';
        writeTextToNtpSourceReport(workDir / "text" / "pronunciations.tsv", ntpResult);
        writeTextToNtpUnknownReport(workDir / "text" / "unknown_words.tsv", ntpResult);
    }
    const Pcm16Mono original = readPcm16Mono(wav);
    FullFrontendFeatures frontend = computeFullFrontendFromOriginalWav(original);
    EndToEndAlignmentResult r = runEndToEndFromFrontend(frontend, ntpPath, opts.faRoot);
    writePhn(outPath, r.alignment.phn);
    writeEndToEndWorkDir(r, workDir, opts.faRoot, outPath);
    printEndToEndSummary(r, outPath, workDir, "single_wav_text_to_ntp");
    std::cout << "models_root\t" << opts.faRoot.string() << "\n";
    std::cout << "text_to_ntp_mode\t" << (opts.mode == NtpOutputMode::SimpleLinear ? "simple" : "cslu") << "\n";
    std::cout << "generated_ntp\t" << ntpPath.string() << "\n";
    std::cout << "word_count\t" << ntpResult.words.size() << "\n";
    std::cout << "unknown_count\t0\n";
    std::cout << "verdict\tWROTE_PHN\n";
    return 0;
}

static int cmdAlignWavTextLip(const std::vector<std::string>& args) {
    if (args.size() < 5) { usage(); return 2; }
    const fs::path wav = args[1];
    const std::string textSource = args[2];
    const bool newStyle = hasFlag(args, "--models") || hasFlag(args, "--model-root") || hasFlag(args, "--package");
    fs::path modelRoot;
    fs::path lipPath;
    fs::path workDir;
    fs::path phnOut;
    LipTailRestOptions tail;
    LipQualityOptions quality;
    std::size_t flagStart = 0;
    if (newStyle) {
        lipPath = args[3];
        flagStart = 4;
        modelRoot = readModelsFlag(args, flagStart);
        if (modelRoot.empty()) throw std::runtime_error("align-wav-text-lip requires --models <models>");
    } else {
        modelRoot = resolveModelRoot(args[3]);
        lipPath = args[4];
        flagStart = 5;
        if (flagStart < args.size() && !isFlagToken(args[flagStart])) workDir = args[flagStart++];
    }
    TextToNtpOptions opts = makeDefaultTextToNtpOptions(modelRoot);
    TextCommandExtraOptions extra;
    opts.mode = NtpOutputMode::SimpleLinear;
    for (std::size_t i = flagStart; i < args.size(); ++i) {
        const bool modelFlag = (args[i] == "--models" || args[i] == "--model-root" || args[i] == "--package");
        if (consumeModelOrWorkFlag(args, i, modelRoot, workDir)) {
            if (modelFlag) opts = retargetTextOptionsPreservingFlags(opts, modelRoot);
            continue;
        }
        if (consumeLipTailRestFlag(args, i, tail)) continue;
        if (consumeLipQualityFlag(args, i, quality)) continue;
        if (args[i] == "--phn-out" || args[i] == "--phn-output") {
            if (i + 1 >= args.size()) throw std::runtime_error(args[i] + " requires a path");
            phnOut = args[++i];
            continue;
        }
        if (args[i] == "--keep-phn") {
            if (phnOut.empty()) { fs::path p = lipPath; p.replace_extension(".phn"); phnOut = p; }
            continue;
        }
        if (args[i] == "--no-phn") { phnOut.clear(); continue; }
        if (newStyle && !isFlagToken(args[i]) && workDir.empty()) { workDir = args[i]; continue; }
        applyTextToNtpFlag(opts, extra, args, i);
    }
    opts.failOnUnknown = false;
    const TextToNtpResult ntpResult = textToNtpFromSource(textSource, opts);
    if (!extra.sourceReport.empty()) writeTextToNtpSourceReport(extra.sourceReport, ntpResult);
    if (!extra.unknownReport.empty()) writeTextToNtpUnknownReport(extra.unknownReport, ntpResult);
    if (!ntpResult.unknownWords.empty()) {
        std::cout << "phase\t26_align_wav_text_lip\n";
        std::cout << "unknown_count\t" << ntpResult.unknownWords.size() << "\n";
        for (const auto& w : ntpResult.unknownWords) std::cout << "unknown_word\t" << w << "\n";
        if (!extra.sourceReport.empty()) std::cout << "source_report\t" << extra.sourceReport.string() << "\n";
        if (!extra.unknownReport.empty()) std::cout << "unknown_report\t" << extra.unknownReport.string() << "\n";
        std::cout << "verdict\tUNKNOWN_WORDS\n";
        return 1;
    }
    fs::path ntpPath;
    if (!workDir.empty()) ntpPath = workDir / "text" / "generated.ntp";
    else { ntpPath = lipPath; ntpPath += ".generated.ntp"; }
    writeNtpFile(ntpPath, ntpResult.ntp);
    if (phnOut.empty() && !workDir.empty()) phnOut = defaultIntermediatePhnForLip(lipPath, workDir);
    if (!workDir.empty()) {
        ensureDirectory(workDir / "text");
        std::ofstream norm(workDir / "text" / "normalized_text.txt");
        norm << ntpResult.normalizedText << "\n";
        std::ofstream words(workDir / "text" / "words.tsv");
        words << "index\tword\n";
        for (std::size_t i = 0; i < ntpResult.words.size(); ++i) words << i << '\t' << ntpResult.words[i] << '\n';
        writeTextToNtpSourceReport(workDir / "text" / "pronunciations.tsv", ntpResult);
        writeTextToNtpUnknownReport(workDir / "text" / "unknown_words.tsv", ntpResult);
    }
    const Pcm16Mono original = readPcm16Mono(wav);
    FullFrontendFeatures frontend = computeFullFrontendFromOriginalWav(original);
    EndToEndAlignmentResult r = runEndToEndFromFrontend(frontend, ntpPath, opts.faRoot);
    if (!phnOut.empty()) writePhnEnsuringParent(phnOut, r.alignment.phn);
    writeEndToEndWorkDir(r, workDir, opts.faRoot, phnOut);
    const int alignedDurationMs = r.alignment.phn.intervals.empty() ? 0 : std::max(0, r.alignment.phn.intervals.back().endMs);
    const int tailMs = resolveTailRestMs(alignedDurationMs, tail);
    LipFile lip = writeLipFromPhnToPath(r.alignment.phn, lipPath, tail, quality);
    printEndToEndSummary(r, lipPath, workDir, "single_wav_text_to_lip");
    std::cout << "models_root\t" << opts.faRoot.string() << "\n";
    std::cout << "text_to_ntp_mode\t" << (opts.mode == NtpOutputMode::SimpleLinear ? "simple" : "cslu") << "\n";
    std::cout << "generated_ntp\t" << ntpPath.string() << "\n";
    if (!phnOut.empty()) std::cout << "intermediate_phn\t" << phnOut.string() << "\n";
    std::cout << "tail_rest_mode\t" << lipTailRestModeName(tail) << "\n";
    std::cout << "tail_rest_ms\t" << tailMs << "\n";
    std::cout << "word_count\t" << ntpResult.words.size() << "\n";
    std::cout << "unknown_count\t0\n";
    printLipSummary(lip, lipPath, tail, tailMs, quality);
    std::cout << "verdict\tWROTE_LIP\n";
    return 0;
}



static std::string lowerAsciiLocal(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

static bool extensionMatchesLocal(const fs::path& p, const std::vector<std::string>& extsLowerWithDot) {
    const std::string ext = lowerAsciiLocal(p.extension().string());
    return std::find(extsLowerWithDot.begin(), extsLowerWithDot.end(), ext) != extsLowerWithDot.end();
}

static fs::path findSameStemFile(const fs::path& wavPath, const std::vector<std::string>& extsLowerWithDot) {
    const fs::path directBase = wavPath.parent_path() / wavPath.stem();
    for (const std::string& ext : extsLowerWithDot) {
        fs::path candidate = directBase;
        candidate += ext;
        if (fs::exists(candidate) && fs::is_regular_file(candidate)) return candidate;
    }
    if (!fs::exists(wavPath.parent_path())) return fs::path();
    const std::string wantedStem = lowerAsciiLocal(wavPath.stem().string());
    for (const auto& ent : fs::directory_iterator(wavPath.parent_path())) {
        if (!ent.is_regular_file()) continue;
        const fs::path p = ent.path();
        if (lowerAsciiLocal(p.stem().string()) == wantedStem && extensionMatchesLocal(p, extsLowerWithDot)) {
            return p;
        }
    }
    return fs::path();
}

static std::vector<fs::path> collectBatchWavs(const fs::path& inputDir, bool recursive) {
    std::vector<fs::path> wavs;
    if (!fs::exists(inputDir) || !fs::is_directory(inputDir)) {
        throw std::runtime_error("input_dir is not a directory: " + inputDir.string());
    }
    const auto consider = [&](const fs::directory_entry& ent) {
        std::error_code ec;
        const fs::file_status status = ent.symlink_status(ec);
        if (!ec && fs::is_regular_file(status) &&
            lowerAsciiLocal(ent.path().extension().string()) == ".wav") {
            wavs.push_back(ent.path());
        }
    };
    if (recursive) {
        for (const auto& ent : fs::recursive_directory_iterator(inputDir)) {
            consider(ent);
        }
    } else {
        for (const auto& ent : fs::directory_iterator(inputDir)) {
            consider(ent);
        }
    }
    std::sort(wavs.begin(), wavs.end());
    return wavs;
}

static bool safeBatchRelativePath(const fs::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& component : path) {
        if (component == "..") return false;
    }
    return true;
}

static fs::path lexicalBatchRelativePath(const fs::path& path, const fs::path& inputRoot) {
    std::error_code ec;
    const fs::path absolutePath = fs::absolute(path, ec).lexically_normal();
    if (ec) throw std::runtime_error("cannot resolve batch input path: " + ec.message());
    const fs::path absoluteRoot = fs::absolute(inputRoot, ec).lexically_normal();
    if (ec) throw std::runtime_error("cannot resolve batch input directory: " + ec.message());
    const fs::path relative = absolutePath.lexically_relative(absoluteRoot);
    if (!safeBatchRelativePath(relative)) {
        throw std::runtime_error("batch input is outside the selected input directory: " +
                                 path.string());
    }
    return relative;
}

static bool resolvedBatchPathIsWithin(const fs::path& root, const fs::path& candidate) {
    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty()) return candidate == root;
    return safeBatchRelativePath(relative);
}

static void requireBatchOutputContained(const fs::path& outputRoot,
                                        const fs::path& candidate) {
    std::error_code ec;
    const fs::path absoluteRoot = fs::absolute(outputRoot, ec);
    if (ec) throw std::runtime_error("cannot resolve batch output directory: " + ec.message());
    const fs::path canonicalRoot = fs::weakly_canonical(absoluteRoot, ec);
    if (ec) throw std::runtime_error("cannot resolve batch output directory: " + ec.message());
    const fs::path absoluteCandidate = fs::absolute(candidate, ec);
    if (ec) throw std::runtime_error("cannot resolve batch output path: " + ec.message());
    const fs::path canonicalCandidate = fs::weakly_canonical(absoluteCandidate, ec);
    if (ec) throw std::runtime_error("cannot resolve batch output path: " + ec.message());
    if (!resolvedBatchPathIsWithin(canonicalRoot, canonicalCandidate)) {
        throw std::runtime_error("refusing batch output outside the selected output directory: " +
                                 candidate.string());
    }
}

static std::string tsvCellLocal(std::string s) {
    for (char& ch : s) {
        if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
    }
    return s;
}

static int phnDurationMsLocal(const PhnFile& phn) {
    return phn.intervals.empty() ? 0 : phn.intervals.back().endMs;
}

static std::string sourceCountsStringLocal(const TextToNtpResult& r) {
    std::map<std::string, int> counts;
    for (const auto& wi : r.wordInfo) ++counts[wi.source];
    std::ostringstream ss;
    bool first = true;
    for (const auto& kv : counts) {
        if (!first) ss << ';';
        first = false;
        ss << kv.first << '=' << kv.second;
    }
    return ss.str();
}

static void writeBatchReportHeader(std::ofstream& out) {
    out << "relative_wav\tstatus\tinput_kind\toutput_kind\ttranscript\toutput_ntp\toutput_phn\toutput_lip\twork_dir"
        << "\tduration_ms\tfrontend_frames\tword_count\tunknown_word_count\tphone_count"
        << "\tphn_interval_count\tlip_entry_count\ttail_rest_ms\tclose_on_pause_ms\tsmoothing\tmin_key_ms\tpreserve_closures"
        << "\tanticipate_closures_ms\tanticipate_fricatives_ms\tanticipate_stops_ms"
        << "\tfirst_phone\tlast_phone\tsource_counts\terror_message\n";
}


static std::vector<fs::path> collectTranscriptFiles(const fs::path& inputPath, bool recursive) {
    const std::vector<std::string> exts = {".txt", ".lab"};
    std::vector<fs::path> files;
    if (!fs::exists(inputPath)) {
        throw std::runtime_error("transcript path does not exist: " + inputPath.string());
    }
    if (fs::is_regular_file(inputPath)) {
        if (!extensionMatchesLocal(inputPath, exts)) {
            throw std::runtime_error("transcript file must have .txt or .lab extension: " + inputPath.string());
        }
        files.push_back(inputPath);
        return files;
    }
    if (!fs::is_directory(inputPath)) {
        throw std::runtime_error("transcript path is neither a file nor a directory: " + inputPath.string());
    }
    if (recursive) {
        for (const auto& ent : fs::recursive_directory_iterator(inputPath)) {
            if (ent.is_regular_file() && extensionMatchesLocal(ent.path(), exts)) files.push_back(ent.path());
        }
    } else {
        for (const auto& ent : fs::directory_iterator(inputPath)) {
            if (ent.is_regular_file() && extensionMatchesLocal(ent.path(), exts)) files.push_back(ent.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

static std::string dictionaryStubForWord(const std::string& word) {
    return word + " =  ;";
}

static std::string trimLocal(const std::string& s) {
    std::size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    std::size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static std::vector<std::string> splitTabLocal(const std::string& line) {
    std::vector<std::string> cols;
    std::string cur;
    for (char ch : line) {
        if (ch == '\t') {
            cols.push_back(cur);
            cur.clear();
        } else if (ch != '\r') {
            cur.push_back(ch);
        }
    }
    cols.push_back(cur);
    return cols;
}

static std::string dictionaryEntryKeyFromLine(const std::string& line) {
    std::string s = trimLocal(line);
    if (s.empty() || s[0] == '#' || s[0] == ';') return std::string();
    const std::size_t eq = s.find('=');
    if (eq == std::string::npos) return std::string();
    return lowerAsciiLocal(trimLocal(s.substr(0, eq)));
}

static int columnIndexByNameLocal(const std::vector<std::string>& header, const std::vector<std::string>& candidates) {
    for (const std::string& c : candidates) {
        for (std::size_t i = 0; i < header.size(); ++i) {
            if (header[i] == c) return static_cast<int>(i);
        }
    }
    return -1;
}

struct ScanUnknownOptions {
    fs::path inputPath;
    fs::path faRoot;
    fs::path unknownReport;
    fs::path sourceReport;
    bool recursive = false;
    TextToNtpOptions textOptions;
};

static int cmdScanUnknowns(const std::vector<std::string>& args) {
    if (args.size() < 4) {
        usage();
        return 2;
    }
    ScanUnknownOptions opt;
    opt.inputPath = args[1];
    const bool newStyle = hasFlag(args, "--models") || hasFlag(args, "--model-root") || hasFlag(args, "--package");
    std::size_t flagStart;
    if (newStyle) {
        opt.unknownReport = args[2];
        flagStart = 3;
        opt.faRoot = readModelsFlag(args, flagStart);
    } else {
        opt.faRoot = resolveModelRoot(args[2]);
        opt.unknownReport = args[3];
        flagStart = 4;
    }
    if (opt.faRoot.empty()) throw std::runtime_error("scan-unknowns requires --models <models> in standalone syntax");
    opt.textOptions = makeDefaultTextToNtpOptions(opt.faRoot);
    opt.textOptions.failOnUnknown = false;
    TextCommandExtraOptions ignored;
    fs::path ignoredWorkDir;
    for (std::size_t i = flagStart; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--recursive") {
            opt.recursive = true;
        } else if (a == "--source-report") {
            if (i + 1 >= args.size()) throw std::runtime_error("--source-report requires a path");
            opt.sourceReport = args[++i];
        } else if (a == "--models" || a == "--model-root" || a == "--package") {
            applyTextToNtpFlag(opt.textOptions, ignored, args, i);
            opt.faRoot = opt.textOptions.faRoot;
            opt.textOptions.failOnUnknown = false;
        } else if (consumeModelOrWorkFlag(args, i, opt.faRoot, ignoredWorkDir)) {
            opt.textOptions.failOnUnknown = false;
        } else {
            applyTextToNtpFlag(opt.textOptions, ignored, args, i);
        }
    }

    const std::vector<fs::path> files = collectTranscriptFiles(opt.inputPath, opt.recursive);
    if (!opt.unknownReport.parent_path().empty()) ensureDirectory(opt.unknownReport.parent_path());
    std::ofstream unknown(opt.unknownReport, std::ios::binary);
    if (!unknown) throw std::runtime_error("cannot write unknown report: " + opt.unknownReport.string());
    unknown << "relative_text\tword\tdictionary_stub\tword_index\tsource_counts\n";

    std::ofstream source;
    if (!opt.sourceReport.empty()) {
        if (!opt.sourceReport.parent_path().empty()) ensureDirectory(opt.sourceReport.parent_path());
        source.open(opt.sourceReport, std::ios::binary);
        if (!source) throw std::runtime_error("cannot write source report: " + opt.sourceReport.string());
        source << "relative_text\tindex\tword\tsource\tpronunciation\n";
    }

    std::size_t filesWithUnknowns = 0;
    std::size_t totalUnknownInstances = 0;
    std::set<std::string> uniqueUnknowns;
    std::map<std::string, int> sourceTotals;
    TextToNtpResolver resolver(opt.textOptions);

    for (const fs::path& file : files) {
        fs::path rel;
        if (fs::is_directory(opt.inputPath)) {
            try { rel = fs::relative(file, opt.inputPath); }
            catch (...) { rel = file.filename(); }
        } else {
            rel = file.filename();
        }
        TextToNtpResult r = resolver.resolveSource(file.string());
        for (const auto& wi : r.wordInfo) {
            ++sourceTotals[wi.source];
            if (source.is_open()) {
                source << tsvCellLocal(rel.generic_string()) << '\t'
                       << wi.index << '\t'
                       << tsvCellLocal(wi.word) << '\t'
                       << tsvCellLocal(wi.source) << '\t'
                       << tsvCellLocal(wi.pronunciation) << '\n';
            }
        }
        if (!r.unknownWords.empty()) ++filesWithUnknowns;
        const std::string counts = sourceCountsStringLocal(r);
        for (const auto& wi : r.wordInfo) {
            if (wi.source != "UNKNOWN") continue;
            ++totalUnknownInstances;
            uniqueUnknowns.insert(wi.word);
            unknown << tsvCellLocal(rel.generic_string()) << '\t'
                    << tsvCellLocal(wi.word) << '\t'
                    << tsvCellLocal(dictionaryStubForWord(wi.word)) << '\t'
                    << wi.index << '\t'
                    << tsvCellLocal(counts) << '\n';
        }
    }

    std::cout << "phase\tscan_unknowns\n";
    std::cout << "input_path\t" << opt.inputPath.string() << "\n";
    std::cout << "models_root\t" << opt.faRoot.string() << "\n";
    std::cout << "recursive\t" << (opt.recursive ? "yes" : "no") << "\n";
    std::cout << "unknown_report\t" << opt.unknownReport.string() << "\n";
    if (!opt.sourceReport.empty()) std::cout << "source_report\t" << opt.sourceReport.string() << "\n";
    std::cout << "files_seen\t" << files.size() << "\n";
    std::cout << "files_with_unknowns\t" << filesWithUnknowns << "\n";
    std::cout << "unknown_instances\t" << totalUnknownInstances << "\n";
    std::cout << "unique_unknown_words\t" << uniqueUnknowns.size() << "\n";
    for (const auto& w : uniqueUnknowns) std::cout << "unknown_word\t" << w << "\n";
    for (const auto& kv : sourceTotals) std::cout << "source_total\t" << kv.first << "\t" << kv.second << "\n";
    std::cout << "verdict\t" << (totalUnknownInstances == 0 ? "NO_UNKNOWNS" : "UNKNOWN_WORDS") << "\n";
    return totalUnknownInstances == 0 ? 0 : 1;
}

struct TlkStringEntryLocal {
    std::uint32_t strref = 0;
    std::string sound;
    std::string text;
};

struct ParsedTlkLocal {
    fs::path path;
    std::uint32_t language = 0;
    std::uint32_t stringCount = 0;
    std::vector<TlkStringEntryLocal> entries;
};

static ParsedTlkLocal parseTlkLocal(const fs::path& path) {
    const neotlk::TalkTable table(path.string());
    if (table.storageFormat() != neotlk::TlkStorageFormat::ClassicV30) {
        throw std::runtime_error("not a TLK V3.0 file: " + path.string());
    }

    ParsedTlkLocal tlk;
    tlk.path = path;
    tlk.language = table.language();
    tlk.stringCount = table.count();
    tlk.entries.reserve(table.entries().size());
    for (const neotlk::TalkString& entry : table.entries()) {
        tlk.entries.push_back({entry.strRef, entry.soundString(), entry.text});
    }
    return tlk;
}

static std::vector<fs::path> collectTlkFilesLocal(const fs::path& inputPath, bool recursive) {
    std::vector<fs::path> files;
    if (!fs::exists(inputPath)) throw std::runtime_error("TLK path does not exist: " + inputPath.string());
    if (fs::is_regular_file(inputPath)) {
        if (lowerAsciiLocal(inputPath.extension().string()) != ".tlk") {
            throw std::runtime_error("TLK input file must have .tlk extension: " + inputPath.string());
        }
        files.push_back(inputPath);
        return files;
    }
    if (!fs::is_directory(inputPath)) throw std::runtime_error("TLK path is neither a file nor a directory: " + inputPath.string());
    if (recursive) {
        for (const auto& ent : fs::recursive_directory_iterator(inputPath)) {
            if (ent.is_regular_file() && lowerAsciiLocal(ent.path().extension().string()) == ".tlk") files.push_back(ent.path());
        }
    } else {
        for (const auto& ent : fs::directory_iterator(inputPath)) {
            if (ent.is_regular_file() && lowerAsciiLocal(ent.path().extension().string()) == ".tlk") files.push_back(ent.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

struct TlkUnknownAggregateLocal {
    std::size_t count = 0;
    std::set<std::string> files;
    std::string exampleTlk;
    std::uint32_t exampleStrref = 0;
    std::string exampleSound;
    std::string exampleText;
};

struct TlkScanFileStatsLocal {
    fs::path path;
    std::string name;
    std::uint32_t stringCount = 0;
    std::size_t nonemptyStrings = 0;
    std::size_t tokenInstances = 0;
    std::size_t knownTokenInstances = 0;
    std::size_t unknownTokenInstances = 0;
    std::map<std::string, std::size_t> sourceCounts;
    std::map<std::string, TlkUnknownAggregateLocal> unknowns;
};

static std::string joinStringSetLocal(const std::set<std::string>& values, const std::string& sep) {
    std::string out;
    bool first = true;
    for (const std::string& v : values) {
        if (!first) out += sep;
        first = false;
        out += v;
    }
    return out;
}

static std::vector<std::pair<std::string, TlkUnknownAggregateLocal>> sortedUnknownsLocal(
        const std::map<std::string, TlkUnknownAggregateLocal>& unknowns) {
    std::vector<std::pair<std::string, TlkUnknownAggregateLocal>> rows(unknowns.begin(), unknowns.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.second.count != b.second.count) return a.second.count > b.second.count;
        return a.first < b.first;
    });
    return rows;
}

static std::string jsonEscapeLocal(std::string s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) >= 0x20) out.push_back(ch);
                break;
        }
    }
    return out;
}

static std::string markdownCellLocal(std::string s) {
    s = tsvCellLocal(std::move(s));
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == '|') out += "\\|";
        else out.push_back(ch);
    }
    return out;
}

static void writeTlkUnknownWordReportLocal(const fs::path& outPath,
                                           const std::map<std::string, TlkUnknownAggregateLocal>& unknowns) {
    std::ofstream out(outPath, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write TLK unknown-word report: " + outPath.string());
    out << "word\tcount\tfiles\texample_tlk\texample_strref\texample_sound\tdictionary_stub\texample_text\n";
    for (const auto& row : sortedUnknownsLocal(unknowns)) {
        const auto& u = row.second;
        out << tsvCellLocal(row.first) << '\t'
            << u.count << '\t'
            << tsvCellLocal(joinStringSetLocal(u.files, ",")) << '\t'
            << tsvCellLocal(u.exampleTlk) << '\t'
            << u.exampleStrref << '\t'
            << tsvCellLocal(u.exampleSound) << '\t'
            << tsvCellLocal(dictionaryStubForWord(row.first)) << '\t'
            << tsvCellLocal(u.exampleText) << '\n';
    }
}

struct ScanTlkUnknownOptionsLocal {
    fs::path inputPath;
    fs::path outputDir;
    fs::path faRoot;
    fs::path sourceReport;
    bool recursive = false;
    TextToNtpOptions textOptions;
};

static int cmdScanTlkUnknowns(const std::vector<std::string>& args) {
    if (args.size() < 4) {
        usage();
        return 2;
    }
    ScanTlkUnknownOptionsLocal opt;
    opt.inputPath = args[1];
    opt.outputDir = args[2];
    opt.faRoot = readModelsFlag(args, 3);
    if (opt.faRoot.empty()) throw std::runtime_error("scan-tlk-unknowns requires --models <models>");
    opt.textOptions = makeDefaultTextToNtpOptions(opt.faRoot);
    opt.textOptions.failOnUnknown = false;
    TextCommandExtraOptions ignored;
    for (std::size_t i = 3; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--recursive") {
            opt.recursive = true;
        } else if (a == "--source-report") {
            if (i + 1 >= args.size()) throw std::runtime_error("--source-report requires a path");
            opt.sourceReport = args[++i];
        } else {
            applyTextToNtpFlag(opt.textOptions, ignored, args, i);
            opt.textOptions.failOnUnknown = false;
        }
    }
    opt.faRoot = opt.textOptions.faRoot;

    ensureDirectory(opt.outputDir);
    const std::vector<fs::path> files = collectTlkFilesLocal(opt.inputPath, opt.recursive);
    if (files.empty()) throw std::runtime_error("no .tlk files found under: " + opt.inputPath.string());

    std::ofstream source;
    if (!opt.sourceReport.empty()) {
        if (!opt.sourceReport.parent_path().empty()) ensureDirectory(opt.sourceReport.parent_path());
        source.open(opt.sourceReport, std::ios::binary);
        if (!source) throw std::runtime_error("cannot write TLK source report: " + opt.sourceReport.string());
        source << "tlk\tstrref\tsound\tindex\tword\tsource\tpronunciation\n";
    }

    TextToNtpResolver resolver(opt.textOptions);
    std::vector<TlkScanFileStatsLocal> stats;
    std::map<std::string, TlkUnknownAggregateLocal> combinedUnknowns;
    std::map<std::string, std::size_t> combinedSourceCounts;

    for (const fs::path& file : files) {
        const ParsedTlkLocal tlk = parseTlkLocal(file);
        TlkScanFileStatsLocal st;
        st.path = file;
        st.name = file.filename().string();
        st.stringCount = tlk.stringCount;

        const fs::path instancePath = opt.outputDir / (file.stem().string() + "_unknown_instances.tsv");
        std::ofstream instances(instancePath, std::ios::binary);
        if (!instances) throw std::runtime_error("cannot write TLK unknown-instance report: " + instancePath.string());
        instances << "tlk\tstrref\tsound\tword\tword_index\ttext\n";

        for (const TlkStringEntryLocal& e : tlk.entries) {
            if (trimLocal(e.text).empty()) continue;
            ++st.nonemptyStrings;
            const TextToNtpResult r = resolver.resolveSource(e.text);
            for (const WordPronunciationInfo& wi : r.wordInfo) {
                ++st.tokenInstances;
                ++st.sourceCounts[wi.source];
                ++combinedSourceCounts[wi.source];
                if (source.is_open()) {
                    source << tsvCellLocal(st.name) << '\t'
                           << e.strref << '\t'
                           << tsvCellLocal(e.sound) << '\t'
                           << wi.index << '\t'
                           << tsvCellLocal(wi.word) << '\t'
                           << tsvCellLocal(wi.source) << '\t'
                           << tsvCellLocal(wi.pronunciation) << '\n';
                }
                if (wi.source == "UNKNOWN") {
                    ++st.unknownTokenInstances;
                    TlkUnknownAggregateLocal& u = st.unknowns[wi.word];
                    ++u.count;
                    u.files.insert(st.name);
                    if (u.exampleTlk.empty()) {
                        u.exampleTlk = st.name;
                        u.exampleStrref = e.strref;
                        u.exampleSound = e.sound;
                        u.exampleText = e.text;
                    }
                    TlkUnknownAggregateLocal& cu = combinedUnknowns[wi.word];
                    ++cu.count;
                    cu.files.insert(st.name);
                    if (cu.exampleTlk.empty()) {
                        cu.exampleTlk = st.name;
                        cu.exampleStrref = e.strref;
                        cu.exampleSound = e.sound;
                        cu.exampleText = e.text;
                    }
                    instances << tsvCellLocal(st.name) << '\t'
                              << e.strref << '\t'
                              << tsvCellLocal(e.sound) << '\t'
                              << tsvCellLocal(wi.word) << '\t'
                              << wi.index << '\t'
                              << tsvCellLocal(e.text) << '\n';
                } else {
                    ++st.knownTokenInstances;
                }
            }
        }

        writeTlkUnknownWordReportLocal(opt.outputDir / (file.stem().string() + "_unknown_words.tsv"), st.unknowns);
        stats.push_back(std::move(st));
    }

    writeTlkUnknownWordReportLocal(opt.outputDir / "combined_unknown_words.tsv", combinedUnknowns);

    {
        std::ofstream stubs(opt.outputDir / "combined_unknown_stubs.dict", std::ios::binary);
        if (!stubs) throw std::runtime_error("cannot write combined_unknown_stubs.dict");
        stubs << "# Unknown TLK words not found in the selected dictionary stack. Fill pronunciations after =.\n";
        for (const auto& row : sortedUnknownsLocal(combinedUnknowns)) {
            stubs << row.first << " =  ; # count=" << row.second.count << "\n";
        }
    }

    {
        std::ofstream md(opt.outputDir / "TLK_UNKNOWN_SCAN_SUMMARY.md", std::ios::binary);
        if (!md) throw std::runtime_error("cannot write TLK_UNKNOWN_SCAN_SUMMARY.md");
        md << "# TLK Unknown Word Scan\n\n";
        md << "Generated by `scan-tlk-unknowns` using the same resolver path as `text-to-ntp`.\n\n";
        md << "TLK cleanup: " << (opt.textOptions.cleanupTlkMarkup ? "enabled" : "disabled") << "; brace stage directions: "
           << (opt.textOptions.stripBraceStageDirections ? "stripped" : "preserved") << "; compositional lookup: "
           << (opt.textOptions.useCompositionalPronunciations ? "enabled" : "disabled") << ".\n\n";
        md << "| TLK | Nonempty strings | Token instances | Unknown token instances | Unique unknown words |\n";
        md << "|---|---:|---:|---:|---:|\n";
        for (const auto& st : stats) {
            md << "| " << markdownCellLocal(st.name) << " | "
               << st.nonemptyStrings << " | "
               << st.tokenInstances << " | "
               << st.unknownTokenInstances << " | "
               << st.unknowns.size() << " |\n";
        }
        md << "\n## Top combined unknowns\n\n";
        md << "| Word | Count | Files | Example |\n|---|---:|---|---|\n";
        std::size_t shown = 0;
        for (const auto& row : sortedUnknownsLocal(combinedUnknowns)) {
            if (shown++ >= 50) break;
            const auto& u = row.second;
            std::string example = u.exampleText;
            if (example.size() > 160) example = example.substr(0, 160) + "...";
            md << "| `" << markdownCellLocal(row.first) << "` | " << u.count << " | "
               << markdownCellLocal(joinStringSetLocal(u.files, ", ")) << " | strref "
               << u.exampleStrref << ": " << markdownCellLocal(example) << " |\n";
        }
        md << "\n## Source totals\n\n";
        md << "| Source | Token instances |\n|---|---:|\n";
        for (const auto& kv : combinedSourceCounts) {
            md << "| " << markdownCellLocal(kv.first) << " | " << kv.second << " |\n";
        }
    }

    {
        std::ofstream js(opt.outputDir / "scan_summary.json", std::ios::binary);
        if (!js) throw std::runtime_error("cannot write scan_summary.json");
        js << "{\n";
        js << "  \"input_path\": \"" << jsonEscapeLocal(opt.inputPath.string()) << "\",\n";
        js << "  \"models_root\": \"" << jsonEscapeLocal(opt.faRoot.string()) << "\",\n";
        js << "  \"cleanup_tlk_markup\": " << (opt.textOptions.cleanupTlkMarkup ? "true" : "false") << ",\n";
        js << "  \"strip_brace_stage_directions\": " << (opt.textOptions.stripBraceStageDirections ? "true" : "false") << ",\n";
        js << "  \"use_compositional_pronunciations\": " << (opt.textOptions.useCompositionalPronunciations ? "true" : "false") << ",\n";
        js << "  \"summary\": [\n";
        for (std::size_t i = 0; i < stats.size(); ++i) {
            const auto& st = stats[i];
            js << "    {\"tlk\": \"" << jsonEscapeLocal(st.name) << "\", \"string_count\": " << st.stringCount
               << ", \"nonempty_strings\": " << st.nonemptyStrings
               << ", \"token_instances\": " << st.tokenInstances
               << ", \"known_token_instances\": " << st.knownTokenInstances
               << ", \"unknown_token_instances\": " << st.unknownTokenInstances
               << ", \"unique_unknown_words\": " << st.unknowns.size() << "}";
            js << (i + 1 == stats.size() ? "\n" : ",\n");
        }
        js << "  ],\n";
        js << "  \"source_counts\": {";
        bool first = true;
        for (const auto& kv : combinedSourceCounts) {
            if (!first) js << ", ";
            first = false;
            js << "\"" << jsonEscapeLocal(kv.first) << "\": " << kv.second;
        }
        js << "}\n";
        js << "}\n";
    }

    std::size_t totalStrings = 0, totalTokens = 0, totalUnknowns = 0;
    for (const auto& st : stats) {
        totalStrings += st.nonemptyStrings;
        totalTokens += st.tokenInstances;
        totalUnknowns += st.unknownTokenInstances;
    }
    std::cout << "phase\t41_scan_tlk_unknowns_standalone\n";
    std::cout << "input_path\t" << opt.inputPath.string() << "\n";
    std::cout << "models_root\t" << opt.faRoot.string() << "\n";
    std::cout << "output_dir\t" << opt.outputDir.string() << "\n";
    std::cout << "recursive\t" << (opt.recursive ? "yes" : "no") << "\n";
    std::cout << "tlk_files_seen\t" << files.size() << "\n";
    std::cout << "nonempty_strings\t" << totalStrings << "\n";
    std::cout << "token_instances\t" << totalTokens << "\n";
    std::cout << "unknown_instances\t" << totalUnknowns << "\n";
    std::cout << "unique_unknown_words\t" << combinedUnknowns.size() << "\n";
    std::cout << "combined_unknown_report\t" << (opt.outputDir / "combined_unknown_words.tsv").string() << "\n";
    std::cout << "summary_report\t" << (opt.outputDir / "TLK_UNKNOWN_SCAN_SUMMARY.md").string() << "\n";
    if (!opt.sourceReport.empty()) std::cout << "source_report\t" << opt.sourceReport.string() << "\n";
    std::cout << "verdict\t" << (totalUnknowns == 0 ? "NO_UNKNOWNS" : "UNKNOWN_WORDS") << "\n";
    return totalUnknowns == 0 ? 0 : 1;
}

static int cmdAppendDictStubs(const std::vector<std::string>& args) {
    if (args.size() < 3 || args.size() > 3) {
        usage();
        return 2;
    }
    const fs::path unknownReport = args[1];
    const fs::path dictPath = args[2];
    std::ifstream in(unknownReport, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read unknown report: " + unknownReport.string());

    std::string headerLine;
    if (!std::getline(in, headerLine)) throw std::runtime_error("unknown report is empty: " + unknownReport.string());
    std::vector<std::string> header = splitTabLocal(headerLine);
    const int stubCol = columnIndexByNameLocal(header, {"dictionary_stub", "suggested_override_line"});
    const int wordCol = columnIndexByNameLocal(header, {"word"});
    if (stubCol < 0 && wordCol < 0) {
        throw std::runtime_error("unknown report must contain dictionary_stub, suggested_override_line, or word column");
    }

    std::set<std::string> existingKeys;
    if (fs::exists(dictPath)) {
        std::ifstream din(dictPath, std::ios::binary);
        std::string line;
        while (std::getline(din, line)) {
            std::string key = dictionaryEntryKeyFromLine(line);
            if (!key.empty()) existingKeys.insert(key);
        }
    }

    std::vector<std::string> toAppend;
    std::set<std::string> appendedKeys;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> cols = splitTabLocal(line);
        std::string stub;
        if (stubCol >= 0 && static_cast<std::size_t>(stubCol) < cols.size()) stub = trimLocal(cols[static_cast<std::size_t>(stubCol)]);
        if (stub.empty() && wordCol >= 0 && static_cast<std::size_t>(wordCol) < cols.size()) stub = dictionaryStubForWord(trimLocal(cols[static_cast<std::size_t>(wordCol)]));
        if (stub.empty()) continue;
        if (stub.find('=') == std::string::npos) stub = dictionaryStubForWord(stub);
        if (!stub.empty() && stub.back() != ';') stub += " ;";
        const std::string key = dictionaryEntryKeyFromLine(stub);
        if (key.empty()) continue;
        if (existingKeys.count(key) || appendedKeys.count(key)) continue;
        appendedKeys.insert(key);
        toAppend.push_back(stub);
    }

    if (!dictPath.parent_path().empty()) ensureDirectory(dictPath.parent_path());
    const bool existed = fs::exists(dictPath);
    std::ofstream out(dictPath, std::ios::binary | std::ios::app);
    if (!out) throw std::runtime_error("cannot write override dictionary: " + dictPath.string());
    if (!existed || fs::file_size(dictPath) == 0) {
        out << "# CSLU/KOTOR pronunciation overrides. Fill in phone strings after '='.\n";
        out << "# Example: revan = r E v ^ n ;\n";
    }
    for (const std::string& stub : toAppend) out << stub << "\n";

    std::cout << "phase\t20_append_dict_stubs\n";
    std::cout << "unknown_report\t" << unknownReport.string() << "\n";
    std::cout << "override_dictionary\t" << dictPath.string() << "\n";
    std::cout << "existing_entries\t" << existingKeys.size() << "\n";
    std::cout << "appended_stubs\t" << toAppend.size() << "\n";
    for (const auto& stub : toAppend) std::cout << "appended\t" << stub << "\n";
    std::cout << "verdict\tUPDATED_DICTIONARY\n";
    return 0;
}

static std::atomic<bool> gBatchCancelRequested{false};

static void csluBatchSignalHandler(int) {
    gBatchCancelRequested.store(true);
}

struct BatchAlignOptions {
    fs::path inputDir;
    fs::path outputDir;
    fs::path faRoot;
    std::string inputKind = "text";
    std::string outputKind = "phn";
    bool recursive = false;
    bool writeWorkDirs = true;
    bool failFast = false;
    bool resume = false;
    int jobs = 0; // 0 means auto.
    fs::path batchReport;
    fs::path unknownReport;
    fs::path decodeCacheDir;
    fs::path cancelFile;
    LipTailRestOptions tail;
    LipQualityOptions quality;
    TextToNtpOptions textOptions;
};

struct BatchItemResult {
    fs::path rel;
    fs::path transcript;
    fs::path outNtp;
    fs::path outPhn;
    fs::path outLip;
    fs::path workDir;
    std::string status = "ERROR";
    std::string inputKindUsed;
    std::string outputKind;
    std::string errorMessage;
    std::vector<std::string> unknownWords;
    std::size_t wordCount = 0;
    std::size_t unknownWordCount = 0;
    std::size_t phoneCount = 0;
    std::size_t phnIntervalCount = 0;
    std::size_t lipEntryCount = 0;
    int tailRestMs = 0;
    std::string closeOnPauseMs;
    std::string smoothing;
    int minKeyMs = 0;
    bool preserveClosures = true;
    int anticipateClosuresMs = 0;
    int anticipateFricativesMs = 0;
    int anticipateStopsMs = 0;
    std::size_t frontendFrames = 0;
    int durationMs = 0;
    std::string firstPhone;
    std::string lastPhone;
    std::string sourceCounts;
};

static void writeBatchReportRow(std::ofstream& batch, const BatchItemResult& r) {
    batch << tsvCellLocal(r.rel.generic_string()) << '\t'
          << tsvCellLocal(r.status) << '\t'
          << tsvCellLocal(r.inputKindUsed) << '\t'
          << tsvCellLocal(r.outputKind) << '\t'
          << tsvCellLocal(r.transcript.string()) << '\t'
          << tsvCellLocal(r.outNtp.string()) << '\t'
          << tsvCellLocal((r.outputKind == "lip") ? std::string() : r.outPhn.string()) << '\t'
          << tsvCellLocal((r.outputKind == "phn") ? std::string() : r.outLip.string()) << '\t'
          << tsvCellLocal(r.workDir.string()) << '\t'
          << r.durationMs << '\t'
          << r.frontendFrames << '\t'
          << r.wordCount << '\t'
          << r.unknownWordCount << '\t'
          << r.phoneCount << '\t'
          << r.phnIntervalCount << '\t'
          << r.lipEntryCount << '\t'
          << r.tailRestMs << '\t'
          << tsvCellLocal(r.closeOnPauseMs) << '\t'
          << tsvCellLocal(r.smoothing) << '\t'
          << r.minKeyMs << '\t'
          << (r.preserveClosures ? "yes" : "no") << '\t'
          << r.anticipateClosuresMs << '\t'
          << r.anticipateFricativesMs << '\t'
          << r.anticipateStopsMs << '\t'
          << tsvCellLocal(r.firstPhone) << '\t'
          << tsvCellLocal(r.lastPhone) << '\t'
          << tsvCellLocal(r.sourceCounts) << '\t'
          << tsvCellLocal(r.errorMessage) << '\n';
}

static std::string hexSizeTLocal(std::size_t v) {
    std::ostringstream ss;
    ss << std::hex << v;
    return ss.str();
}

static fs::path cachedDecodePathFor(const fs::path& input, const fs::path& cacheDir) {
    std::error_code ec;
    const auto size = fs::file_size(input, ec);
    const auto t = fs::last_write_time(input, ec).time_since_epoch().count();
    const std::string keySource = input.lexically_normal().generic_string() + "|" + std::to_string(size) + "|" + std::to_string(t);
    const std::size_t h = std::hash<std::string>{}(keySource);
    std::string name = input.stem().string();
    for (char& ch : name) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') ch = '_';
    }
    if (name.empty()) name = "audio";
    return cacheDir / (name + "_" + hexSizeTLocal(h) + "_pcm16mono.wav");
}

static Pcm16Mono readPcm16MonoMaybeCached(const fs::path& input, const fs::path& cacheDir, std::mutex& cacheMutex) {
    if (cacheDir.empty()) return readPcm16Mono(input);
    ensureDirectory(cacheDir);
    const fs::path cachePath = cachedDecodePathFor(input, cacheDir);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (fs::exists(cachePath)) {
            return readPcm16Mono(cachePath);
        }
    }
    Pcm16Mono decoded = readPcm16Mono(input);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (!fs::exists(cachePath)) writePcm16Mono(decoded, cachePath);
    }
    return decoded;
}

static bool isValidExistingPhn(const fs::path& p) {
    try { return fs::exists(p) && !readPhn(p).intervals.empty(); }
    catch (...) { return false; }
}

static bool isValidExistingLip(const fs::path& p) {
    try {
        if (!fs::exists(p)) return false;
        LipFile lip = readLip(p);
        return validateLipForKotorRuntime(lip, fs::file_size(p)).ok;
    } catch (...) { return false; }
}

static bool resumeOutputsAreComplete(const BatchAlignOptions& opt, const fs::path& outPhn, const fs::path& outLip) {
    if (opt.outputKind == "phn") return isValidExistingPhn(outPhn);
    if (opt.outputKind == "lip") return isValidExistingLip(outLip);
    return isValidExistingPhn(outPhn) && isValidExistingLip(outLip);
}

static bool batchCancellationRequested(const BatchAlignOptions& opt) {
    if (gBatchCancelRequested.load()) return true;
    return !opt.cancelFile.empty() && fs::exists(opt.cancelFile);
}

static int resolveBatchJobCount(int requested, std::size_t itemCount) {
    if (itemCount == 0) return 1;
    if (requested > 0) return std::max(1, requested);
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 2;
    return static_cast<int>(std::max<std::size_t>(1, std::min<std::size_t>({itemCount, static_cast<std::size_t>(hw), 4})));
}

static BatchItemResult processOneBatchItem(const fs::path& wav,
                                           const BatchAlignOptions& opt,
                                           const LoadedCsluModelSet& models,
                                           TextToNtpResolver& textResolver,
                                           std::mutex& textMutex,
                                           std::mutex& decodeCacheMutex) {
    BatchItemResult result;
    result.rel = lexicalBatchRelativePath(wav, opt.inputDir);
    result.outputKind = opt.outputKind;
    const fs::path relParent = result.rel.parent_path();
    const std::string stem = wav.stem().string();
    const fs::path outBase = opt.outputDir / relParent / stem;
    result.outNtp = fs::path(outBase.string() + ".ntp");
    result.outPhn = fs::path(outBase.string() + ".phn");
    result.outLip = fs::path(outBase.string() + ".lip");
    result.workDir = opt.writeWorkDirs ? fs::path(outBase.string() + "_work") : fs::path();

    const std::vector<std::string> textExts = {".txt", ".lab"};
    const std::vector<std::string> ntpExts = {".ntp"};

    try {
        requireBatchOutputContained(opt.outputDir, outBase.parent_path());
        ensureDirectory(outBase.parent_path());
        requireBatchOutputContained(opt.outputDir, outBase.parent_path());
        if (!result.workDir.empty()) {
            requireBatchOutputContained(opt.outputDir, result.workDir);
            ensureDirectory(result.workDir);
            requireBatchOutputContained(opt.outputDir, result.workDir);
        }

        if (opt.resume && resumeOutputsAreComplete(opt, result.outPhn, result.outLip)) {
            result.status = "SKIPPED_RESUME";
            result.errorMessage = "requested outputs already exist and validate";
            if (isValidExistingPhn(result.outPhn)) {
                PhnFile phn = readPhn(result.outPhn);
                result.phnIntervalCount = phn.intervals.size();
                result.durationMs = phnDurationMsLocal(phn);
                if (!phn.intervals.empty()) {
                    result.firstPhone = phn.intervals.front().label;
                    result.lastPhone = phn.intervals.back().label;
                }
            }
            if (isValidExistingLip(result.outLip)) result.lipEntryCount = readLip(result.outLip).frames.size();
            return result;
        }

        if (batchCancellationRequested(opt)) {
            result.status = "CANCELLED";
            result.errorMessage = "cancel requested before item started";
            return result;
        }

        if (opt.inputKind == "text" || opt.inputKind == "auto") {
            result.transcript = findSameStemFile(wav, textExts);
            if (!result.transcript.empty()) result.inputKindUsed = "text";
        }
        if (result.transcript.empty() && (opt.inputKind == "ntp" || opt.inputKind == "auto")) {
            result.transcript = findSameStemFile(wav, ntpExts);
            if (!result.transcript.empty()) result.inputKindUsed = "ntp";
        }

        if (result.transcript.empty()) {
            result.status = "MISSING_TRANSCRIPT";
            result.errorMessage = "no same-stem transcript found for input mode " + opt.inputKind;
            return result;
        }

        fs::path ntpForAlignment;
        if (result.inputKindUsed == "text") {
            TextToNtpResult ntpResult;
            {
                std::lock_guard<std::mutex> lock(textMutex);
                ntpResult = textResolver.resolveSource(result.transcript.string());
            }
            result.wordCount = ntpResult.words.size();
            result.unknownWordCount = ntpResult.unknownWords.size();
            result.unknownWords = ntpResult.unknownWords;
            result.sourceCounts = sourceCountsStringLocal(ntpResult);
            if (!result.workDir.empty()) {
                ensureDirectory(result.workDir / "text");
                writeTextToNtpSourceReport(result.workDir / "text" / "pronunciations.tsv", ntpResult);
                writeTextToNtpUnknownReport(result.workDir / "text" / "unknown_words.tsv", ntpResult);
                std::ofstream norm(result.workDir / "text" / "normalized_text.txt");
                norm << ntpResult.normalizedText << "\n";
            }
            if (!ntpResult.unknownWords.empty()) {
                result.status = "UNKNOWN_WORDS";
                result.errorMessage = "unknown words; see unknown report";
                return result;
            }
            writeNtpFile(result.outNtp, ntpResult.ntp);
            ntpForAlignment = result.outNtp;
        } else {
            ntpForAlignment = result.transcript;
            if (result.transcript != result.outNtp) {
                fs::copy_file(result.transcript, result.outNtp, fs::copy_options::overwrite_existing);
            }
        }

        if (batchCancellationRequested(opt)) {
            result.status = "CANCELLED";
            result.errorMessage = "cancel requested before alignment";
            return result;
        }

        const Pcm16Mono original = readPcm16MonoMaybeCached(wav, opt.decodeCacheDir, decodeCacheMutex);
        FullFrontendFeatures frontend = computeFullFrontendFromOriginalWav(original);
        EndToEndAlignmentResult r = runEndToEndFromFrontend(frontend, ntpForAlignment, opt.faRoot, models);
        const bool writePhnOutput = (opt.outputKind == "phn" || opt.outputKind == "both");
        const bool writeLipOutput = (opt.outputKind == "lip" || opt.outputKind == "both");
        fs::path workPhn = result.outPhn;
        if (!writePhnOutput && !result.workDir.empty()) workPhn = result.workDir / "output" / "phn.txt";
        if (writePhnOutput || !result.workDir.empty()) writePhnEnsuringParent(workPhn, r.alignment.phn);
        if (writeLipOutput) {
            result.tailRestMs = resolveTailRestMs(phnDurationMsLocal(r.alignment.phn), opt.tail);
            result.closeOnPauseMs = lipCloseOnPauseSetting(opt.quality);
            result.smoothing = lipSmoothingPresetName(opt.quality.smoothing);
            result.minKeyMs = opt.quality.minKeyMs;
            result.preserveClosures = opt.quality.preserveClosures;
            result.anticipateClosuresMs = opt.quality.anticipateClosuresMs;
            result.anticipateFricativesMs = opt.quality.anticipateFricativesMs;
            result.anticipateStopsMs = opt.quality.anticipateStopsMs;
            LipFile lip = writeLipFromPhnToPath(r.alignment.phn, result.outLip, opt.tail, opt.quality);
            result.lipEntryCount = lip.frames.size();
        }
        writeEndToEndWorkDir(r, result.workDir, opt.faRoot, writePhnOutput ? result.outPhn : workPhn);
        result.status = writeLipOutput ? (writePhnOutput ? "WROTE_BOTH" : "WROTE_LIP") : "WROTE_PHN";
        result.frontendFrames = r.frontend.netin.rows;
        result.phoneCount = r.graphBundle.phones.size();
        result.phnIntervalCount = r.alignment.phn.intervals.size();
        result.durationMs = phnDurationMsLocal(r.alignment.phn);
        if (!r.alignment.phn.intervals.empty()) {
            result.firstPhone = r.alignment.phn.intervals.front().label;
            result.lastPhone = r.alignment.phn.intervals.back().label;
        }
    } catch (const std::exception& e) {
        result.status = "ERROR";
        result.errorMessage = e.what();
    }
    return result;
}

static int cmdBatchAlign(const std::vector<std::string>& args) {
    if (args.size() < 4) {
        usage();
        return 2;
    }
    gBatchCancelRequested.store(false);
    std::signal(SIGINT, csluBatchSignalHandler);
    std::signal(SIGTERM, csluBatchSignalHandler);

    BatchAlignOptions opt;
    opt.inputDir = args[1];
    opt.outputDir = args[2];
    const bool newStyle = hasFlag(args, "--models") || hasFlag(args, "--model-root") || hasFlag(args, "--package");
    std::size_t flagStart;
    if (newStyle) {
        flagStart = 3;
        opt.faRoot = readModelsFlag(args, flagStart);
    } else {
        opt.faRoot = resolveModelRoot(args[3]);
        flagStart = 4;
    }
    if (opt.faRoot.empty()) throw std::runtime_error("batch-align requires --models <models> in standalone syntax");
    opt.textOptions = makeDefaultTextToNtpOptions(opt.faRoot);
    opt.textOptions.mode = NtpOutputMode::SimpleLinear;
    opt.textOptions.failOnUnknown = false;

    TextCommandExtraOptions ignoredTextExtra;
    fs::path ignoredWorkDir;
    for (std::size_t i = flagStart; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--input") {
            if (i + 1 >= args.size()) throw std::runtime_error("--input requires text, ntp, or auto");
            opt.inputKind = lowerAsciiLocal(args[++i]);
            if (opt.inputKind != "text" && opt.inputKind != "ntp" && opt.inputKind != "auto") throw std::runtime_error("--input must be text, ntp, or auto");
        } else if (a == "--output") {
            if (i + 1 >= args.size()) throw std::runtime_error("--output requires phn, lip, or both");
            opt.outputKind = lowerAsciiLocal(args[++i]);
            if (opt.outputKind != "phn" && opt.outputKind != "lip" && opt.outputKind != "both") throw std::runtime_error("--output must be phn, lip, or both");
        } else if (consumeLipTailRestFlag(args, i, opt.tail)) {
            // consumed tail-rest option
        } else if (consumeLipQualityFlag(args, i, opt.quality)) {
            // consumed lip quality option
        } else if (a == "--recursive") {
            opt.recursive = true;
        } else if (a == "--no-workdirs") {
            opt.writeWorkDirs = false;
        } else if (a == "--workdirs") {
            opt.writeWorkDirs = true;
        } else if (a == "--fail-fast") {
            opt.failFast = true;
        } else if (a == "--keep-going") {
            opt.failFast = false;
        } else if (a == "--resume") {
            opt.resume = true;
        } else if (a == "--no-resume") {
            opt.resume = false;
        } else if (a == "--jobs" || a == "-j") {
            if (i + 1 >= args.size()) throw std::runtime_error("--jobs requires a number or auto");
            std::string v = lowerAsciiLocal(args[++i]);
            opt.jobs = (v == "auto") ? 0 : std::stoi(v);
            if (opt.jobs < 0) throw std::runtime_error("--jobs must be positive or auto");
        } else if (a == "--decode-cache-dir") {
            if (i + 1 >= args.size()) throw std::runtime_error("--decode-cache-dir requires a path");
            opt.decodeCacheDir = args[++i];
        } else if (a == "--cancel-file") {
            if (i + 1 >= args.size()) throw std::runtime_error("--cancel-file requires a path");
            opt.cancelFile = args[++i];
        } else if (a == "--batch-report") {
            if (i + 1 >= args.size()) throw std::runtime_error("--batch-report requires a path");
            opt.batchReport = args[++i];
        } else if (a == "--unknown-report") {
            if (i + 1 >= args.size()) throw std::runtime_error("--unknown-report requires a path");
            opt.unknownReport = args[++i];
        } else if (a == "--models" || a == "--model-root" || a == "--package") {
            applyTextToNtpFlag(opt.textOptions, ignoredTextExtra, args, i);
            opt.faRoot = opt.textOptions.faRoot;
            opt.textOptions.mode = NtpOutputMode::SimpleLinear;
            opt.textOptions.failOnUnknown = false;
        } else if (consumeModelOrWorkFlag(args, i, opt.faRoot, ignoredWorkDir)) {
            opt.textOptions.mode = NtpOutputMode::SimpleLinear;
            opt.textOptions.failOnUnknown = false;
        } else {
            applyTextToNtpFlag(opt.textOptions, ignoredTextExtra, args, i);
        }
    }

    if (opt.batchReport.empty()) opt.batchReport = opt.outputDir / "batch_report.tsv";
    if (opt.unknownReport.empty()) opt.unknownReport = opt.outputDir / "unknown_words.tsv";
    ensureDirectory(opt.outputDir);
    ensureDirectory(opt.batchReport.parent_path());
    ensureDirectory(opt.unknownReport.parent_path());
    if (!opt.decodeCacheDir.empty()) ensureDirectory(opt.decodeCacheDir);

    const std::vector<fs::path> wavs = collectBatchWavs(opt.inputDir, opt.recursive);
    const int jobs = resolveBatchJobCount(opt.jobs, wavs.size());

    // Load expensive shared resources once for the batch. The text resolver owns
    // the CSLU/OGI dictionaries and the model set owns all six .45 networks.
    LoadedCsluModelSet models = LoadedCsluModelSet::load(opt.faRoot);
    TextToNtpResolver textResolver(opt.textOptions);

    std::ofstream batch(opt.batchReport, std::ios::binary);
    if (!batch) throw std::runtime_error("cannot write batch report: " + opt.batchReport.string());
    std::ofstream unknown(opt.unknownReport, std::ios::binary);
    if (!unknown) throw std::runtime_error("cannot write unknown report: " + opt.unknownReport.string());
    writeBatchReportHeader(batch);
    unknown << "relative_wav\tword\tdictionary_stub\n";
    batch.flush();
    unknown.flush();

    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> okCount{0}, unknownCountFiles{0}, missingCount{0}, errorCount{0}, skippedCount{0}, cancelledCount{0};
    std::mutex textMutex;
    std::mutex decodeCacheMutex;
    std::mutex reportMutex;
    std::mutex coutMutex;

    auto recordResult = [&](const BatchItemResult& r) {
        if (r.status.rfind("WROTE_", 0) == 0) ++okCount;
        else if (r.status == "UNKNOWN_WORDS") ++unknownCountFiles;
        else if (r.status == "MISSING_TRANSCRIPT") ++missingCount;
        else if (r.status == "SKIPPED_RESUME") ++skippedCount;
        else if (r.status == "CANCELLED") ++cancelledCount;
        else ++errorCount;

        {
            std::lock_guard<std::mutex> lock(reportMutex);
            writeBatchReportRow(batch, r);
            for (const auto& w : r.unknownWords) {
                unknown << tsvCellLocal(r.rel.generic_string()) << '\t'
                        << tsvCellLocal(w) << '\t'
                        << tsvCellLocal(w + " =  ;") << '\n';
            }
            batch.flush();
            unknown.flush();
        }
        {
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cout << "batch_item\t" << r.rel.generic_string() << "\t" << r.status;
            if (!r.errorMessage.empty()) std::cout << "\t" << r.errorMessage;
            std::cout << "\n" << std::flush;
        }
        if (opt.failFast && r.status.rfind("WROTE_", 0) != 0 && r.status != "SKIPPED_RESUME") {
            gBatchCancelRequested.store(true);
        }
    };

    auto worker = [&]() {
        while (!gBatchCancelRequested.load()) {
            if (batchCancellationRequested(opt)) {
                gBatchCancelRequested.store(true);
                break;
            }
            const std::size_t i = next.fetch_add(1);
            if (i >= wavs.size()) break;
            BatchItemResult r = processOneBatchItem(wavs[i], opt, models, textResolver, textMutex, decodeCacheMutex);
            recordResult(r);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(jobs));
    for (int j = 0; j < jobs; ++j) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    // Mark items that were never started after a cancel/fail-fast request so the
    // partial report is explicit and resume can be used cleanly later.
    while (true) {
        const std::size_t i = next.fetch_add(1);
        if (i >= wavs.size()) break;
        BatchItemResult r;
        r.rel = lexicalBatchRelativePath(wavs[i], opt.inputDir);
        r.outputKind = opt.outputKind;
        r.status = "CANCELLED";
        r.errorMessage = "batch cancelled before item started";
        recordResult(r);
    }

    std::cout << "phase\t36_batch_performance\n";
    std::cout << "input_dir\t" << opt.inputDir.string() << "\n";
    std::cout << "output_dir\t" << opt.outputDir.string() << "\n";
    std::cout << "models_root\t" << opt.faRoot.string() << "\n";
    std::cout << "input_kind\t" << opt.inputKind << "\n";
    std::cout << "output_kind\t" << opt.outputKind << "\n";
    std::cout << "jobs\t" << jobs << "\n";
    std::cout << "resume\t" << (opt.resume ? "yes" : "no") << "\n";
    if (!opt.decodeCacheDir.empty()) std::cout << "decode_cache_dir\t" << opt.decodeCacheDir.string() << "\n";
    if (!opt.cancelFile.empty()) std::cout << "cancel_file\t" << opt.cancelFile.string() << "\n";
    std::cout << "tail_rest_mode\t" << lipTailRestModeName(opt.tail) << "\n";
    std::cout << "tail_rest_ms_setting\t" << (opt.tail.automatic ? 0 : opt.tail.fixedMs) << "\n";
    std::cout << "close_on_pause_ms\t" << lipCloseOnPauseSetting(opt.quality) << "\n";
    std::cout << "smoothing\t" << lipSmoothingPresetName(opt.quality.smoothing) << "\n";
    std::cout << "min_key_ms\t" << opt.quality.minKeyMs << "\n";
    std::cout << "preserve_closures\t" << (opt.quality.preserveClosures ? "yes" : "no") << "\n";
    printLipAnticipationSummary(opt.quality);
    std::cout << "recursive\t" << (opt.recursive ? "yes" : "no") << "\n";
    std::cout << "write_workdirs\t" << (opt.writeWorkDirs ? "yes" : "no") << "\n";
    std::cout << "batch_report\t" << opt.batchReport.string() << "\n";
    std::cout << "unknown_report\t" << opt.unknownReport.string() << "\n";
    std::cout << "files_seen\t" << wavs.size() << "\n";
    std::cout << "files_succeeded\t" << okCount.load() << "\n";
    std::cout << "files_skipped_resume\t" << skippedCount.load() << "\n";
    std::cout << "files_unknown_words\t" << unknownCountFiles.load() << "\n";
    std::cout << "files_missing_transcript\t" << missingCount.load() << "\n";
    std::cout << "files_cancelled\t" << cancelledCount.load() << "\n";
    std::cout << "files_errors\t" << errorCount.load() << "\n";
    const bool pass = (!wavs.empty() && (okCount.load() + skippedCount.load()) == wavs.size());
    std::cout << "verdict\t" << (pass ? "PASS" : (gBatchCancelRequested.load() ? "CANCELLED" : "CHECK_REPORT")) << "\n";
    return (errorCount.load() == 0 && missingCount.load() == 0 && unknownCountFiles.load() == 0 && cancelledCount.load() == 0) ? 0 : 1;
}

static int cmdCompareEndToEndTrace(const std::vector<std::string>& args) {
    if (args.size() < 4 || args.size() > 5) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const fs::path faRoot = args[2];
    const fs::path outPath = args[3];
    const fs::path workDir = (args.size() == 5) ? fs::path(args[4]) : fs::path();

    FullFrontendFeatures frontend = buildFullFrontendFromTracePreDcWavs(dump);
    const fs::path tempNtp = dump / "text" / "ntp_original.txt";
    EndToEndAlignmentResult r = runEndToEndFromFrontend(frontend, tempNtp, faRoot, readViterbiOptionsFromTrace(dump));
    writePhn(outPath, r.alignment.phn);
    writeEndToEndWorkDir(r, workDir, faRoot, outPath);

    printEndToEndSummary(r, outPath, workDir, "trace_pre_dc_16k_8k");
    const PhnFile expected = phnFromTraceLabelList(dump / "viterbi" / "phn_labels_after_undo.list.tsv");
    std::string reason;
    const bool match = phnFilesEqual(r.alignment.phn, expected, &reason);
    std::cout << "match_trace_phn\t" << (match ? "yes" : "no") << "\n";
    if (!match) {
        std::cout << "mismatch_reason\t" << reason << "\n";
    }

    std::cout << "\nfrontend_checks\n";
    printMatrixCompare("features/plp_feat_16k.end_to_end", compareMatrixValues(r.frontend.plpFeat16k, readDumpMatrix(dump, "features/plp_feat_16k")), 1e-5, 1e-4);
    std::cout << "\n";
    printMatrixCompare("features/burst_pulses_col7.end_to_end", compareMatrixValues(r.frontend.burstPulsesCol7, readDumpMatrix(dump, "features/burst_pulses_col7")), 1e-3, 2e-1);
    std::cout << "\n";
    printMatrixCompare("features/intensity_0_15.end_to_end", compareMatrixValues(r.frontend.intensity0To15, readDumpMatrix(dump, "features/intensity_0_15")), 1e-6, 1e-4);
    std::cout << "\n";
    printMatrixCompare("netin/manner.end_to_end", compareMatrixValues(r.frontend.netin, readDumpMatrix(dump, "netin/manner")), 1e-4, 2e-1);
    std::cout << "\n";
    const TraceGraph expectedGraph = readTraceGraph(dump);
    const Matrix phnRemap = remapMatrixColumnsByLabels(r.probabilities.phnProb, r.graphBundle.graph.uniqueStates, expectedGraph.uniqueStates);
    const Matrix transRemap = remapMatrixColumnsByLabels(r.probabilities.transProb, r.graphBundle.graph.phnToFrom, expectedGraph.phnToFrom);
    printMatrixCompare("prob/phnProb.end_to_end.remapped_to_trace_order", compareMatrixValues(phnRemap, readDumpMatrix(dump, "prob/phnProb")), 1e-3, 2e-1);
    std::cout << "\n";
    printMatrixCompare("prob/transProb.end_to_end.remapped_to_trace_order", compareMatrixValues(transRemap, readDumpMatrix(dump, "prob/transProb")), 1e-3, 2e-1);

    std::cout << "verdict\t" << (match ? "PASS" : "CHECK") << "\n";
    return match ? 0 : 1;
}

static int cmdComposeFeatureStage(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        usage();
        return 2;
    }
    const fs::path dump = args[1];
    const fs::path outDir = args[2];
    const Matrix allFeat = composeAllFeatFromDump(dump);
    const Matrix netin = composeNetInFromAllFeat(allFeat);

    writeNumericTsv(allFeat, outDir / "features" / "all_feat.tsv");
    for (const char* name : {"manner", "place", "height", "trans"}) {
        writeNumericTsv(netin, outDir / "netin" / (std::string(name) + ".tsv"));
    }

    std::cout << "out_dir\t" << outDir.string() << "\n";
    std::cout << "all_feat_rows\t" << allFeat.rows << "\n";
    std::cout << "all_feat_cols\t" << allFeat.cols << "\n";
    std::cout << "netin_rows\t" << netin.rows << "\n";
    std::cout << "netin_cols\t" << netin.cols << "\n";
    return 0;
}



struct KotorDictionaryInspection {
    std::size_t entries = 0;
    std::size_t malformed = 0;
    std::size_t emptyPronunciations = 0;
    std::size_t duplicates = 0;
};

static std::string trimAsciiLocal(std::string s) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static KotorDictionaryInspection inspectKotorDictionaryFile(const fs::path& path) {
    KotorDictionaryInspection r;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        r.malformed = 1;
        return r;
    }
    std::set<std::string> seen;
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trimAsciiLocal(line);
        if (line.empty()) continue;
        const auto eq = line.find('=');
        const auto semi = line.find(';', eq == std::string::npos ? 0 : eq + 1);
        if (eq == std::string::npos || semi == std::string::npos || eq == 0 || semi <= eq + 1) {
            ++r.malformed;
            continue;
        }
        std::string word = trimAsciiLocal(line.substr(0, eq));
        std::string pron = trimAsciiLocal(line.substr(eq + 1, semi - eq - 1));
        if (word.empty()) {
            ++r.malformed;
            continue;
        }
        if (pron.empty()) ++r.emptyPronunciations;
        if (!seen.insert(word).second) ++r.duplicates;
        ++r.entries;
    }
    return r;
}


static int cmdValidatePackage(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        usage();
        return 2;
    }
    const fs::path packageRoot = args[1];
    const fs::path models = resolveModelRoot(packageRoot);
    const fs::path root = models.parent_path();
    const fs::path dicts = root / "dictionaries";
    const fs::path docs = root / "docs";
    const fs::path licenses = docs / "licenses";

    struct Requirement {
        fs::path path;
        bool directory;
        bool non_empty;
    };
    const std::vector<Requirement> required = {
        {models / "phonemes_new_16k.train.spec", false, true},
        {models / "phonemes_new_16k.parts", false, true},
        {models / "bc_16k", true, true},
        {models / "bc_16k" / "manner_nnet.45", false, true},
        {models / "bc_16k" / "place_nnet.45", false, true},
        {models / "bc_16k" / "height_nnet.45", false, true},
        {models / "bc_16k" / "manner.train.spec", false, true},
        {models / "bc_16k" / "place.train.spec", false, true},
        {models / "bc_16k" / "height.train.spec", false, true},
        {models / "bc_16k" / "manner.map", false, true},
        {models / "bc_16k" / "place.map", false, true},
        {models / "bc_16k" / "height.map", false, true},
        {models / "bc_16k" / "manner.parts", false, true},
        {models / "bc_16k" / "place.parts", false, true},
        {models / "bc_16k" / "height.parts", false, true},
        {models / "trans_16k", true, true},
        {models / "trans_16k" / "manner_nnet.45", false, true},
        {models / "trans_16k" / "place_nnet.45", false, true},
        {models / "trans_16k" / "height_nnet.45", false, true},
        {models / "trans_16k" / "manner.train.spec", false, true},
        {models / "trans_16k" / "place.train.spec", false, true},
        {models / "trans_16k" / "height.train.spec", false, true},
        {dicts / "custom_dictionary.txt", false, true},
        {dicts / "cmu", true, true},
        {dicts / "ogi_lex.out", false, true},
        {dicts / "ogi_lex_addenda.scm", false, true},
        {dicts / "kotor_override.dict", false, true},
        {docs / "THIRD_PARTY_NOTICES.md", false, true},
        {licenses, true, true}
    };

    auto fileOk = [](const fs::path& p, bool nonEmpty) {
        std::error_code ec;
        if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) return false;
        return !nonEmpty || fs::file_size(p, ec) > 0;
    };
    auto dirOk = [](const fs::path& p, bool nonEmpty) {
        std::error_code ec;
        if (!fs::exists(p, ec) || !fs::is_directory(p, ec)) return false;
        if (!nonEmpty) return true;
        return fs::directory_iterator(p, ec) != fs::directory_iterator();
    };
    auto countFilesWithExtension = [](const fs::path& dir, const std::string& ext) {
        std::size_t count = 0;
        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return count;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (entry.is_regular_file() && entry.path().extension() == ext) ++count;
        }
        return count;
    };
    auto countFiles = [](const fs::path& dir) {
        std::size_t count = 0;
        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return count;
        for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
            if (ec) break;
            if (entry.is_regular_file()) ++count;
        }
        return count;
    };

    std::size_t missing = 0;
    std::cout << "phase\tcheck_bundle\n";
    std::cout << "package_root\t" << packageRoot.string() << "\n";
    std::cout << "models_root\t" << models.string() << "\n";
    std::cout << "dictionaries_root\t" << dicts.string() << "\n";
    for (const auto& r : required) {
        const bool ok = r.directory ? dirOk(r.path, r.non_empty) : fileOk(r.path, r.non_empty);
        std::cout << "asset\t" << (ok ? "OK" : "MISSING") << "\t" << r.path.string() << "\n";
        if (!ok) ++missing;
    }
    const std::size_t cmuObFiles = countFilesWithExtension(dicts / "cmu", ".ob");
    const std::size_t licenseFiles = countFiles(licenses);
    const KotorDictionaryInspection kotorDict = inspectKotorDictionaryFile(dicts / "kotor_override.dict");
    constexpr std::size_t kMinimumRuntimeKotorEntries = 1000;
    std::cout << "cmu_ob_files\t" << cmuObFiles << "\n";
    std::cout << "license_files\t" << licenseFiles << "\n";
    std::cout << "kotor_override_entries\t" << kotorDict.entries << "\n";
    std::cout << "kotor_override_minimum_entries\t" << kMinimumRuntimeKotorEntries << "\n";
    std::cout << "kotor_override_duplicates\t" << kotorDict.duplicates << "\n";
    std::cout << "kotor_override_empty_pronunciations\t" << kotorDict.emptyPronunciations << "\n";
    std::cout << "kotor_override_malformed_rows\t" << kotorDict.malformed << "\n";
    constexpr std::size_t kMinimumLicenseFiles = 6;
    std::cout << "license_files_minimum\t" << kMinimumLicenseFiles << "\n";
    if (cmuObFiles < 26) ++missing;
    if (licenseFiles < kMinimumLicenseFiles) ++missing;
    if (kotorDict.entries < kMinimumRuntimeKotorEntries || kotorDict.duplicates || kotorDict.emptyPronunciations || kotorDict.malformed) ++missing;

    TextToNtpOptions opts = makeDefaultTextToNtpOptions(models);
    std::cout << "resolved_custom_dictionary\t" << opts.customDictionary.string() << "\n";
    std::cout << "resolved_cmu_dictionary_dir\t" << opts.cmuDictionaryDir.string() << "\n";
    std::cout << "resolved_ogi_lexicon\t" << opts.ogiLexiconPath.string() << "\n";
    std::cout << "resolved_ogi_addenda\t" << opts.ogiAddendaPath.string() << "\n";
    std::cout << "redistribution_notice\tPreserve docs/THIRD_PARTY_NOTICES.md and docs/licenses; confirm CSLU redistribution terms before public binary release.\n";
    std::cout << "missing_assets\t" << missing << "\n";
    std::cout << "verdict\t" << (missing == 0 ? "PASS" : "MISSING_ASSETS") << "\n";
    return missing == 0 ? 0 : 1;
}

static void writeStandaloneUsageDoc(const fs::path& path) {
    ensureDirectory(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write usage doc: " + path.string());
    out << "# NeoLIP standalone GUI package\n\n";
    out << "This package contains the NeoLIP GUI and the assets needed for CSLU-compatible KOTOR LIP generation.\n\n";
    out << "## Layout\n\n";
    out << "```text\n";
    out << "bin/                  neolip_gui.exe plus its private neolip_backend.exe helper\n";
    out << "models/               CSLU FA acoustic models/specs/maps\n";
    out << "dictionaries/         CSLU dictionaries plus kotor_override.dict\n";
    out << "examples/             bundled CSLU test WAV/TXT/NTP files\n";
    out << "docs/                 usage notes\n";
    out << "```\n\n";
    out << "## Start NeoLIP\n\n";
    out << "Launch `bin/neolip_gui` on Linux/macOS or `bin\\neolip_gui.exe` on Windows. Select this package root (or its `models` folder) under Package/models.\n\n";
    out << "Use the Align, Batch, Unknown words, PHN preview, LIP editor, LIP timeline, Text to NTP, and Dictionary lookup tabs for all supported workflows.\n\n";
    out << "`neolip_backend` is a private worker used by the GUI for cancellable background jobs. Do not launch or redistribute it separately from the GUI.\n\n";
    out << "Preserve docs/THIRD_PARTY_NOTICES.md and docs/licenses/ when redistributing a bundle that includes CSLU/OGI/Festival assets.\n";
}

static int cmdPackageAssets(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        usage();
        return 2;
    }
    const fs::path faRoot = inferFaRootFromSource(args[1]);
    const fs::path csluRoot = inferCsluInstallRootFromFa(faRoot);
    const fs::path packageRoot = args[2];
    fs::path backendExe;
    fs::path guiExe;
    fs::path kotorDictOverride;
    bool copyExamples = true;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--backend") {
            if (i + 1 >= args.size()) throw std::runtime_error("--backend requires a path");
            backendExe = args[++i];
        } else if (args[i] == "--gui") {
            if (i + 1 >= args.size()) throw std::runtime_error("--gui requires a path");
            guiExe = args[++i];
        } else if (args[i] == "--kotor-dict") {
            if (i + 1 >= args.size()) throw std::runtime_error("--kotor-dict requires a path");
            kotorDictOverride = args[++i];
        } else if (args[i] == "--no-examples") {
            copyExamples = false;
        } else if (args[i] == "--examples") {
            copyExamples = true;
        } else {
            throw std::runtime_error("unknown bundle-assets/package-assets option: " + args[i]);
        }
    }

    const fs::path models = packageRoot / "models";
    const fs::path dicts = packageRoot / "dictionaries";
    const fs::path examples = packageRoot / "examples";
    const fs::path docs = packageRoot / "docs";
    const fs::path bin = packageRoot / "bin";
    const fs::path kotorDictSourceRoot = !kotorDictOverride.empty() ? canonicalIfPossible(kotorDictOverride).parent_path().parent_path() : fs::path();
    ensureDirectory(models);
    ensureDirectory(dicts);
    ensureDirectory(docs);
    ensureDirectory(bin);

    copyFileIfExists(faRoot / "phonemes_new_16k.train.spec", models / "phonemes_new_16k.train.spec");
    copyFileIfExists(faRoot / "phonemes_new_16k.parts", models / "phonemes_new_16k.parts");
    copyDirectoryRecursiveIfExists(faRoot / "bc_16k", models / "bc_16k");
    copyDirectoryRecursiveIfExists(faRoot / "trans_16k", models / "trans_16k");

    copyFileIfExists(faRoot / "custom_dictionary.txt", dicts / "custom_dictionary.txt", true);
    copyDirectoryRecursiveIfExists(faRoot / ".." / ".." / "script" / "dictionary_1.0" / "cmu", dicts / "cmu", true);
    copyFileIfExists(csluRoot / "Festival" / "1.4.2" / "festival" / "lib" / "dicts" / "ogi_lex.out", dicts / "ogi_lex.out", true);
    copyFileIfExists(csluRoot / "Festival" / "1.4.2" / "festival" / "lib" / "ogi_lex_addenda.scm", dicts / "ogi_lex_addenda.scm", true);
    if (!fs::exists(dicts / "kotor_override.dict")) {
        fs::path sourceKotorDict = !kotorDictOverride.empty() ? kotorDictOverride : findBundledKotorOverrideDictionary();
        if (sourceKotorDict.empty() || !fs::exists(sourceKotorDict) || !fs::is_regular_file(sourceKotorDict)) {
            throw std::runtime_error("missing KOTOR override dictionary; run bundle-assets from the NeoLIP source root or pass --kotor-dict <path>");
        }
        copyFileIfExists(sourceKotorDict, dicts / "kotor_override.dict", false);
    }
    const KotorDictionaryInspection packagedKotorDict = inspectKotorDictionaryFile(dicts / "kotor_override.dict");
    if (packagedKotorDict.entries < 1000 || packagedKotorDict.duplicates || packagedKotorDict.emptyPronunciations || packagedKotorDict.malformed) {
        throw std::runtime_error("packaged kotor_override.dict failed runtime dictionary validation");
    }

    if (copyExamples) {
        ensureDirectory(examples);
        copyFileIfExists(faRoot / "test.wav", examples / "test.wav", false);
        copyFileIfExists(faRoot / "test.txt", examples / "test.txt", false);
        copyFileIfExists(faRoot / "test_manual.ntp", examples / "test_manual.ntp", false);
        copyFileIfExists(faRoot / "test_automatic.ntp", examples / "test_automatic.ntp", false);
    }

    if (!backendExe.empty()) copyFileIfExists(backendExe, bin / backendExe.filename(), false);
    if (!guiExe.empty()) copyFileIfExists(guiExe, bin / guiExe.filename(), false);
    if (!fs::exists(bin / "README.txt")) {
        std::ofstream br(bin / "README.txt", std::ios::binary);
        br << "Keep neolip_gui.exe and its private neolip_backend.exe helper together in this folder.\n";
    }

    copyFileIfExists(kotorDictSourceRoot / "tools" / "RUN_SMOKE_TESTS.sh", packageRoot / "RUN_SMOKE_TESTS.sh", false);
    copyFileIfExists(kotorDictSourceRoot / "tools" / "RUN_SMOKE_TESTS.ps1", packageRoot / "RUN_SMOKE_TESTS.ps1", false);
    {
        std::error_code ec;
        fs::permissions(packageRoot / "RUN_SMOKE_TESTS.sh",
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add,
                        ec);
    }

    writeStandaloneUsageDoc(docs / "usage.md");
    {
        const fs::path sourceDocs = kotorDictSourceRoot / "docs";
        const std::vector<std::string> extraDocs = {
            "CSLU_ASSETS.md"
        };
        for (const auto& name : extraDocs) {
            copyFileIfExists(sourceDocs / name, docs / name, false);
        }
    }
    const std::vector<fs::path> noticeCandidates = {
        kotorDictSourceRoot / "docs" / "THIRD_PARTY_NOTICES.md",
        fs::path(CSLU_SOURCE_DOCS_DIR) / "THIRD_PARTY_NOTICES.md",
        fs::path(CSLU_BUNDLED_DICTIONARY_DIR).parent_path() / "docs" / "THIRD_PARTY_NOTICES.md",
        fs::current_path() / "docs" / "THIRD_PARTY_NOTICES.md",
        fs::current_path() / ".." / "docs" / "THIRD_PARTY_NOTICES.md"
    };
    bool copiedThirdPartyNotice = false;
    for (const auto& candidate : noticeCandidates) {
        if (!candidate.empty() && fs::exists(candidate) && fs::is_regular_file(candidate)) {
            copyFileIfExists(candidate, docs / "THIRD_PARTY_NOTICES.md", false);
            copiedThirdPartyNotice = true;
            break;
        }
    }
    if (!copiedThirdPartyNotice) {
        std::ofstream notice(docs / "THIRD_PARTY_NOTICES.md", std::ios::binary);
        notice << "# Third-party notices\n\n";
        notice << "This runtime package includes a KOTOR pronunciation override dictionary. Preserve source-tree third-party notices and attribution files when redistributing dictionary-derived content.\n";
    }

    const std::vector<fs::path> attributionDirCandidates = {
        kotorDictSourceRoot / "docs" / "attribution",
        fs::path(CSLU_SOURCE_DOCS_DIR) / "attribution",
        fs::path(CSLU_BUNDLED_DICTIONARY_DIR).parent_path() / "docs" / "attribution",
        fs::current_path() / "docs" / "attribution",
        fs::current_path() / ".." / "docs" / "attribution"
    };
    for (const auto& candidate : attributionDirCandidates) {
        if (!candidate.empty() && fs::exists(candidate) && fs::is_directory(candidate)) {
            copyDirectoryRecursiveIfExists(candidate, docs / "attribution", false);
            break;
        }
    }

    const std::vector<fs::path> licenseDirCandidates = {
        kotorDictSourceRoot / "docs" / "licenses",
        fs::path(CSLU_SOURCE_DOCS_DIR) / "licenses",
        fs::path(CSLU_BUNDLED_DICTIONARY_DIR).parent_path() / "docs" / "licenses",
        fs::current_path() / "docs" / "licenses",
        fs::current_path() / ".." / "docs" / "licenses"
    };
    bool copiedLicenseDir = false;
    for (const auto& candidate : licenseDirCandidates) {
        if (!candidate.empty() && fs::exists(candidate) && fs::is_directory(candidate)) {
            copyDirectoryRecursiveIfExists(candidate, docs / "licenses", false);
            copiedLicenseDir = true;
            break;
        }
    }
    if (!copiedLicenseDir) {
        ensureDirectory(docs / "licenses");
        std::ofstream note(docs / "licenses" / "LICENSE_FILES_MISSING.txt", std::ios::binary);
        note << "License files were not found next to this NeoLIP build. Preserve original CSLU/OGI/Festival license files when redistributing asset-bundled packages.\n";
    }

    std::ofstream manifest(packageRoot / "manifest.tsv", std::ios::binary);
    manifest << "key\tvalue\n";
    manifest << "phase\tbundle_assets\n";
    manifest << "source_fa_root\t<local CSLU Toolkit apps/fa path omitted>\n";
    manifest << "source_cslu_root\t<local CSLU Toolkit root omitted>\n";
    manifest << "models_root\tmodels\n";
    manifest << "dictionaries_root\tdictionaries\n";
    manifest << "examples_copied\t" << (copyExamples ? "yes" : "no") << "\n";

    std::cout << "phase\tbundle_assets\n";
    std::cout << "source_fa_root\t<local CSLU Toolkit apps/fa path omitted>\n";
    std::cout << "package_root\t" << packageRoot.string() << "\n";
    std::cout << "models_root\t" << models.string() << "\n";
    std::cout << "dictionaries_root\t" << dicts.string() << "\n";
    std::cout << "usage_doc\t" << (docs / "usage.md").string() << "\n";
    std::cout << "verdict\tWROTE_PACKAGE\n";
    return 0;
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage();
            return 2;
        }
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }
        const std::string& cmd = args[0];
        if (cmd == "help" || cmd == "--help" || cmd == "-h") { usage(); return 0; }
        if (cmd == "version" || cmd == "--version" || cmd == "-V" || cmd == "-v") return cmdVersion(args);
        if (cmd == "bundle-assets" || cmd == "package-assets") return cmdPackageAssets(args);
        if (cmd == "check-bundle" || cmd == "validate-package") return cmdValidatePackage(args);
        if (cmd == "validate-trace") return cmdValidateTrace(args);
        if (cmd == "matrix-stats") return cmdMatrixStats(args);
        if (cmd == "inspect-model") return cmdInspectModel(args);
        if (cmd == "compare-nnet") return cmdCompareNNet(args);
        if (cmd == "parse-phn") return cmdParsePhn(args);
        if (cmd == "lip-info") return cmdLipInfo(args);
        if (cmd == "lip-dump") return cmdLipDump(args);
        if (cmd == "lip-export-table") return cmdLipExportTable(args);
        if (cmd == "lip-import-table") return cmdLipImportTable(args);
        if (cmd == "lip-normalize") return cmdLipNormalize(args);
        if (cmd == "lip-shapes") return cmdLipShapes(args);
        if (cmd == "phn-to-lip") return cmdPhnToLip(args);
        if (cmd == "align-wav-text-lip") return cmdAlignWavTextLip(args);
        if (cmd == "align-wav-ntp-lip") return cmdAlignWavNtpLip(args);
        if (cmd == "wav-info" || cmd == "audio-info") return cmdWavInfo(args);
        if (cmd == "decode-audio-wav") return cmdDecodeAudioWav(args);
        if (cmd == "compose-all-feat") return cmdComposeAllFeat(args);
        if (cmd == "compare-all-feat") return cmdCompareAllFeat(args);
        if (cmd == "compose-netin") return cmdComposeNetIn(args);
        if (cmd == "compare-netin") return cmdCompareNetIn(args);
        if (cmd == "compose-feature-stage") return cmdComposeFeatureStage(args);
        if (cmd == "compose-logenergy") return cmdComposeLogEnergy(args);
        if (cmd == "compare-logenergy-trace") return cmdCompareLogEnergyTrace(args);
        if (cmd == "compose-intensity-energy") return cmdComposeIntensityEnergy(args);
        if (cmd == "compare-intensity-energy-trace") return cmdCompareIntensityEnergyTrace(args);
        if (cmd == "compose-intensity-discrimination") return cmdComposeIntensityDiscrimination(args);
        if (cmd == "compare-intensity-discrimination-trace") return cmdCompareIntensityDiscriminationTrace(args);
        if (cmd == "compose-burst") return cmdComposeBurst(args);
        if (cmd == "compare-burst-trace") return cmdCompareBurstTrace(args);
        if (cmd == "compose-dc") return cmdComposeDc(args);
        if (cmd == "compose-dc-wav") return cmdComposeDcWav(args);
        if (cmd == "compare-dc-trace") return cmdCompareDcTrace(args);
        if (cmd == "compose-plp-frontend" || cmd == "compose-plp-approx") return cmdComposePlpApprox(args);
        if (cmd == "compare-plp-frontend-trace" || cmd == "compare-plp-approx-trace") return cmdComparePlpApproxTrace(args);
        if (cmd == "compare-delta-trace") return cmdCompareDeltaTrace(args);
        if (cmd == "compare-plp-post-trace") return cmdComparePlpPostTrace(args);
        if (cmd == "compose-plp-post-trace") return cmdComposePlpPostTrace(args);
        if (cmd == "compose-full-frontend") return cmdComposeFullFrontend(args);
        if (cmd == "compose-full-frontend-from-wav") return cmdComposeFullFrontendFromWav(args);
        if (cmd == "compare-full-frontend-trace") return cmdCompareFullFrontendTrace(args);
        if (cmd == "compare-sampleconvert-trace") return cmdCompareSampleConvertTrace(args);
        if (cmd == "text-to-ntp") return cmdTextToNtp(args);
        if (cmd == "lookup-word") return cmdLookupWord(args);
        if (cmd == "compare-text-to-ntp-test") return cmdCompareTextToNtpTest(args);
        if (cmd == "align-wav-text") return cmdAlignWavText(args);
        if (cmd == "scan-unknowns") return cmdScanUnknowns(args);
        if (cmd == "scan-tlk-unknowns") return cmdScanTlkUnknowns(args);
        if (cmd == "append-dict-stubs") return cmdAppendDictStubs(args);
        if (cmd == "batch-align") return cmdBatchAlign(args);
        if (cmd == "align-wav-ntp") return cmdAlignWavNtp(args);
        if (cmd == "align-predc-ntp") return cmdAlignPreDcNtp(args);
        if (cmd == "compare-end-to-end-trace") return cmdCompareEndToEndTrace(args);
        if (cmd == "raw-front-end-status") return cmdRawFrontEndStatus(args);
        if (cmd == "compare-garbage-median-trace") return cmdCompareGarbageMedianTrace(args);
        if (cmd == "compose-garbage-median") return cmdComposeGarbageMedian(args);
        if (cmd == "compare-phnfeat-trace") return cmdComparePhnfeatTrace(args);
        if (cmd == "compose-backend-prob-from-trace") return cmdComposeBackendProbFromTrace(args);
        if (cmd == "inspect-backend-graph") return cmdInspectBackendGraph(args);
        if (cmd == "write-phn-from-trace-answer") return cmdWritePhnFromTraceAnswer(args);
        if (cmd == "simple-viterbi-trace") return cmdSimpleViterbiTrace(args);
        if (cmd == "align-viterbi-trace") return cmdAlignViterbiTrace(args);
        if (cmd == "compare-viterbi-trace") return cmdCompareViterbiTrace(args);
        if (cmd == "align-viterbi-backend") return cmdAlignViterbiBackend(args);
        if (cmd == "align-backend-from-ntp-trace") return cmdAlignBackendFromNtpTrace(args);
        if (cmd == "compose-graph-from-ntp") return cmdComposeGraphFromNtp(args);
        if (cmd == "compare-graph-from-ntp-trace") return cmdCompareGraphFromNtpTrace(args);
        if (cmd == "compose-backend-from-ntp-trace") return cmdComposeBackendFromNtpTrace(args);
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
