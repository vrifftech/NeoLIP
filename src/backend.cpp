#include "backend.hpp"

#include "feature_context.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace cslu {
namespace {

constexpr double kSmallProb = 0.0001;
constexpr double kLargeProb = 0.9999;
constexpr double kDependentCap = 0.95;

void requireSameRows(const Matrix& a, const Matrix& b, const std::string& labelA, const std::string& labelB) {
    if (a.rows != b.rows) {
        throw std::runtime_error(labelA + " and " + labelB + " row-count mismatch");
    }
}

void requireRowColumn(const Matrix& m, const std::string& label) {
    if (m.rows == 0 || m.cols == 0) {
        throw std::runtime_error(label + " is empty");
    }
}

std::vector<std::string> splitSpaces(const std::string& s) {
    std::istringstream ss(s);
    std::vector<std::string> out;
    std::string tok;
    while (ss >> tok) {
        out.push_back(tok);
    }
    return out;
}

IndexList parseOneIndexList(const std::string& value) {
    IndexList out;
    std::istringstream ss(value);
    int v = 0;
    while (ss >> v) {
        out.push_back(v);
    }
    return out;
}

static double maxMapped(const Matrix& m, std::size_t row, const IndexList& idxs) {
    double best = 0.0;
    bool have = false;
    for (int idx : idxs) {
        if (idx < 0 || static_cast<std::size_t>(idx) >= m.cols) {
            std::ostringstream err;
            err << "mapping index " << idx << " out of range for matrix with " << m.cols << " columns";
            throw std::runtime_error(err.str());
        }
        const double v = m(row, static_cast<std::size_t>(idx));
        if (!have || v > best) {
            best = v;
            have = true;
        }
    }
    return have ? best : 0.0;
}

static double clampProbability(double v) {
    if (v < kSmallProb) {
        return kSmallProb;
    }
    if (v > kLargeProb) {
        return kLargeProb;
    }
    return v;
}

static double exponentAdjust(double x, std::size_t numCategories, double exponent) {
    x = clampProbability(x);
    const double base = 1.0 / static_cast<double>(numCategories);
    double y = 0.0;
    if (x >= base) {
        y = base + std::pow(x - base, exponent);
    } else {
        y = base - std::pow(base - x, exponent);
    }
    if (y < 0.0) {
        return 0.0;
    }
    if (y > 1.0) {
        return 1.0;
    }
    return y;
}

static void normalizeFeatureRows(Matrix& prob, const IndexLists& stateLists) {
    requireRowColumn(prob, "probability matrix");
    for (std::size_t r = 0; r < prob.rows; ++r) {
        double sum = 0.0;
        for (const auto& idxs : stateLists) {
            sum += maxMapped(prob, r, idxs);
        }
        // phnfeat.dll divides whenever the accumulated state mass is positive.
        // It is not a conditional sum>1 normalization.
        if (sum > 0.0) {
            for (std::size_t c = 0; c < prob.cols; ++c) {
                prob(r, c) /= sum;
            }
        }
    }
}

static int categoryIndexForStateName(const std::vector<std::string>& uniqueStates, const std::string& stateName) {
    auto it = std::find(uniqueStates.begin(), uniqueStates.end(), stateName);
    if (it == uniqueStates.end()) {
        return -1;
    }
    return static_cast<int>(std::distance(uniqueStates.begin(), it));
}

static std::size_t findUnescapedStateDelimiter(const std::string& state, char delimiter) {
    bool escaped = false;
    for (std::size_t i = 0; i < state.size(); ++i) {
        const char ch = state[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == delimiter) return i;
    }
    return std::string::npos;
}

static std::string stateNameToVisiblePhoneLabel(const std::string& state) {
    if (state.size() >= 2 && state.front() == '<' && state.back() == '>') {
        return state.substr(1, state.size() - 2);
    }
    const std::size_t lt = findUnescapedStateDelimiter(state, '<');
    const std::size_t gt = findUnescapedStateDelimiter(state, '>');
    if (lt != std::string::npos) {
        return state.substr(lt + 1);
    }
    if (gt != std::string::npos) {
        return state.substr(0, gt);
    }
    return state;
}

static PhnFile phnFromStateSegments(const std::vector<PhnInterval>& stateSegments) {
    PhnFile out;
    out.millisecondsPerFrame = 1.0;
    std::vector<PhnInterval> raw;
    raw.reserve(stateSegments.size());

    // Match viterbi answer -phonemes behavior approximately: only center states
    // (<phn>) and transition states are later merged by phone label. For this
    // experimental helper we convert state names to visible labels and coalesce
    // consecutive identical labels.
    for (const auto& s : stateSegments) {
        std::string label = s.label;
        if (label == "/BOU" || label == "/EOU") {
            continue;
        }
        label = stateNameToVisiblePhoneLabel(label);
        if (!raw.empty() && raw.back().label == label && raw.back().endMs == s.startMs) {
            raw.back().endMs = s.endMs;
        } else {
            raw.push_back({s.startMs, s.endMs, label});
        }
    }

    // Undo the split labels used by fa_new_16k.tcl.
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (i + 1 < raw.size()) {
            const auto& a = raw[i];
            const auto& b = raw[i + 1];
            std::string merged;
            if (a.label == "aI_a" && b.label == "aI_I") merged = "aI";
            if (a.label == "oI_o" && b.label == "oI_I") merged = ">i";
            if (a.label == "iU_i" && b.label == "iU_U") merged = "iU";
            if (a.label == "aU_a" && b.label == "aU_U") merged = "aU";
            if (a.label == "ei_e" && b.label == "ei_i") merged = "ei";
            if (a.label == "oU_o" && b.label == "oU_U") merged = "oU";
            if (a.label == "tS_t" && b.label == "tS_S") merged = "tS";
            if (a.label == "dZ_d" && b.label == "dZ_Z") merged = "dZ";
            if (!merged.empty()) {
                out.intervals.push_back({a.startMs, b.endMs, merged});
                ++i;
                continue;
            }
        }
        out.intervals.push_back(raw[i]);
    }
    return out;
}

} // namespace

IndexLists parseIndexLists(const std::vector<std::string>& values) {
    IndexLists lists;
    lists.reserve(values.size());
    for (const auto& v : values) {
        lists.push_back(parseOneIndexList(v));
    }
    return lists;
}

std::vector<int> parseIntList(const std::vector<std::string>& values, int emptyValue) {
    std::vector<int> out;
    out.reserve(values.size());
    for (const auto& v : values) {
        auto idxs = parseOneIndexList(v);
        out.push_back(idxs.empty() ? emptyValue : idxs.front());
    }
    return out;
}

Matrix appendGarbageMedianColumn(const Matrix& nnetOrig, int N) {
    requireRowColumn(nnetOrig, "nnetOrig");
    if (N <= 0) {
        throw std::runtime_error("garbage median N must be positive");
    }
    const std::size_t j = nnetOrig.cols;
    const std::size_t effectiveN = std::min<std::size_t>(static_cast<std::size_t>(N), j);
    const std::size_t rank = effectiveN / 2; // CSLU's zero-based selected value after descending partial sort.

    Matrix out;
    out.rows = nnetOrig.rows;
    out.cols = nnetOrig.cols + 1;
    out.data.assign(out.rows * out.cols, 0.0);

    std::vector<double> row(j);
    for (std::size_t r = 0; r < nnetOrig.rows; ++r) {
        for (std::size_t c = 0; c < j; ++c) {
            const double v = nnetOrig(r, c);
            out(r, c) = v;
            row[c] = v;
        }
        std::nth_element(row.begin(), row.begin() + static_cast<std::ptrdiff_t>(rank), row.end(), std::greater<double>());
        out(r, j) = row[rank];
    }
    return out;
}

BackendTraceData readBackendTraceData(const std::filesystem::path& dumpDir) {
    BackendTraceData d;
    d.bcMOrig = readNumericTsv(tracePath(dumpDir, "nnet/bc_m_orig_out"));
    d.bcPOrig = readNumericTsv(tracePath(dumpDir, "nnet/bc_p_orig_out"));
    d.bcHOrig = readNumericTsv(tracePath(dumpDir, "nnet/bc_h_orig_out"));
    d.transMOrig = readNumericTsv(tracePath(dumpDir, "nnet/trans_m_orig_out"));
    d.transPOrig = readNumericTsv(tracePath(dumpDir, "nnet/trans_p_orig_out"));
    d.transHOrig = readNumericTsv(tracePath(dumpDir, "nnet/trans_h_orig_out"));

    d.bcMMedian = readNumericTsv(tracePath(dumpDir, "nnet/bc_m_median_out"));
    d.bcPMedian = readNumericTsv(tracePath(dumpDir, "nnet/bc_p_median_out"));
    d.bcHMedian = readNumericTsv(tracePath(dumpDir, "nnet/bc_h_median_out"));
    d.transMMedian = readNumericTsv(tracePath(dumpDir, "nnet/trans_m_median_out"));
    d.transPMedian = readNumericTsv(tracePath(dumpDir, "nnet/trans_p_median_out"));
    d.transHMedian = readNumericTsv(tracePath(dumpDir, "nnet/trans_h_median_out"));

    d.featStatesM = parseIndexLists(readIndexValueList(dumpDir / "mapping" / "feat_states_idx_m.list.tsv"));
    d.featStatesP = parseIndexLists(readIndexValueList(dumpDir / "mapping" / "feat_states_idx_p.list.tsv"));
    d.featStatesH = parseIndexLists(readIndexValueList(dumpDir / "mapping" / "feat_states_idx_h.list.tsv"));
    d.depList = parseIntList(readIndexValueList(dumpDir / "mapping" / "dep_list.list.tsv"));
    d.vcList = parseIntList(readIndexValueList(dumpDir / "mapping" / "vc_list.list.tsv"));

    d.transStatesM = parseIndexLists(readIndexValueList(dumpDir / "mapping" / "trans_states_m.list.tsv"));
    d.transStatesP = parseIndexLists(readIndexValueList(dumpDir / "mapping" / "trans_states_p.list.tsv"));
    d.transStatesH = parseIndexLists(readIndexValueList(dumpDir / "mapping" / "trans_states_h.list.tsv"));
    return d;
}

TraceGraph readTraceGraph(const std::filesystem::path& dumpDir) {
    TraceGraph g;
    g.statesInitial = readIndexValueList(dumpDir / "graph" / "states_initial.list.tsv");
    g.uniqueStates = readIndexValueList(dumpDir / "graph" / "unique_states.list.tsv");
    g.phnToFrom = readIndexValueList(dumpDir / "graph" / "phn_to_from.list.tsv");
    g.stateToFrom = readIndexValueList(dumpDir / "graph" / "state_to_from.list.tsv");
    g.stateToFromIdx = parseIndexLists(readIndexValueList(dumpDir / "graph" / "state_to_from_idx.list.tsv"));
    return g;
}

Matrix phnfeatCombinePhn(Matrix probM,
                         Matrix probP,
                         Matrix probH,
                         const IndexLists& statesM,
                         const IndexLists& statesP,
                         const IndexLists& statesH,
                         const std::vector<int>& depList,
                         const std::vector<int>* vcList,
                         const Matrix* prevoicing,
                         double mannerExp,
                         double placeExp,
                         double heightExp) {
    requireSameRows(probM, probP, "probM", "probP");
    requireSameRows(probM, probH, "probM", "probH");
    const std::size_t frames = probM.rows;
    const std::size_t states = statesM.size();
    if (statesP.size() != states || statesH.size() != states || depList.size() != states) {
        throw std::runtime_error("phnfeatCombinePhn state-list size mismatch");
    }
    if (vcList && vcList->size() != states) {
        throw std::runtime_error("phnfeatCombinePhn vcList size mismatch");
    }

    normalizeFeatureRows(probM, statesM);
    normalizeFeatureRows(probP, statesP);
    normalizeFeatureRows(probH, statesH);

    Matrix out;
    out.rows = frames;
    out.cols = states;
    out.data.assign(out.rows * out.cols, 0.0);

    for (std::size_t r = 0; r < frames; ++r) {
        for (std::size_t s = 0; s < states; ++s) {
            double m = clampProbability(maxMapped(probM, r, statesM[s]));
            double p = clampProbability(maxMapped(probP, r, statesP[s]));
            double h = clampProbability(maxMapped(probH, r, statesH[s]));
            double y = 0.0;
            if (depList[s] != 0) {
                y = (m + p + h) * (1.0 / 3.0);
                if (vcList && prevoicing && (*vcList)[s] != 0 && r < prevoicing->rows && prevoicing->cols > 0) {
                    y = std::max(y, (*prevoicing)(r, 0));
                }
            } else {
                m = exponentAdjust(m, probM.cols, mannerExp);
                p = exponentAdjust(p, probP.cols, placeExp);
                h = exponentAdjust(h, probH.cols, heightExp);
                y = m * p * h;
            }
            out(r, s) = y;
        }

        double depSum = 0.0;
        double nonDepSum = 0.0;
        for (std::size_t s = 0; s < states; ++s) {
            if (depList[s] != 0) {
                depSum += out(r, s);
            } else {
                nonDepSum += out(r, s);
            }
        }
        if (depSum > kLargeProb) {
            depSum = kDependentCap;
        }
        if (nonDepSum > 0.0) {
            const double scale = (1.0 - depSum) / nonDepSum;
            for (std::size_t s = 0; s < states; ++s) {
                if (depList[s] == 0) {
                    out(r, s) *= scale;
                }
            }
        }
    }
    return out;
}

Matrix phnfeatCombineTrans(const Matrix& probM,
                           const Matrix& probP,
                           const Matrix& probH,
                           const IndexLists& statesM,
                           const IndexLists& statesP,
                           const IndexLists& statesH,
                           double mannerExp,
                           double placeExp,
                           double heightExp) {
    requireSameRows(probM, probP, "probM", "probP");
    requireSameRows(probM, probH, "probM", "probH");
    const std::size_t frames = probM.rows;
    const std::size_t states = statesM.size();
    if (statesP.size() != states || statesH.size() != states) {
        throw std::runtime_error("phnfeatCombineTrans state-list size mismatch");
    }

    Matrix out;
    out.rows = frames;
    out.cols = states;
    out.data.assign(out.rows * out.cols, 0.0);

    for (std::size_t r = 0; r < frames; ++r) {
        double total = 0.0;
        for (std::size_t s = 0; s < states; ++s) {
            const double m = exponentAdjust(maxMapped(probM, r, statesM[s]), probM.cols, mannerExp);
            const double p = exponentAdjust(maxMapped(probP, r, statesP[s]), probP.cols, placeExp);
            const double h = exponentAdjust(maxMapped(probH, r, statesH[s]), probH.cols, heightExp);
            const double y = m * p * h;
            out(r, s) = y;
            total += y;
        }
        if (total > 0.0) {
            for (std::size_t s = 0; s < states; ++s) {
                out(r, s) /= total;
            }
        }
    }
    return out;
}

BackendProbabilities composeBackendProbabilitiesFromMedian(const BackendTraceData& data) {
    BackendProbabilities p;
    p.phnProb = phnfeatCombinePhn(data.bcMMedian, data.bcPMedian, data.bcHMedian,
                                  data.featStatesM, data.featStatesP, data.featStatesH,
                                  data.depList, data.vcList.empty() ? nullptr : &data.vcList,
                                  nullptr, 1.0, 1.4, 1.3);
    p.transProb = phnfeatCombineTrans(data.transMMedian, data.transPMedian, data.transHMedian,
                                      data.transStatesM, data.transStatesP, data.transStatesH,
                                      1.0, 1.0, 1.0);
    return p;
}

BackendProbabilities composeBackendProbabilitiesFromOrig(const BackendTraceData& data, int garbageN) {
    BackendTraceData d = data;
    d.bcMMedian = appendGarbageMedianColumn(d.bcMOrig, garbageN);
    d.bcPMedian = appendGarbageMedianColumn(d.bcPOrig, garbageN);
    d.bcHMedian = appendGarbageMedianColumn(d.bcHOrig, garbageN);
    d.transMMedian = appendGarbageMedianColumn(d.transMOrig, garbageN);
    d.transPMedian = appendGarbageMedianColumn(d.transPOrig, garbageN);
    d.transHMedian = appendGarbageMedianColumn(d.transHOrig, garbageN);
    return composeBackendProbabilitiesFromMedian(d);
}

void writeBackendProbabilities(const BackendProbabilities& p, const std::filesystem::path& outDir) {
    ensureDirectory(outDir / "prob");
    writeNumericTsv(p.phnProb, outDir / "prob" / "phnProb.tsv", 9);
    writeNumericTsv(p.transProb, outDir / "prob" / "transProb.tsv", 9);
}

PhnFile phnFromTraceLabelList(const std::filesystem::path& listPath) {
    const auto values = readIndexValueList(listPath);
    PhnFile phn;
    phn.millisecondsPerFrame = 1.0;
    for (const auto& v : values) {
        auto toks = splitSpaces(v);
        if (toks.size() < 3) {
            continue;
        }
        PhnInterval iv;
        iv.startMs = std::stoi(toks[0]);
        iv.endMs = std::stoi(toks[1]);
        iv.label = toks[2];
        if (iv.label.size() >= 2 && iv.label.front() == '{' && iv.label.back() == '}') {
            iv.label = iv.label.substr(1, iv.label.size() - 2);
        }
        phn.intervals.push_back(iv);
    }
    return phn;
}

ViterbiAlignment csluViterbiAlign(const TraceGraph& graph,
                                  const Matrix& phnProb,
                                  const Matrix& transProb,
                                  const ViterbiOptions& options) {
    const std::size_t frames = phnProb.rows;
    const std::size_t nodes = graph.statesInitial.size();
    if (nodes < 2 || graph.stateToFromIdx.empty()) {
        throw std::runtime_error("csluViterbiAlign graph is empty");
    }
    if (phnProb.rows == 0 || phnProb.cols == 0 || transProb.rows == 0 || transProb.cols == 0) {
        throw std::runtime_error("csluViterbiAlign probability matrices must be non-empty");
    }
    if (transProb.rows != frames) {
        throw std::runtime_error("csluViterbiAlign phn/trans row mismatch");
    }
    if (graph.uniqueStates.size() != phnProb.cols) {
        throw std::runtime_error("csluViterbiAlign phnProb column count does not match unique state count");
    }
    if (graph.phnToFrom.size() != transProb.cols) {
        throw std::runtime_error("csluViterbiAlign transProb column count does not match transition label count");
    }

    auto findNode = [&](const std::string& label) -> std::size_t {
        for (std::size_t i = 0; i < graph.statesInitial.size(); ++i) {
            if (graph.statesInitial[i] == label) {
                return i;
            }
        }
        throw std::runtime_error("csluViterbiAlign missing state " + label);
    };
    const std::size_t bouNode = findNode("/BOU");
    const std::size_t eouNode = findNode("/EOU");

    std::vector<std::vector<std::pair<int, int>>> incoming(nodes);
    for (const auto& row : graph.stateToFromIdx) {
        if (row.size() < 3) {
            continue;
        }
        const int to = row[0];
        const int from = row[1];
        const int trans = row[2];
        if (to >= 0 && from >= 0 && trans >= 0 &&
            static_cast<std::size_t>(to) < nodes &&
            static_cast<std::size_t>(from) < nodes &&
            static_cast<std::size_t>(trans) < transProb.cols) {
            incoming[static_cast<std::size_t>(to)].push_back({from, trans});
        }
    }

    constexpr double neg = -1.0e100;
    std::vector<double> prev(nodes, neg), cur(nodes, neg);
    prev[bouNode] = 0.0; // /BOU
    std::vector<std::vector<int>> back(frames, std::vector<int>(nodes, -1));
    std::vector<std::vector<int>> backTrans(frames, std::vector<int>(nodes, -1));

    for (std::size_t t = 0; t < frames; ++t) {
        std::fill(cur.begin(), cur.end(), neg);
        for (std::size_t to = 0; to < nodes; ++to) {
            const auto& state = graph.statesInitial[to];
            if (state == "/BOU" || state == "/EOU") {
                continue;
            }
            const int cat = categoryIndexForStateName(graph.uniqueStates, state);
            if (cat < 0 || static_cast<std::size_t>(cat) >= phnProb.cols) {
                continue;
            }
            const double obs = std::log(std::max(phnProb(t, static_cast<std::size_t>(cat)),
                                                 options.observationProbFloor));
            double best = neg;
            int bestFrom = -1;
            int bestTrans = -1;
            for (const auto& [from, trans] : incoming[to]) {
                if (prev[static_cast<std::size_t>(from)] <= neg / 2.0) {
                    continue;
                }
                const double tr = options.transProbFactor *
                    std::log(std::max(transProb(t, static_cast<std::size_t>(trans)),
                                      options.transitionProbFloor));
                const double score = prev[static_cast<std::size_t>(from)] + tr + obs;
                if (score > best) {
                    best = score;
                    bestFrom = from;
                    bestTrans = trans;
                }
            }
            cur[to] = best;
            back[t][to] = bestFrom;
            backTrans[t][to] = bestTrans;
        }
        prev.swap(cur);
    }

    int endFrom = -1;
    int endTrans = -1;
    double best = neg;
    for (const auto& [from, trans] : incoming[eouNode]) {
        if (prev[static_cast<std::size_t>(from)] <= neg / 2.0) {
            continue;
        }
        const double score = prev[static_cast<std::size_t>(from)] +
            options.transProbFactor *
            std::log(std::max(transProb(frames - 1, static_cast<std::size_t>(trans)),
                              options.transitionProbFloor));
        if (score > best) {
            best = score;
            endFrom = from;
            endTrans = trans;
        }
    }
    if (endFrom < 0 || endTrans < 0) {
        throw std::runtime_error("csluViterbiAlign did not reach /EOU");
    }

    std::vector<int> path(frames, -1);
    int node = endFrom;
    for (std::size_t rev = 0; rev < frames; ++rev) {
        const std::size_t t = frames - 1 - rev;
        path[t] = node;
        node = back[t][static_cast<std::size_t>(node)];
        if (node < 0 && t > 0) {
            throw std::runtime_error("csluViterbiAlign backtrace failed");
        }
    }

    ViterbiAlignment out;
    out.pathScore = best;
    int curNode = path[0];
    int startFrame = 0;
    for (std::size_t t = 1; t < frames; ++t) {
        if (path[t] != curNode) {
            out.stateSegments.push_back({startFrame * options.frameMs,
                                         static_cast<int>(t) * options.frameMs,
                                         graph.statesInitial[static_cast<std::size_t>(curNode)]});
            startFrame = static_cast<int>(t);
            curNode = path[t];
        }
    }
    out.stateSegments.push_back({startFrame * options.frameMs,
                                 static_cast<int>(frames) * options.frameMs,
                                 graph.statesInitial[static_cast<std::size_t>(curNode)]});
    out.phn = phnFromStateSegments(out.stateSegments);
    return out;
}

PhnFile simpleViterbiTracePhn(const std::filesystem::path& dumpDir,
                              const Matrix& phnProb,
                              const Matrix& transProb) {
    return csluViterbiAlign(readTraceGraph(dumpDir), phnProb, transProb).phn;
}

} // namespace cslu
