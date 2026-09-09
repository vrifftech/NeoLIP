#pragma once

#include "phn.hpp"
#include "tsv.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace cslu {

using IndexList = std::vector<int>;
using IndexLists = std::vector<IndexList>;

struct BackendTraceData {
    Matrix bcMOrig;
    Matrix bcPOrig;
    Matrix bcHOrig;
    Matrix transMOrig;
    Matrix transPOrig;
    Matrix transHOrig;

    Matrix bcMMedian;
    Matrix bcPMedian;
    Matrix bcHMedian;
    Matrix transMMedian;
    Matrix transPMedian;
    Matrix transHMedian;

    IndexLists featStatesM;
    IndexLists featStatesP;
    IndexLists featStatesH;
    std::vector<int> depList;
    std::vector<int> vcList;

    IndexLists transStatesM;
    IndexLists transStatesP;
    IndexLists transStatesH;
};

struct StateDurationBounds {
    int minimumFrames = 0;
    int maximumFrames = 0;
};

struct TraceGraph {
    std::vector<std::string> statesInitial;
    std::vector<std::string> uniqueStates;
    std::vector<std::string> phnToFrom;
    std::vector<std::string> stateToFrom;
    std::vector<IndexList> stateToFromIdx; // rows: toNode fromNode transColumn
    // Parallel to statesInitial. /BOU and /EOU use zero bounds. Generated
    // graphs populate this from phonemes_new_16k.train.spec. Legacy trace
    // imports may leave it empty, in which case duration scoring is disabled.
    std::vector<StateDurationBounds> stateDurations;
    std::vector<int> stateWords; // Optional word identity per node; -1 for boundary/pause.
};

Matrix appendGarbageMedianColumn(const Matrix& nnetOrig, int N = 20);
IndexLists parseIndexLists(const std::vector<std::string>& values);
std::vector<int> parseIntList(const std::vector<std::string>& values, int emptyValue = 0);

BackendTraceData readBackendTraceData(const std::filesystem::path& dumpDir);
TraceGraph readTraceGraph(const std::filesystem::path& dumpDir);

Matrix phnfeatCombinePhn(Matrix probM,
                         Matrix probP,
                         Matrix probH,
                         const IndexLists& statesM,
                         const IndexLists& statesP,
                         const IndexLists& statesH,
                         const std::vector<int>& depList,
                         const std::vector<int>* vcList = nullptr,
                         const Matrix* prevoicing = nullptr,
                         double mannerExp = 1.0,
                         double placeExp = 1.4,
                         double heightExp = 1.3);

Matrix phnfeatCombineTrans(const Matrix& probM,
                           const Matrix& probP,
                           const Matrix& probH,
                           const IndexLists& statesM,
                           const IndexLists& statesP,
                           const IndexLists& statesH,
                           double mannerExp = 1.0,
                           double placeExp = 1.0,
                           double heightExp = 1.0);

struct BackendProbabilities {
    Matrix phnProb;
    Matrix transProb;
};

BackendProbabilities composeBackendProbabilitiesFromMedian(const BackendTraceData& data);
BackendProbabilities composeBackendProbabilitiesFromOrig(const BackendTraceData& data, int garbageN = 20);

PhnFile phnFromTraceLabelList(const std::filesystem::path& listPath);
void writeBackendProbabilities(const BackendProbabilities& p, const std::filesystem::path& outDir);

struct ViterbiOptions {
    double transProbFactor = 4.0;
    double observationProbFloor = 1.0e-5;
    double transitionProbFloor = 1.0e-10;
    int frameMs = 5;

    // CSLU's 16 kHz forced aligner uses duration_model greedy. Leaving a state
    // before dur_p1 costs one short penalty for each missing frame. Remaining
    // in a state beyond dur_p2 costs one long penalty per additional self-loop.
    // Viterbi_init scales both configured values by frameMs / 10 before use.
    std::string durationModel = "greedy";
    double durationShortPenalty = -6.5;
    double durationLongPenalty = -1.0;
};

struct ViterbiAlignment {
    PhnFile phn;
    std::vector<PhnInterval> stateSegments;
    std::vector<int> segmentNodes; // Parallel node IDs, including repeated state labels.
    double pathScore = 0.0;
};

// CSLU-compatible forced-alignment DP for both linear and optional-branch NTP
// graphs. This includes the original greedy state-duration penalties.
// CSLU's printed answer score has extra internal accounting and is not needed
// for .phn/.lip generation.
ViterbiAlignment csluViterbiAlign(const TraceGraph& graph,
                                  const Matrix& phnProb,
                                  const Matrix& transProb,
                                  const ViterbiOptions& options = {});

PhnFile simpleViterbiTracePhn(const std::filesystem::path& dumpDir,
                              const Matrix& phnProb,
                              const Matrix& transProb);

} // namespace cslu
