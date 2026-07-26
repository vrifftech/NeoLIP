#include "graph_build.hpp"

#include "feature_context.hpp"
#include "tsv.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace cslu {
namespace {

static std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

static std::string stripTrailingSemicolon(std::string s) {
    s = trim(s);
    while (!s.empty() && s.back() == ';') {
        s.pop_back();
        s = trim(s);
    }
    return s;
}

static bool startsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

static std::string joinLinesAsSpaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == '\r' || ch == '\n') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

static std::string regexReplaceAll(std::string s, const std::string& pat, const std::string& repl) {
    return std::regex_replace(s, std::regex(pat), repl);
}

static std::string contextClusterName(const std::string& phone) {
    if (phone == ".pau" || phone == "/BOU" || phone == "/EOU" || phone == ".garbage") {
        return "$pau";
    }
    return phone;
}

static std::string unclusterContext(const std::string& context) {
    return context == "$pau" ? std::string(".pau") : context;
}

struct StateContext {
    std::string left;
    std::string phone;
    std::string right;
};

static StateContext parseStateContext(const std::string& state) {
    StateContext out;
    if (state.size() >= 2 && state.front() == '<' && state.back() == '>') {
        out.phone = state.substr(1, state.size() - 2);
        return out;
    }

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
        if (ch == '>') {
            out.phone = state.substr(0, i);
            out.right = state.substr(i + 1);
            return out;
        }
        if (ch == '<') {
            out.left = state.substr(0, i);
            out.phone = state.substr(i + 1);
            return out;
        }
    }
    throw std::runtime_error("cannot parse state context: " + state);
}

static std::pair<std::string, std::string> boundaryContextsForState(const std::string& state,
                                                                     const std::string& phone,
                                                                     const std::string& prevPhone,
                                                                     const std::string& nextPhone,
                                                                     int phoneParts) {
    const StateContext ctx = parseStateContext(state);
    if (ctx.left.empty() && ctx.right.empty()) {
        if (phoneParts == 1) {
            return {prevPhone, nextPhone};
        }
        return {phone, phone};
    }
    if (!ctx.left.empty()) {
        return {unclusterContext(ctx.left), ctx.phone};
    }
    return {ctx.phone, unclusterContext(ctx.right)};
}

static void addUnique(std::vector<std::string>& values, const std::string& value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

static std::string joinList(const std::vector<std::string>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out.push_back(' ');
        out += values[i];
    }
    return out;
}

static std::string joinIndexList(const IndexList& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out.push_back(' ');
        out += std::to_string(values[i]);
    }
    return out;
}

static void writeIndexValueList(const std::vector<std::string>& values, const std::filesystem::path& path) {
    ensureDirectory(path.parent_path());
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("cannot write list TSV: " + path.string());
    }
    out << "index\tvalue\n";
    for (std::size_t i = 0; i < values.size(); ++i) {
        out << i << '\t' << values[i] << '\n';
    }
}

static void writeIndexLists(const IndexLists& lists, const std::filesystem::path& path) {
    std::vector<std::string> values;
    values.reserve(lists.size());
    for (const auto& l : lists) {
        values.push_back(joinIndexList(l));
    }
    writeIndexValueList(values, path);
}

static void writeIntList(const std::vector<int>& values, const std::filesystem::path& path) {
    std::vector<std::string> out;
    out.reserve(values.size());
    for (int v : values) {
        out.push_back(std::to_string(v));
    }
    writeIndexValueList(out, path);
}

static int resolveSpecIndex(const std::vector<SpecEntry>& entries, const std::string& state) {
    for (const auto& e : entries) {
        if (e.state == state) {
            if (e.index >= 0) {
                return e.index;
            }
            for (const auto& tie : entries) {
                if (tie.state == e.tie && tie.index >= 0) {
                    return tie.index;
                }
            }
            throw std::runtime_error("cannot resolve tie state " + e.tie + " for " + state);
        }
    }
    return -1;
}

static const FeatureMapEntry& requireFeature(const std::map<std::string, FeatureMapEntry>& m,
                                             const std::string& phn,
                                             const char* dpf) {
    auto it = m.find(phn);
    if (it == m.end()) {
        throw std::runtime_error(std::string("missing ") + dpf + " feature-map entry for phone " + phn);
    }
    return it->second;
}

static int featurePartsForLabelList(const std::vector<std::string>& labels,
                                    std::map<std::string, int>& featParts) {
    const std::string key = joinList(labels);
    if (auto it = featParts.find(key); it != featParts.end()) {
        return it->second;
    }
    if (labels.empty()) {
        throw std::runtime_error("empty feature label list");
    }
    if (labels.size() == 1) {
        throw std::runtime_error("missing feature-parts entry for " + labels.front());
    }
    int p = -1;
    for (const auto& label : labels) {
        auto it = featParts.find(label);
        if (it == featParts.end()) {
            throw std::runtime_error("missing feature-parts entry for subfeature " + label);
        }
        if (p < 0) {
            p = it->second;
        } else if (p != it->second) {
            throw std::runtime_error("inconsistent subfeature part count for " + key);
        }
    }
    featParts[key] = p;
    return p;
}

static std::vector<std::string> contextListForFeature(const std::map<std::string, FeatureMapEntry>& featSpec,
                                                       int which,
                                                       const std::vector<std::string>& phones) {
    std::vector<std::string> out;
    for (const auto& phn : phones) {
        const auto& e = requireFeature(featSpec, phn, "context");
        const auto& src = (which == 1) ? e.leftContext : e.rightContext;
        for (const auto& item : src) {
            addUnique(out, item);
        }
    }
    return out;
}

static void addStateIndexList(IndexList& sIdxList,
                              const std::string& dir,
                              const std::map<std::string, FeatureMapEntry>& featSpec,
                              const std::vector<std::string>& contextPhones,
                              const std::string& dpfLabel,
                              const std::vector<SpecEntry>& dpfSpec) {
    std::vector<std::string> newStates;
    if (dir == "LEFT") {
        for (const auto& context : contextListForFeature(featSpec, 1, contextPhones)) {
            newStates.push_back(context == "-" ? ("<" + dpfLabel + ">") : (context + "<" + dpfLabel));
        }
    } else if (dir == "RIGHT") {
        for (const auto& context : contextListForFeature(featSpec, 2, contextPhones)) {
            newStates.push_back(context == "-" ? ("<" + dpfLabel + ">") : (dpfLabel + ">" + context));
        }
    } else if (dir == "CENTER") {
        newStates.push_back("<" + dpfLabel + ">");
    } else {
        throw std::runtime_error("internal error: invalid direction " + dir);
    }

    for (const auto& ns : newStates) {
        const int idx = resolveSpecIndex(dpfSpec, ns);
        if (idx >= 0) {
            sIdxList.push_back(idx);
        }
    }
}

static std::string dpfSubdir(char dpf) {
    switch (dpf) {
        case 'm': return "manner";
        case 'p': return "place";
        case 'h': return "height";
        default: throw std::runtime_error("invalid DPF");
    }
}

static bool isVoicedClosure(const std::string& phn) {
    static const std::set<std::string> k = {"bc", "dc", "gc", "dZc", "bcl", "dcl", "gcl", "vc"};
    return k.count(phn) != 0;
}

static std::vector<std::string> splitSpaceList(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) {
        out.push_back(tok);
    }
    return out;
}

static GeneratedMappings buildMappingsForGraph(const TraceGraph& graph, const std::filesystem::path& faRoot) {
    GeneratedMappings out;

    std::map<std::string, std::vector<std::string>> fromList;
    std::map<std::string, std::vector<std::string>> toList;
    for (const auto& s : graph.uniqueStates) {
        fromList[s] = {};
        toList[s] = {};
    }
    for (const auto& item : graph.stateToFrom) {
        const auto toks = splitSpaceList(item);
        if (toks.size() < 3) {
            continue;
        }
        const std::string& state = toks[0];
        const std::string& context = toks[1];
        const std::string& dir = toks[2];
        if (fromList.find(state) == fromList.end()) {
            continue;
        }
        auto& list = (dir == "-1") ? fromList[state] : toList[state];
        addUnique(list, context);
    }

    auto phonemeParts = readPartsFile(faRoot / "phonemes_new_16k.parts");
    phonemeParts[".garbage"] = 1;

    std::map<char, std::vector<std::string>*> phnLabels = {
        {'m', &out.featPhnM}, {'p', &out.featPhnP}, {'h', &out.featPhnH}
    };
    std::map<char, IndexLists*> idxLists = {
        {'m', &out.featStatesM}, {'p', &out.featStatesP}, {'h', &out.featStatesH}
    };

    for (char dpf : {'m', 'p', 'h'}) {
        const std::string sub = dpfSubdir(dpf);
        const auto dpfSpec = readSpecEntries(faRoot / "bc_16k" / (sub + ".train.spec"));
        const auto featSpec = readFeatureMap(faRoot / "bc_16k" / (sub + ".map"));
        auto featParts = readPartsFile(faRoot / "bc_16k" / (sub + ".parts"));
        featParts[".garbage"] = 1;

        IndexLists& rows = *idxLists[dpf];
        std::vector<std::string>& featureNames = *phnLabels[dpf];
        rows.clear();
        featureNames.clear();
        rows.reserve(graph.uniqueStates.size());
        featureNames.reserve(graph.uniqueStates.size());

        for (const auto& state : graph.uniqueStates) {
            const StateContext ctx = parseStateContext(state);
            const std::string& phn = ctx.phone;
            const auto& f = requireFeature(featSpec, phn, sub.c_str());
            const int phnParts = phonemeParts.at(phn);
            const int featPartCount = featurePartsForLabelList(f.feature, featParts);
            const std::string featureName = joinList(f.feature);

            IndexList sIdxList;
            for (const auto& dpfLabel : f.feature) {
                if (!ctx.left.empty()) {
                    addStateIndexList(sIdxList, "LEFT", featSpec, fromList[state], dpfLabel, dpfSpec);
                    if (phnParts == 2 && featPartCount == 3) {
                        addStateIndexList(sIdxList, "CENTER", featSpec, {}, dpfLabel, dpfSpec);
                    }
                } else if (!ctx.right.empty()) {
                    addStateIndexList(sIdxList, "RIGHT", featSpec, toList[state], dpfLabel, dpfSpec);
                    if (phnParts == 2 && featPartCount == 3) {
                        addStateIndexList(sIdxList, "CENTER", featSpec, {}, dpfLabel, dpfSpec);
                    }
                } else {
                    if (phnParts == 3) {
                        if (featPartCount == 1 || featPartCount == 3) {
                            addStateIndexList(sIdxList, "CENTER", featSpec, {}, dpfLabel, dpfSpec);
                        } else {
                            addStateIndexList(sIdxList, "LEFT", featSpec, fromList[state], dpfLabel, dpfSpec);
                            addStateIndexList(sIdxList, "RIGHT", featSpec, toList[state], dpfLabel, dpfSpec);
                        }
                    }
                    if (phnParts == 1) {
                        addStateIndexList(sIdxList, "CENTER", featSpec, {}, dpfLabel, dpfSpec);
                    }
                }
            }
            rows.push_back(std::move(sIdxList));
            featureNames.push_back(featureName);
        }
    }

    out.depList.clear();
    out.vcList.clear();
    out.depList.reserve(graph.uniqueStates.size());
    out.vcList.reserve(graph.uniqueStates.size());
    for (std::size_t i = 0; i < graph.uniqueStates.size(); ++i) {
        out.depList.push_back((out.featPhnM[i] == out.featPhnP[i] && out.featPhnM[i] == out.featPhnH[i]) ? 1 : 0);
        const auto ctx = parseStateContext(graph.uniqueStates[i]);
        out.vcList.push_back(isVoicedClosure(ctx.phone) ? 1 : 0);
    }

    std::map<char, IndexLists*> transRows = {
        {'m', &out.transStatesM}, {'p', &out.transStatesP}, {'h', &out.transStatesH}
    };

    for (char dpf : {'m', 'p', 'h'}) {
        const std::string sub = dpfSubdir(dpf);
        const auto transSpec = readSpecEntries(faRoot / "trans_16k" / (sub + ".train.spec"));
        const auto featSpec = readFeatureMap(faRoot / "bc_16k" / (sub + ".map"));
        int ntrIdx = -1;
        for (std::size_t i = 0; i < transSpec.size(); ++i) {
            if (transSpec[i].state == "<NTR>" || transSpec[i].state == "<ntr>") {
                ntrIdx = static_cast<int>(i);
                break;
            }
        }
        if (ntrIdx < 0) {
            throw std::runtime_error("cannot find <NTR>/<ntr> in transition spec for " + sub);
        }

        IndexLists& rows = *transRows[dpf];
        rows.clear();
        rows.reserve(graph.phnToFrom.size());
        for (const auto& item : graph.phnToFrom) {
            const auto toks = splitSpaceList(item);
            if (toks.size() < 2) {
                throw std::runtime_error("invalid phnToFrom row: " + item);
            }
            const std::string& to = toks[0];
            const std::string& from = toks[1];
            if (from == "<<N/A>>" || to == "<<N/A>>" || to == "<NTR>") {
                rows.push_back({ntrIdx});
                continue;
            }
            const auto& fromDpf = requireFeature(featSpec, from, sub.c_str()).feature;
            const auto& toDpf = requireFeature(featSpec, to, sub.c_str()).feature;
            IndexList idxs;
            for (const auto& e : transSpec) {
                int index = e.index;
                if (index < 0) {
                    index = resolveSpecIndex(transSpec, e.state);
                }
                if (e.state == "<NTR>" || e.state == "<ntr>") {
                    continue;
                }
                if ((std::find(fromDpf.begin(), fromDpf.end(), ".garbage") != fromDpf.end() ||
                     std::find(toDpf.begin(), toDpf.end(), ".garbage") != toDpf.end()) &&
                    e.state == "<.garbage>") {
                    idxs.push_back(index);
                    continue;
                }
                const auto pos = e.state.find('>');
                if (pos == std::string::npos) {
                    continue;
                }
                const std::string fromState = e.state.substr(0, pos);
                const std::string toState = e.state.substr(pos + 1);
                if (std::find(fromDpf.begin(), fromDpf.end(), fromState) != fromDpf.end() &&
                    std::find(toDpf.begin(), toDpf.end(), toState) != toDpf.end()) {
                    idxs.push_back(index);
                }
            }
            if (idxs.empty()) {
                throw std::runtime_error("cannot find transition mapping for " + sub + ": " + item);
            }
            rows.push_back(std::move(idxs));
        }
    }

    return out;
}


struct NtpExpr {
    enum class Kind { Phone, Seq, Alt } kind = Kind::Seq;
    std::string phone;
    std::vector<NtpExpr> seq;
    std::vector<std::vector<NtpExpr>> alts;
};

static std::vector<std::string> tokenizeNtpExpression(const std::string& text) {
    std::vector<std::string> toks;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            toks.push_back(cur);
            cur.clear();
        }
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (std::isspace(static_cast<unsigned char>(ch))) {
            flush();
        } else if (ch == '\\' && i + 1 < text.size()) {
            cur.push_back(ch);
            cur.push_back(text[i + 1]);
            ++i;
        } else if (ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '|') {
            flush();
            toks.emplace_back(1, ch);
        } else {
            cur.push_back(ch);
        }
    }
    flush();
    return toks;
}


static std::string joinTokensWithSpaces(const std::vector<std::string>& toks) {
    std::string out;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        if (i) out.push_back(' ');
        out += toks[i];
    }
    return out;
}

static std::string canonicalizeNtpTopLevelAlternatives(const std::string& ntp) {
    const auto toks = tokenizeNtpExpression(ntp);
    struct Element {
        std::vector<std::string> toks;
        bool barrier = false;
        bool bareBar = false;
    };
    std::vector<Element> elems;
    for (std::size_t i = 0; i < toks.size();) {
        Element e;
        if (toks[i] == "(" || toks[i] == "[") {
            const std::string open = toks[i];
            const std::string close = (open == "(") ? ")" : "]";
            int depth = 0;
            do {
                if (toks[i] == "(" || toks[i] == "[") ++depth;
                else if (toks[i] == ")" || toks[i] == "]") --depth;
                e.toks.push_back(toks[i]);
                ++i;
                if (i >= toks.size() && depth != 0) break;
            } while (i < toks.size() && depth > 0);
            e.barrier = (e.toks.size() == 3 && e.toks[0] == "[" && e.toks[1] == ".pau" && e.toks[2] == "]");
        } else {
            e.toks.push_back(toks[i]);
            e.bareBar = (toks[i] == "|");
            e.barrier = (toks[i] == ".pau" || toks[i] == ".garbage");
            ++i;
        }
        elems.push_back(std::move(e));
    }

    std::vector<std::string> out;
    std::vector<std::string> segment;
    bool segmentHasBar = false;
    auto flushSegment = [&]() {
        if (segment.empty()) return;
        if (segmentHasBar) out.push_back("(");
        out.insert(out.end(), segment.begin(), segment.end());
        if (segmentHasBar) out.push_back(")");
        segment.clear();
        segmentHasBar = false;
    };

    for (const auto& e : elems) {
        if (e.barrier) {
            flushSegment();
            out.insert(out.end(), e.toks.begin(), e.toks.end());
        } else {
            if (e.bareBar) segmentHasBar = true;
            segment.insert(segment.end(), e.toks.begin(), e.toks.end());
        }
    }
    flushSegment();
    return joinTokensWithSpaces(out);
}

class NtpParser {
public:
    explicit NtpParser(std::vector<std::string> toks) : toks_(std::move(toks)) {}

    NtpExpr parseRoot() {
        auto seq = parseSequence("");
        if (pos_ != toks_.size()) {
            throw std::runtime_error("unexpected token in NTP expression: " + toks_[pos_]);
        }
        return makeSeq(std::move(seq));
    }

private:
    std::vector<std::string> toks_;
    std::size_t pos_ = 0;

    static NtpExpr makePhone(std::string phone) {
        NtpExpr e;
        e.kind = NtpExpr::Kind::Phone;
        e.phone = std::move(phone);
        return e;
    }

    static NtpExpr makeSeq(std::vector<NtpExpr> seq) {
        NtpExpr e;
        e.kind = NtpExpr::Kind::Seq;
        e.seq = std::move(seq);
        return e;
    }

    static NtpExpr makeAlt(std::vector<std::vector<NtpExpr>> alts) {
        NtpExpr e;
        e.kind = NtpExpr::Kind::Alt;
        e.alts = std::move(alts);
        return e;
    }

    bool atStop(const std::string& closeTok) const {
        if (pos_ >= toks_.size()) return true;
        if (!closeTok.empty() && toks_[pos_] == closeTok) return true;
        return toks_[pos_] == "|";
    }

    std::vector<NtpExpr> parseSequence(const std::string& closeTok) {
        std::vector<NtpExpr> seq;
        while (pos_ < toks_.size() && !atStop(closeTok)) {
            const std::string tok = toks_[pos_++];
            if (tok == "(") {
                seq.push_back(parseDelimited(")", false));
            } else if (tok == "[") {
                seq.push_back(parseDelimited("]", true));
            } else if (tok == ")" || tok == "]") {
                throw std::runtime_error("unmatched NTP delimiter: " + tok);
            } else {
                seq.push_back(makePhone(tok));
            }
        }
        return seq;
    }

    NtpExpr parseDelimited(const std::string& closeTok, bool optional) {
        std::vector<std::vector<NtpExpr>> alts;
        while (true) {
            alts.push_back(parseSequence(closeTok));
            if (pos_ >= toks_.size()) {
                throw std::runtime_error("missing NTP delimiter " + closeTok);
            }
            if (toks_[pos_] == "|") {
                ++pos_;
                continue;
            }
            if (toks_[pos_] == closeTok) {
                ++pos_;
                break;
            }
            throw std::runtime_error("unexpected token in NTP group: " + toks_[pos_]);
        }
        if (optional) {
            // CSLU square brackets mean the bracketed material may be skipped.
            alts.insert(alts.begin(), std::vector<NtpExpr>{});
        }
        if (!optional && alts.size() == 1) {
            return makeSeq(std::move(alts.front()));
        }
        return makeAlt(std::move(alts));
    }
};

struct NfaPhoneEdge {
    int from = -1;
    int to = -1;
    std::string phone;
};

struct NfaGraph {
    int start = 0;
    int end = 1;
    std::vector<std::vector<int>> eps;
    std::vector<NfaPhoneEdge> phones;

    NfaGraph() : eps(2) {}

    int newNode() {
        eps.push_back({});
        return static_cast<int>(eps.size() - 1);
    }

    void addEps(int from, int to) {
        eps.at(static_cast<std::size_t>(from)).push_back(to);
    }

    void addPhone(int from, int to, const std::string& phone) {
        phones.push_back({from, to, phone});
    }
};

static void compileNtpSeqToNfa(const std::vector<NtpExpr>& seq, NfaGraph& nfa, int start, int end);

static void compileNtpExprToNfa(const NtpExpr& expr, NfaGraph& nfa, int start, int end) {
    if (expr.kind == NtpExpr::Kind::Phone) {
        nfa.addPhone(start, end, expr.phone);
        return;
    }
    if (expr.kind == NtpExpr::Kind::Seq) {
        compileNtpSeqToNfa(expr.seq, nfa, start, end);
        return;
    }
    if (expr.kind == NtpExpr::Kind::Alt) {
        for (const auto& alt : expr.alts) {
            const int aStart = nfa.newNode();
            const int aEnd = nfa.newNode();
            nfa.addEps(start, aStart);
            compileNtpSeqToNfa(alt, nfa, aStart, aEnd);
            nfa.addEps(aEnd, end);
        }
        return;
    }
}

static void compileNtpSeqToNfa(const std::vector<NtpExpr>& seq, NfaGraph& nfa, int start, int end) {
    if (seq.empty()) {
        nfa.addEps(start, end);
        return;
    }
    int cur = start;
    for (std::size_t i = 0; i < seq.size(); ++i) {
        const int next = (i + 1 == seq.size()) ? end : nfa.newNode();
        compileNtpExprToNfa(seq[i], nfa, cur, next);
        cur = next;
    }
}

static std::vector<std::vector<int>> reverseEps(const NfaGraph& nfa) {
    std::vector<std::vector<int>> rev(nfa.eps.size());
    for (std::size_t i = 0; i < nfa.eps.size(); ++i) {
        for (int to : nfa.eps[i]) {
            rev.at(static_cast<std::size_t>(to)).push_back(static_cast<int>(i));
        }
    }
    return rev;
}

struct NfaImmediateContexts {
    std::set<int> prevEdges;
    std::set<int> nextEdges;
    bool hasStart = false;
    bool hasEnd = false;
};

static NfaImmediateContexts computeImmediateContexts(const NfaGraph& nfa, int edgeIndex) {
    const auto rev = reverseEps(nfa);
    NfaImmediateContexts ctx;
    const auto& e = nfa.phones.at(static_cast<std::size_t>(edgeIndex));

    {
        std::queue<int> q;
        std::set<int> visited;
        q.push(e.from);
        visited.insert(e.from);
        while (!q.empty()) {
            const int node = q.front();
            q.pop();
            if (node == nfa.start) ctx.hasStart = true;
            for (std::size_t i = 0; i < nfa.phones.size(); ++i) {
                if (static_cast<int>(i) == edgeIndex) continue;
                if (nfa.phones[i].to == node) ctx.prevEdges.insert(static_cast<int>(i));
            }
            for (int from : rev.at(static_cast<std::size_t>(node))) {
                if (visited.insert(from).second) q.push(from);
            }
        }
    }
    {
        std::queue<int> q;
        std::set<int> visited;
        q.push(e.to);
        visited.insert(e.to);
        while (!q.empty()) {
            const int node = q.front();
            q.pop();
            if (node == nfa.end) ctx.hasEnd = true;
            for (std::size_t i = 0; i < nfa.phones.size(); ++i) {
                if (static_cast<int>(i) == edgeIndex) continue;
                if (nfa.phones[i].from == node) ctx.nextEdges.insert(static_cast<int>(i));
            }
            for (int to : nfa.eps.at(static_cast<std::size_t>(node))) {
                if (visited.insert(to).second) q.push(to);
            }
        }
    }
    return ctx;
}

static std::string stateContextBoundaryLabel(const std::string& raw) {
    if (raw == "<<N/A>>" || raw == "/BOU" || raw == "/EOU") return ".pau";
    return raw;
}

static std::string stateContextClusterLabel(const std::string& raw) {
    if (raw == "<<N/A>>") return "$pau";
    return contextClusterName(raw);
}

struct PhoneEdgeFragment {
    std::vector<int> allStateNodes;
    std::map<std::string, std::vector<int>> entriesByPrevPhone;
    std::map<std::string, std::vector<int>> exitsByNextPhone;
};

static TraceGraph buildExpandedNtpGraph(const std::string& ntpAfterSubstitutions,
                                        const std::map<std::string, int>& phonemeParts,
                                        std::vector<std::string>* firstPathOut = nullptr) {
    const auto toks = tokenizeNtpExpression(ntpAfterSubstitutions);
    NtpParser parser(toks);
    const NtpExpr root = parser.parseRoot();

    NfaGraph nfa;
    compileNtpExprToNfa(root, nfa, nfa.start, nfa.end);
    if (nfa.phones.empty()) {
        throw std::runtime_error("NTP produced no phones");
    }

    TraceGraph graph;
    graph.statesInitial.push_back("/BOU");
    graph.statesInitial.push_back("/EOU");

    auto addStateNode = [&](const std::string& state) -> int {
        const int idx = static_cast<int>(graph.statesInitial.size());
        graph.statesInitial.push_back(state);
        addUnique(graph.uniqueStates, state);
        return idx;
    };

    auto addTransitionArc = [&](int to, int from, const std::string& label) {
        const int idx = static_cast<int>(graph.phnToFrom.size());
        graph.phnToFrom.push_back(label);
        graph.stateToFromIdx.push_back({to, from, idx});
    };

    auto addStateContext = [&](const std::string& state, const std::string& context, int dir) {
        graph.stateToFrom.push_back(state + " " + stateContextBoundaryLabel(context) + " " + std::to_string(dir));
    };

    std::vector<NfaImmediateContexts> contexts;
    contexts.reserve(nfa.phones.size());
    for (std::size_t i = 0; i < nfa.phones.size(); ++i) {
        contexts.push_back(computeImmediateContexts(nfa, static_cast<int>(i)));
    }

    std::vector<PhoneEdgeFragment> fragments(nfa.phones.size());
    for (std::size_t ei = 0; ei < nfa.phones.size(); ++ei) {
        const auto& e = nfa.phones[ei];
        auto it = phonemeParts.find(e.phone);
        if (it == phonemeParts.end()) {
            throw std::runtime_error("phone '" + e.phone + "' is missing from phonemes_new_16k.parts");
        }
        const int parts = it->second;
        std::set<std::string> prevLabels;
        std::set<std::string> nextLabels;
        if (contexts[ei].hasStart) prevLabels.insert("<<N/A>>");
        for (int pe : contexts[ei].prevEdges) prevLabels.insert(nfa.phones[static_cast<std::size_t>(pe)].phone);
        if (contexts[ei].hasEnd) nextLabels.insert("<<N/A>>");
        for (int ne : contexts[ei].nextEdges) nextLabels.insert(nfa.phones[static_cast<std::size_t>(ne)].phone);
        if (prevLabels.empty()) prevLabels.insert("<<N/A>>");
        if (nextLabels.empty()) nextLabels.insert("<<N/A>>");

        PhoneEdgeFragment& f = fragments[ei];
        if (parts == 1) {
            const std::string state = "<" + e.phone + ">";
            const int n = addStateNode(state);
            f.allStateNodes.push_back(n);
            for (const auto& p : prevLabels) {
                f.entriesByPrevPhone[p].push_back(n);
                addStateContext(state, p, -1);
            }
            for (const auto& nlabel : nextLabels) {
                f.exitsByNextPhone[nlabel].push_back(n);
                addStateContext(state, nlabel, 1);
            }
        } else if (parts == 2) {
            std::vector<int> leftNodes;
            std::vector<int> rightNodes;
            for (const auto& p : prevLabels) {
                const std::string state = stateContextClusterLabel(p) + "<" + e.phone;
                const int n = addStateNode(state);
                f.allStateNodes.push_back(n);
                f.entriesByPrevPhone[p].push_back(n);
                leftNodes.push_back(n);
                addStateContext(state, p, -1);
                addStateContext(state, e.phone, 1);
            }
            for (const auto& nlabel : nextLabels) {
                const std::string state = e.phone + ">" + stateContextClusterLabel(nlabel);
                const int n = addStateNode(state);
                f.allStateNodes.push_back(n);
                f.exitsByNextPhone[nlabel].push_back(n);
                rightNodes.push_back(n);
                addStateContext(state, e.phone, -1);
                addStateContext(state, nlabel, 1);
            }
            for (int l : leftNodes) {
                for (int r : rightNodes) addTransitionArc(r, l, "<NTR> " + e.phone);
            }
        } else if (parts == 3) {
            std::vector<int> leftNodes;
            std::vector<int> rightNodes;
            for (const auto& p : prevLabels) {
                const std::string state = stateContextClusterLabel(p) + "<" + e.phone;
                const int n = addStateNode(state);
                f.allStateNodes.push_back(n);
                f.entriesByPrevPhone[p].push_back(n);
                leftNodes.push_back(n);
                addStateContext(state, p, -1);
                addStateContext(state, e.phone, 1);
            }
            const std::string centerState = "<" + e.phone + ">";
            const int center = addStateNode(centerState);
            f.allStateNodes.push_back(center);
            addStateContext(centerState, e.phone, -1);
            addStateContext(centerState, e.phone, 1);
            for (const auto& nlabel : nextLabels) {
                const std::string state = e.phone + ">" + stateContextClusterLabel(nlabel);
                const int n = addStateNode(state);
                f.allStateNodes.push_back(n);
                f.exitsByNextPhone[nlabel].push_back(n);
                rightNodes.push_back(n);
                addStateContext(state, e.phone, -1);
                addStateContext(state, nlabel, 1);
            }
            for (int l : leftNodes) addTransitionArc(center, l, "<NTR> " + e.phone);
            for (int r : rightNodes) addTransitionArc(r, center, "<NTR> " + e.phone);
        } else {
            throw std::runtime_error("unsupported phoneme part count for " + e.phone + ": " + std::to_string(parts));
        }

        for (int n : f.allStateNodes) {
            addTransitionArc(n, n, "<NTR> " + e.phone);
        }
    }

    // Boundary and inter-phone arcs.
    std::set<std::string> firstPhones;
    std::set<std::string> lastPhones;
    for (std::size_t ei = 0; ei < nfa.phones.size(); ++ei) {
        const auto& e = nfa.phones[ei];
        const auto& f = fragments[ei];
        if (contexts[ei].hasStart) {
            firstPhones.insert(e.phone);
            const auto entryIt = f.entriesByPrevPhone.find("<<N/A>>");
            if (entryIt == f.entriesByPrevPhone.end()) {
                throw std::runtime_error("internal NTP graph missing BOU entry for " + e.phone);
            }
            for (int n : entryIt->second) {
                addTransitionArc(n, 0, e.phone + " <<N/A>>");
            }
        }
        if (contexts[ei].hasEnd) {
            lastPhones.insert(e.phone);
            const auto exitIt = f.exitsByNextPhone.find("<<N/A>>");
            if (exitIt == f.exitsByNextPhone.end()) {
                throw std::runtime_error("internal NTP graph missing EOU exit for " + e.phone);
            }
            for (int n : exitIt->second) {
                addTransitionArc(1, n, "<<N/A>> " + e.phone);
            }
        }
        for (int nextEdgeIndex : contexts[ei].nextEdges) {
            const auto& ne = nfa.phones[static_cast<std::size_t>(nextEdgeIndex)];
            const auto& nf = fragments[static_cast<std::size_t>(nextEdgeIndex)];
            auto exitIt = f.exitsByNextPhone.find(ne.phone);
            auto entryIt = nf.entriesByPrevPhone.find(e.phone);
            if (exitIt == f.exitsByNextPhone.end() || entryIt == nf.entriesByPrevPhone.end()) {
                throw std::runtime_error("internal NTP graph context mismatch between " + e.phone + " and " + ne.phone);
            }
            for (int from : exitIt->second) {
                for (int to : entryIt->second) {
                    addTransitionArc(to, from, ne.phone + " " + e.phone);
                }
            }
        }
    }

    for (const auto& p : firstPhones) graph.stateToFrom.push_back("/BOU " + p + " 1");
    for (const auto& p : lastPhones) graph.stateToFrom.push_back("/EOU " + p + " -1");

    if (firstPathOut) {
        // Keep a deterministic diagnostic path: first alternative, include optional material.
        *firstPathOut = parseSimpleNtpPhoneSequence(ntpAfterSubstitutions);
    }

    return graph;
}

[[maybe_unused]] static TraceGraph buildLinearGraph(const std::vector<std::string>& phones,
                                   const std::map<std::string, int>& phonemeParts) {
    if (phones.empty()) {
        throw std::runtime_error("NTP produced no phones");
    }

    struct NodeInfo {
        std::string state;
        std::string phone;
        std::string prev;
        std::string next;
    };

    std::vector<NodeInfo> nodes;
    nodes.reserve(phones.size() * 3);
    for (std::size_t i = 0; i < phones.size(); ++i) {
        const std::string& phn = phones[i];
        auto it = phonemeParts.find(phn);
        if (it == phonemeParts.end()) {
            throw std::runtime_error("phone '" + phn + "' is missing from phonemes_new_16k.parts");
        }
        const int parts = it->second;
        const std::string prev = (i == 0) ? "<<N/A>>" : phones[i - 1];
        const std::string next = (i + 1 == phones.size()) ? "<<N/A>>" : phones[i + 1];
        std::vector<std::string> stateNames;
        if (parts == 1) {
            stateNames.push_back("<" + phn + ">");
        } else if (parts == 2) {
            stateNames.push_back(contextClusterName(prev) + "<" + phn);
            stateNames.push_back(phn + ">" + contextClusterName(next));
        } else if (parts == 3) {
            stateNames.push_back(contextClusterName(prev) + "<" + phn);
            stateNames.push_back("<" + phn + ">");
            stateNames.push_back(phn + ">" + contextClusterName(next));
        } else {
            throw std::runtime_error("unsupported phoneme part count for " + phn + ": " + std::to_string(parts));
        }
        for (const auto& s : stateNames) {
            nodes.push_back({s, phn, prev, next});
        }
    }

    TraceGraph graph;
    graph.statesInitial.push_back("/BOU");
    graph.statesInitial.push_back("/EOU");
    for (const auto& n : nodes) {
        graph.statesInitial.push_back(n.state);
        addUnique(graph.uniqueStates, n.state);
    }

    graph.stateToFrom.push_back("/BOU " + phones.front() + " 1");
    graph.stateToFrom.push_back("/EOU " + phones.back() + " -1");
    for (const auto& n : nodes) {
        const int parts = phonemeParts.at(n.phone);
        const auto lr = boundaryContextsForState(n.state, n.phone, n.prev, n.next, parts);
        graph.stateToFrom.push_back(n.state + " " + lr.first + " -1");
        graph.stateToFrom.push_back(n.state + " " + lr.second + " 1");
    }

    auto addArc = [&](int to, int from, const std::string& label) {
        const int idx = static_cast<int>(graph.phnToFrom.size());
        graph.phnToFrom.push_back(label);
        graph.stateToFromIdx.push_back({to, from, idx});
    };

    addArc(1, static_cast<int>(graph.statesInitial.size() - 1), "<<N/A>> " + phones.back());
    addArc(2, 0, phones.front() + " <<N/A>>");

    for (std::size_t nodeIdx = 0; nodeIdx < nodes.size(); ++nodeIdx) {
        const int absolute = static_cast<int>(nodeIdx + 2);
        const auto& n = nodes[nodeIdx];
        addArc(absolute, absolute, "<NTR> " + n.phone);
        if (nodeIdx + 1 < nodes.size()) {
            const auto& next = nodes[nodeIdx + 1];
            const std::string label = (next.phone == n.phone) ? ("<NTR> " + n.phone) : (next.phone + " " + n.phone);
            addArc(absolute + 1, absolute, label);
        }
    }

    return graph;
}

static std::map<std::string, std::pair<int, int>> durationMapFromSpec(const std::filesystem::path& specPath) {
    std::map<std::string, std::pair<int, int>> out;
    for (const auto& e : readSpecEntries(specPath)) {
        out[e.state] = {e.dur1, e.dur2};
    }
    return out;
}

static void writeTempSpec(const GeneratedGraphBundle& bundle,
                          const std::filesystem::path& faRoot,
                          const std::filesystem::path& path) {
    const auto durations = durationMapFromSpec(faRoot / "phonemes_new_16k.train.spec");
    ensureDirectory(path.parent_path());
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("cannot write temp spec: " + path.string());
    }
    out << "// generated by NeoLIP\n";
    out << "duration_model\tgreedy ;\n";
    out << "sampling_freq\t16000 ;\n";
    out << "frame_size\t\t5 ;\n\n";
    out << "$pau     := .pau /BOU /EOU .garbage ;\n\n";
    out << "// name\t\t\tindex\tdur_p1\tdur_p2\t\ttie\n";
    for (std::size_t i = 0; i < bundle.graph.uniqueStates.size(); ++i) {
        auto it = durations.find(bundle.graph.uniqueStates[i]);
        if (it == durations.end()) {
            throw std::runtime_error("duration not found in complete spec for state " + bundle.graph.uniqueStates[i]);
        }
        out << bundle.graph.uniqueStates[i] << "\t" << i << "\t" << it->second.first << "\t" << it->second.second << "\t- ;\n";
    }
}

} // namespace

std::vector<std::string> tokenizeTclListLike(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    int braceDepth = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '\\' && i + 1 < line.size()) {
            cur.push_back(ch);
            cur.push_back(line[i + 1]);
            ++i;
            continue;
        }
        if (braceDepth > 0) {
            if (ch == '{') {
                ++braceDepth;
                cur.push_back(ch);
            } else if (ch == '}') {
                --braceDepth;
                if (braceDepth == 0) {
                    out.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(ch);
                }
            } else {
                cur.push_back(ch);
            }
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == ';') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else if (ch == '{') {
            if (!cur.empty()) {
                cur.push_back(ch);
            }
            braceDepth = 1;
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}


std::string applyFaNtpSubstitutions(const std::string& ntpText) {
    std::string ntp = joinLinesAsSpaces(ntpText);
    ntp = regexReplaceAll(ntp, "d_\\(", "d_\\(");
    ntp = regexReplaceAll(ntp, "n_\\(", "n_\\(");
    ntp = regexReplaceAll(ntp, ">i", "(oI_o oI_I)");
    ntp = regexReplaceAll(ntp, "(^|[^A-Za-z0-9_])oI([^A-Za-z0-9_]|$)", "$1(oI_o oI_I)$2");
    ntp = regexReplaceAll(ntp, "([^\\*\\+0-9\\-])>", "$1\\>");
    ntp = regexReplaceAll(ntp, "aI", "(aI_a aI_I)");
    ntp = regexReplaceAll(ntp, "iU", "(iU_i iU_U)");
    ntp = regexReplaceAll(ntp, "aU", "(aU_a aU_U)");
    ntp = regexReplaceAll(ntp, "ei", "(ei_e ei_i)");
    ntp = regexReplaceAll(ntp, "oU", "(oU_o oU_U)");
    ntp = regexReplaceAll(ntp, "tS([^c])", "(tS_t tS_S)$1");
    ntp = regexReplaceAll(ntp, "dZ([^c])", "(dZ_d dZ_Z)$1");
    return ntp;
}

std::vector<std::string> parseSimpleNtpPhoneSequence(const std::string& ntpAfterSubstitutions) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < ntpAfterSubstitutions.size(); ++i) {
        const char ch = ntpAfterSubstitutions[i];
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        if (ch == '\\' && i + 1 < ntpAfterSubstitutions.size()) {
            cur.push_back(ch);
            cur.push_back(ntpAfterSubstitutions[i + 1]);
            ++i;
            continue;
        }
        if (ch == '(' || ch == ')' || ch == '[' || ch == ']') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        if (ch == '|') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            // This helper intentionally flattens by ignoring alternatives.
            // It is kept only for deterministic diagnostic/simple-NTP paths;
            // the active graph builder below expands alternatives and optionals.
            continue;
        }
        cur.push_back(ch);
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}


static void collectFirstPathPhones(const NtpExpr& expr, std::vector<std::string>& out, bool includeOptional) {
    if (expr.kind == NtpExpr::Kind::Phone) {
        out.push_back(expr.phone);
    } else if (expr.kind == NtpExpr::Kind::Seq) {
        for (const auto& e : expr.seq) collectFirstPathPhones(e, out, includeOptional);
    } else if (expr.kind == NtpExpr::Kind::Alt) {
        if (expr.alts.empty()) return;
        std::size_t altIndex = 0;
        if (includeOptional && expr.alts.size() > 1 && expr.alts[0].empty()) {
            altIndex = 1; // for square-bracket optional material, show the included diagnostic path
        }
        for (const auto& e : expr.alts[altIndex]) collectFirstPathPhones(e, out, includeOptional);
    }
}

std::vector<std::string> parseNtpFirstPathPhoneSequence(const std::string& ntpAfterSubstitutions) {
    NtpParser parser(tokenizeNtpExpression(ntpAfterSubstitutions));
    const NtpExpr root = parser.parseRoot();
    std::vector<std::string> out;
    collectFirstPathPhones(root, out, true);
    return out;
}

std::map<std::string, int> readPartsFile(const std::filesystem::path& path) {
    std::map<std::string, int> out;
    std::istringstream in(readTextFile(path));
    std::string line;
    while (std::getline(in, line)) {
        line = stripTrailingSemicolon(line);
        if (line.empty() || startsWith(line, "//") || line.find(":=") != std::string::npos || line.find('=') != std::string::npos) {
            continue;
        }
        const auto toks = tokenizeTclListLike(line);
        if (toks.size() >= 2) {
            out[toks[0]] = std::stoi(toks[1]);
        }
    }
    return out;
}

std::vector<SpecEntry> readSpecEntries(const std::filesystem::path& path) {
    std::vector<SpecEntry> entries;
    std::istringstream in(readTextFile(path));
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || startsWith(line, "//")) {
            continue;
        }
        const auto toksRaw = tokenizeTclListLike(stripTrailingSemicolon(line));
        if (toksRaw.empty()) {
            continue;
        }
        const std::string& word1 = toksRaw.front();
        if (word1 == "duration_model" || word1 == "features" || word1 == "feat_context" ||
            word1 == "frame_size" || word1 == "sampling_freq" || word1 == "map") {
            continue;
        }
        if (line.find(":=") != std::string::npos) {
            continue;
        }
        if (toksRaw.size() == 5) {
            SpecEntry e;
            e.state = toksRaw[0];
            e.index = std::stoi(toksRaw[1]);
            e.dur1 = std::stoi(toksRaw[2]);
            e.dur2 = std::stoi(toksRaw[3]);
            e.tie = toksRaw[4];
            entries.push_back(std::move(e));
        }
    }
    return entries;
}

std::map<std::string, FeatureMapEntry> readFeatureMap(const std::filesystem::path& path) {
    std::map<std::string, FeatureMapEntry> out;
    std::istringstream in(readTextFile(path));
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || startsWith(line, "//")) {
            continue;
        }
        const auto toks = tokenizeTclListLike(line);
        if (toks.size() >= 4) {
            FeatureMapEntry e;
            e.feature = tokenizeTclListLike(toks[1]);
            e.leftContext = tokenizeTclListLike(toks[2]);
            e.rightContext = tokenizeTclListLike(toks[3]);
            out[toks[0]] = std::move(e);
        }
    }
    out[".garbage"] = FeatureMapEntry{{".garbage"}, {"clo"}, {"clo"}};
    return out;
}

GeneratedGraphBundle buildGraphBundleFromNtpText(const std::string& ntpText,
                                                  const std::filesystem::path& faRoot) {
    GeneratedGraphBundle b;
    b.ntpOriginal = joinLinesAsSpaces(ntpText);
    b.ntpAfterSubstitutions = canonicalizeNtpTopLevelAlternatives(applyFaNtpSubstitutions(ntpText));
    const auto parts = readPartsFile(faRoot / "phonemes_new_16k.parts");
    b.graph = buildExpandedNtpGraph(b.ntpAfterSubstitutions, parts, &b.phones);
    b.mappings = buildMappingsForGraph(b.graph, faRoot);
    return b;
}

GeneratedGraphBundle buildGraphBundleFromNtpFile(const std::filesystem::path& ntpFile,
                                                  const std::filesystem::path& faRoot) {
    return buildGraphBundleFromNtpText(readTextFile(ntpFile), faRoot);
}

BackendTraceData applyGeneratedMappingsToTraceNnets(BackendTraceData data,
                                                    const GeneratedMappings& mappings) {
    data.featStatesM = mappings.featStatesM;
    data.featStatesP = mappings.featStatesP;
    data.featStatesH = mappings.featStatesH;
    data.depList = mappings.depList;
    data.vcList = mappings.vcList;
    data.transStatesM = mappings.transStatesM;
    data.transStatesP = mappings.transStatesP;
    data.transStatesH = mappings.transStatesH;
    return data;
}

std::vector<std::string> phnProbLabels(const GeneratedGraphBundle& bundle) {
    return bundle.graph.uniqueStates;
}

std::vector<std::string> transProbLabels(const GeneratedGraphBundle& bundle) {
    return bundle.graph.phnToFrom;
}

void writeGraphBundle(const GeneratedGraphBundle& bundle,
                      const std::filesystem::path& outDir,
                      const std::filesystem::path& faRoot) {
    ensureDirectory(outDir);
    ensureDirectory(outDir / "text");
    ensureDirectory(outDir / "graph");
    ensureDirectory(outDir / "mapping");

    {
        std::ofstream out(outDir / "text" / "ntp_after_substitutions.txt");
        out << bundle.ntpAfterSubstitutions << "\n";
    }
    writeIndexValueList(bundle.phones, outDir / "text" / "phones.list.tsv");

    writeIndexValueList(bundle.graph.statesInitial, outDir / "graph" / "states_initial.list.tsv");
    writeIndexValueList(bundle.graph.uniqueStates, outDir / "graph" / "unique_states.list.tsv");
    writeIndexValueList(bundle.graph.phnToFrom, outDir / "graph" / "phn_to_from.list.tsv");
    writeIndexValueList(bundle.graph.stateToFrom, outDir / "graph" / "state_to_from.list.tsv");
    writeIndexLists(bundle.graph.stateToFromIdx, outDir / "graph" / "state_to_from_idx.list.tsv");

    writeIndexLists(bundle.mappings.featStatesM, outDir / "mapping" / "feat_states_idx_m.list.tsv");
    writeIndexLists(bundle.mappings.featStatesP, outDir / "mapping" / "feat_states_idx_p.list.tsv");
    writeIndexLists(bundle.mappings.featStatesH, outDir / "mapping" / "feat_states_idx_h.list.tsv");
    writeIndexValueList(bundle.mappings.featPhnM, outDir / "mapping" / "feat_states_phn_m.list.tsv");
    writeIndexValueList(bundle.mappings.featPhnP, outDir / "mapping" / "feat_states_phn_p.list.tsv");
    writeIndexValueList(bundle.mappings.featPhnH, outDir / "mapping" / "feat_states_phn_h.list.tsv");
    writeIntList(bundle.mappings.depList, outDir / "mapping" / "dep_list.list.tsv");
    writeIntList(bundle.mappings.vcList, outDir / "mapping" / "vc_list.list.tsv");
    writeIndexLists(bundle.mappings.transStatesM, outDir / "mapping" / "trans_states_m.list.tsv");
    writeIndexLists(bundle.mappings.transStatesP, outDir / "mapping" / "trans_states_p.list.tsv");
    writeIndexLists(bundle.mappings.transStatesH, outDir / "mapping" / "trans_states_h.list.tsv");

    if (!faRoot.empty()) {
        writeTempSpec(bundle, faRoot, outDir / "graph" / "temp_spec.txt");
    }
}

} // namespace cslu
