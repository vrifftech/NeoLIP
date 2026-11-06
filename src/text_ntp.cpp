#include "work_control.hpp"
#include "text_ntp.hpp"
#include "tsv.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace cslu {
namespace {

struct PronEntry {
    std::string pron;
    std::string source;
};

static std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

static std::string toLowerAscii(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

static std::vector<std::string> splitWhitespace(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string tok;
    while (in >> tok) out.push_back(tok);
    return out;
}

static std::string joinWords(const std::vector<std::string>& words, const std::string& sep = " ") {
    std::string out;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if ((i & 63) == 0) checkWork();
        if (i) out += sep;
        out += words[i];
    }
    return out;
}

static std::string loadTextOrLiteral(const std::string& textOrFile) {
    const std::filesystem::path p(textOrFile);
    if (std::filesystem::exists(p) && std::filesystem::is_regular_file(p)) {
        std::ifstream in(p, std::ios::binary);
        if (!in) throw std::runtime_error("cannot read text file: " + p.string());
        std::string line;
        std::vector<std::string> lines;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        return joinWords(lines, " ");
    }
    return textOrFile;
}

static std::string csluNormalizeText(const std::string& original,
                                     std::vector<std::string>& words,
                                     std::vector<TranscriptToken>& tokens,
                                     const TextToNtpOptions& options) {
    tokens = tokenizeTranscript(original, options.cleanupTlkMarkup,
                                options.stripBraceStageDirections);
    words.clear();
    for (const auto& token : tokens) words.push_back(token.word);
    return joinWords(words, " ");
}

static std::map<std::string, PronEntry> readCustomDictionary(const std::filesystem::path& path,
                                                             const std::string& sourceName) {
    std::map<std::string, PronEntry> out;
    if (path.empty() || !std::filesystem::exists(path)) return out;
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read custom dictionary: " + path.string());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        std::string s = trim(line);
        if (s.empty()) continue;
        const std::size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string word = trim(s.substr(0, eq));
        std::string pron = trim(s.substr(eq + 1));
        if (!pron.empty() && pron.back() == ';') pron.pop_back();
        pron = trim(pron);
        if (word.empty() || pron.empty()) continue;
        out[toLowerAscii(word)] = PronEntry{pron, sourceName};
    }
    return out;
}

static std::vector<std::string> tokenizePronExpression(const std::string& s) {
    std::vector<std::string> toks;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            toks.push_back(cur);
            cur.clear();
        }
    };
    for (char ch : s) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            flush();
        } else if (ch == '(' && !cur.empty() && cur.back() == '\\') {
            // CSLU custom dictionaries use escaped phones such as d_\( and th_\(.
            // Treat the escaped open paren as part of the phone token, not as a
            // pronunciation-group delimiter. The following ')' remains the group close.
            cur.push_back(ch);
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

static std::size_t findMatching(const std::vector<std::string>& toks, std::size_t open, const std::string& closeTok) {
    int depth = 0;
    for (std::size_t i = open; i < toks.size(); ++i) {
        if ((i & 63) == 0) checkWork();
        if (toks[i] == "(" || toks[i] == "[") ++depth;
        else if (toks[i] == ")" || toks[i] == "]") {
            --depth;
            if (depth == 0 && toks[i] == closeTok) return i;
        }
    }
    return toks.size();
}

static std::vector<std::pair<std::size_t, std::size_t>> splitTopLevelAlternatives(const std::vector<std::string>& toks,
                                                                                  std::size_t begin,
                                                                                  std::size_t end) {
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    int depth = 0;
    std::size_t start = begin;
    for (std::size_t i = begin; i < end; ++i) {
        if ((i & 63) == 0) checkWork();
        if (toks[i] == "(" || toks[i] == "[") ++depth;
        else if (toks[i] == ")" || toks[i] == "]") --depth;
        else if (toks[i] == "|" && depth == 0) {
            ranges.push_back({start, i});
            start = i + 1;
        }
    }
    ranges.push_back({start, end});
    return ranges;
}

static std::vector<std::string> flattenPronTokens(const std::vector<std::string>& toks, std::size_t begin, std::size_t end) {
    const auto alts = splitTopLevelAlternatives(toks, begin, end);
    std::size_t aBegin = alts.front().first;
    std::size_t aEnd = alts.front().second;
    std::vector<std::string> out;
    for (std::size_t i = aBegin; i < aEnd; ++i) {
        if ((i & 63) == 0) checkWork();
        if (toks[i] == "(") {
            const std::size_t j = findMatching(toks, i, ")");
            if (j == toks.size()) continue;
            auto sub = flattenPronTokens(toks, i + 1, j);
            out.insert(out.end(), sub.begin(), sub.end());
            i = j;
        } else if (toks[i] == "[") {
            const std::size_t j = findMatching(toks, i, "]");
            if (j == toks.size()) continue;
            auto sub = flattenPronTokens(toks, i + 1, j);
            out.insert(out.end(), sub.begin(), sub.end());
            i = j;
        } else if (toks[i] == ")" || toks[i] == "]" || toks[i] == "|") {
            continue;
        } else {
            out.push_back(toks[i]);
        }
    }
    return out;
}

static std::vector<std::string> linearizePronunciation(const std::string& pron) {
    const auto toks = tokenizePronExpression(pron);
    return flattenPronTokens(toks, 0, toks.size());
}

static std::string csluAutomaticFormat(
        const std::vector<std::pair<std::string, std::string>>& wordPron,
        bool includeInitialPause,
        bool includeFinalPause) {
    std::vector<std::string> elements;
    if (includeInitialPause) {
        elements.push_back("[.pau]");
    }
    std::size_t counter = 0;
    for (std::size_t i = 0; i < wordPron.size(); ++i) {
        if ((i & 63) == 0) checkWork();
        const auto& wp = wordPron[i];
        const std::string& word = wp.first;
        std::string phones = wp.second;
        if (word == ".pau" && counter == 0) elements.clear();
        ++counter;
        if (word == ".pau") phones = ".pau";
        if (word == ".garbage") phones = ".garbage";
        elements.push_back(phones);
        const bool isLastWord = i + 1 == wordPron.size();
        if (word != ".pau" && (!isLastWord || includeFinalPause)) {
            elements.push_back("[.pau]");
        }
    }
    return joinWords(elements, " ");
}

static bool lookupAtomicPronunciation(const std::string& word,
                                      const std::map<std::string, PronEntry>& user,
                                      const std::map<std::string, PronEntry>& builtin,
                                      const TextToNtpOptions&,
                                      PronEntry& entry) {
    if (word == ".pau" || word == ".garbage") { entry = {word, "literal"}; return true; }
    const auto custom = user.find(word);
    if (custom != user.end()) { entry = custom->second; return true; }
    const auto found = builtin.find(word);
    if (found == builtin.end()) return false;
    entry = found->second;
    return true;
}

static bool isAsciiAlphaString(const std::string& s) {
    if (s.empty()) return false;
    for (char ch : s) {
        if (!std::isalpha(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

static bool isAllowedClosedCompoundPart(const std::string& part) {
    // Closed compounds are intentionally conservative. This avoids treating many
    // KOTOR proper names as accidental compounds just because a broad dictionary
    // happens to contain short name fragments.
    static const std::set<std::string> allowed = {
        "able", "access", "air", "all", "auto", "back", "bag", "bags", "bank", "banks",
        "battle", "bay", "beam", "beast", "bike", "bio", "blade", "bladed", "blast", "blaster",
        "blood", "blow", "body", "bow", "card", "caster", "class", "click", "clicking", "clock",
        "clockwise", "com", "console", "control", "controller", "counter", "cross", "cry", "cut",
        "data", "deet", "defense", "density", "double", "down", "drive", "dura", "effect", "effective",
        "enviro", "face", "field", "fire", "floor", "forge", "free", "friend", "game", "gear", "guard",
        "hand", "handed", "hard", "head", "high", "holder", "home", "hydro", "hyper", "ing", "junk",
        "key", "killer", "land", "laser", "left", "lethal", "level", "life", "lift", "light", "line",
        "link", "lock", "lower", "map", "master", "meat", "med", "menu", "mini", "most", "non", "off",
        "offline", "one", "pac", "pacs", "pad", "pads", "panel", "particle", "pass", "path", "pause",
        "photo", "place", "play", "point", "points", "poison", "port", "power", "print", "pulse",
        "receptors", "regulator", "right", "rider", "riders", "saber", "sabers", "save", "scene",
        "self", "single", "side", "slime", "speed", "speeder", "spanner", "star", "stars", "steel",
        "stick", "stock", "stocked", "sub", "suit", "swoop", "system", "systems", "target", "tech",
        "turbo", "two", "under", "upload", "upper", "use", "vapor", "voice", "walk", "war", "ware",
        "weapon", "weight", "well", "wired", "world", "worlder", "worlders", "zone"
    };
    return allowed.find(part) != allowed.end();
}

static bool appendCompositionalPart(const std::string& part,
                                    const std::map<std::string, PronEntry>& user,
                                    const std::map<std::string, PronEntry>& builtin,
                                    const TextToNtpOptions& options,
                                    std::vector<std::string>& componentWords,
                                    std::vector<std::string>& phones) {
    PronEntry partEntry;
    if (!lookupAtomicPronunciation(part, user, builtin, options, partEntry)) return false;
    componentWords.push_back(part);
    const std::vector<std::string> flat = linearizePronunciation(partEntry.pron);
    phones.insert(phones.end(), flat.begin(), flat.end());
    return true;
}

static bool composeFromDelimitedParts(const std::string& word,
                                      char delimiter,
                                      const std::map<std::string, PronEntry>& user,
                                      const std::map<std::string, PronEntry>& builtin,
                                      const TextToNtpOptions& options,
                                      PronEntry& entry) {
    if (word.find(delimiter) == std::string::npos) return false;

    std::vector<std::string> parts;
    std::string cur;
    for (char ch : word) {
        if (ch == delimiter) {
            if (cur.empty()) return false;
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (cur.empty()) return false;
    parts.push_back(cur);
    if (parts.size() < 2 || parts.size() > 4) return false;

    std::vector<std::string> componentWords;
    std::vector<std::string> phones;
    for (const std::string& part : parts) {
        if (part.size() < 3 || !isAsciiAlphaString(part)) return false;
        if (!appendCompositionalPart(part, user, builtin, options, componentWords, phones)) return false;
    }

    entry = PronEntry{joinWords(phones, " "), "compositional:" + joinWords(componentWords, "+")};
    return true;
}

struct CompositionPart {
    std::string word;
    PronEntry entry;
};

static bool segmentAlphaCompound(const std::string& word,
                                 std::size_t pos,
                                 int remainingParts,
                                 const std::map<std::string, PronEntry>& user,
                                 const std::map<std::string, PronEntry>& builtin,
                                 const TextToNtpOptions& options,
                                 std::vector<CompositionPart>& path) {
    constexpr std::size_t minPartLen = 3;
    const std::size_t n = word.size();
    if (remainingParts == 0) return pos == n;
    if (pos >= n) return false;
    if (n - pos < minPartLen * static_cast<std::size_t>(remainingParts)) return false;

    if (remainingParts == 1) {
        const std::string part = word.substr(pos);
        if (part.size() < minPartLen || !isAsciiAlphaString(part) || !isAllowedClosedCompoundPart(part)) return false;
        PronEntry partEntry;
        if (!lookupAtomicPronunciation(part, user, builtin, options, partEntry)) return false;
        path.push_back(CompositionPart{part, partEntry});
        return true;
    }

    const std::size_t maxEnd = n - minPartLen * static_cast<std::size_t>(remainingParts - 1);
    for (std::size_t end = pos + minPartLen; end <= maxEnd; ++end) {
        const std::string part = word.substr(pos, end - pos);
        PronEntry partEntry;
        if (!isAllowedClosedCompoundPart(part)) continue;
        if (!lookupAtomicPronunciation(part, user, builtin, options, partEntry)) continue;
        path.push_back(CompositionPart{part, partEntry});
        if (segmentAlphaCompound(word, end, remainingParts - 1,
                                 user, builtin, options, path)) {
            return true;
        }
        path.pop_back();
    }
    return false;
}

static bool composeFromAlphaCompound(const std::string& word,
                                     const std::map<std::string, PronEntry>& user,
                                     const std::map<std::string, PronEntry>& builtin,
                                     const TextToNtpOptions& options,
                                     PronEntry& entry) {
    if (word.size() < 6 || word.size() > 40 || !isAsciiAlphaString(word)) return false;

    for (int partCount = 2; partCount <= 4; ++partCount) {
        std::vector<CompositionPart> path;
        if (!segmentAlphaCompound(word, 0, partCount, user, builtin, options, path)) continue;

        std::vector<std::string> componentWords;
        std::vector<std::string> phones;
        for (const CompositionPart& part : path) {
            componentWords.push_back(part.word);
            const std::vector<std::string> flat = linearizePronunciation(part.entry.pron);
            phones.insert(phones.end(), flat.begin(), flat.end());
        }
        entry = PronEntry{joinWords(phones, " "), "compositional:" + joinWords(componentWords, "+")};
        return true;
    }
    return false;
}

static bool lookupRootPronunciation(const std::string& word,
                                    const std::map<std::string, PronEntry>& user,
                                    const std::map<std::string, PronEntry>& builtin,
                                    const TextToNtpOptions& options,
                                    PronEntry& entry);

static bool isSibilantFinalPhone(const std::string& phone) {
    return phone == "s" || phone == "z" || phone == "S" || phone == "Z" ||
           phone == "tS" || phone == "dZ";
}

static bool isVoicelessFinalPhone(const std::string& phone) {
    return phone == "ph" || phone == "th" || phone == "kh" ||
           phone == "f" || phone == "T";
}

static std::vector<std::string> suffixPhonesForSibilantAwareS(const std::vector<std::string>& basePhones,
                                                              std::string& suffixLabel) {
    if (basePhones.empty()) {
        suffixLabel = "z";
        return {"z"};
    }
    const std::string& last = basePhones.back();
    if (isSibilantFinalPhone(last)) {
        suffixLabel = "iz";
        return {"I", "z"};
    }
    if (isVoicelessFinalPhone(last)) {
        suffixLabel = "s";
        return {"s"};
    }
    suffixLabel = "z";
    return {"z"};
}

static bool composeInflectionFromBase(const std::string& word,
                                      const std::string& base,
                                      const std::string& writtenSuffix,
                                      const std::map<std::string, PronEntry>& user,
                                      const std::map<std::string, PronEntry>& builtin,
                                      const TextToNtpOptions& options,
                                      PronEntry& entry) {
    if (base.empty() || base == word ||
        !std::isalpha(static_cast<unsigned char>(base.front())) ||
        !std::isalpha(static_cast<unsigned char>(base.back()))) return false;
    PronEntry baseEntry;
    if (!lookupRootPronunciation(base, user, builtin, options, baseEntry)) return false;
    std::vector<std::string> phones = linearizePronunciation(baseEntry.pron);
    std::string suffixLabel;
    const std::vector<std::string> suffixPhones = suffixPhonesForSibilantAwareS(phones, suffixLabel);
    phones.insert(phones.end(), suffixPhones.begin(), suffixPhones.end());
    entry = PronEntry{joinWords(phones, " "), "inflection:" + base + "+" + writtenSuffix + ":" + suffixLabel};
    return true;
}

static bool lookupInflectionPronunciation(const std::string& word,
                                          const std::map<std::string, PronEntry>& user,
                                          const std::map<std::string, PronEntry>& builtin,
                                          const TextToNtpOptions& options,
                                          PronEntry& entry) {
    if (!options.useInflectionPronunciations) return false;
    if (word.size() < 4) return false;

    if (word.size() > 2 && word.compare(word.size() - 2, 2, "'s") == 0) {
        const std::string base = word.substr(0, word.size() - 2);
        if (composeInflectionFromBase(word, base, "'s", user, builtin, options, entry)) return true;
    }

    if (word.size() > 3 && word.compare(word.size() - 3, 3, "ies") == 0) {
        const std::string base = word.substr(0, word.size() - 3) + "y";
        if (composeInflectionFromBase(word, base, "ies", user, builtin, options, entry)) return true;
    }

    if (word.size() > 2 && word.compare(word.size() - 2, 2, "es") == 0) {
        const std::string base = word.substr(0, word.size() - 2);
        if (composeInflectionFromBase(word, base, "es", user, builtin, options, entry)) return true;
    }

    if (word.size() > 1 && word.back() == 's' && !(word.size() > 2 && word[word.size() - 2] == 's')) {
        const std::string base = word.substr(0, word.size() - 1);
        if (composeInflectionFromBase(word, base, "s", user, builtin, options, entry)) return true;
    }

    return false;
}

static bool lookupCompositionalPronunciation(const std::string& word,
                                             const std::map<std::string, PronEntry>& user,
                                             const std::map<std::string, PronEntry>& builtin,
                                             const TextToNtpOptions& options,
                                             PronEntry& entry) {
    if (!options.useCompositionalPronunciations) return false;

    // Hyphenated compounds are common in the raw TLK scan, e.g. right-most and left-most.
    if (composeFromDelimitedParts(word, '-', user, builtin, options, entry)) return true;

    // Closed compounds such as lightsaber are resolved only when every component is already known.
    if (composeFromAlphaCompound(word, user, builtin, options, entry)) return true;

    return false;
}

static bool lookupRootPronunciation(const std::string& word,
                                    const std::map<std::string, PronEntry>& user,
                                    const std::map<std::string, PronEntry>& builtin,
                                    const TextToNtpOptions& options,
                                    PronEntry& entry) {
    if (lookupAtomicPronunciation(word, user, builtin, options, entry)) return true;
    if (lookupCompositionalPronunciation(word, user, builtin, options, entry)) return true;
    return false;
}

static bool lookupPronunciation(const std::string& word,
                                const std::map<std::string, PronEntry>& user,
                                const std::map<std::string, PronEntry>& builtin,
                                const TextToNtpOptions& options,
                                PronEntry& entry) {
    if (lookupRootPronunciation(word, user, builtin, options, entry)) return true;
    if (lookupInflectionPronunciation(word, user, builtin, options, entry)) return true;
    return false;
}

static std::string tsvEscapeText(std::string s) {
    std::string out;
    for (char ch : s) {
        if (ch == '\t' || ch == '\r' || ch == '\n') out.push_back(' ');
        else out.push_back(ch);
    }
    return out;
}

static std::map<std::string, PronEntry> readLexicon(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read bundled lexicon: " + path.string());
    std::map<std::string, PronEntry> entries;
    std::string line;
    while (std::getline(in, line)) {
        checkWork();
        const auto tab = line.find('\t');
        entries.emplace(line.substr(0, tab), PronEntry{trim(line.substr(tab + 1)), "built-in"});
    }
    return entries;
}

} // namespace

struct TextToNtpResolver::Impl {
    explicit Impl(TextToNtpOptions opts)
        : options(std::move(opts)),
          user(readCustomDictionary(options.userDictionary, "user")),
          builtin(readLexicon(options.dictionary)) {}

    TextToNtpResult resolve(const std::string& text, bool literal = false) {
        TextToNtpResult r;
        r.originalText = literal ? text : loadTextOrLiteral(text);
        r.normalizedText = csluNormalizeText(r.originalText, r.words, r.tokens, options);

        std::vector<std::pair<std::string, std::string>> wordPron;
        wordPron.reserve(r.words.size());
        for (std::size_t i = 0; i < r.words.size(); ++i) {
            if ((i & 63) == 0) checkWork();
            const std::string& word = r.words[i];
            PronEntry entry;
            if (lookupPronunciation(word, user, builtin, options, entry)) {
                wordPron.push_back({word, entry.pron});
                r.wordInfo.push_back(WordPronunciationInfo{i, word, entry.pron, entry.source});
            } else {
                r.unknownWords.push_back(word);
                r.wordInfo.push_back(WordPronunciationInfo{i, word, "", "UNKNOWN"});
            }
        }
        if (!r.unknownWords.empty()) {
            std::ostringstream ss;
            ss << "unknown words:";
            for (const auto& w : r.unknownWords) ss << ' ' << w;
            r.notes.push_back(ss.str());
            if (options.failOnUnknown) throw std::runtime_error(ss.str());
            return r;
        }

        r.ntp = csluAutomaticFormat(wordPron, options.includeInitialPause, options.includeFinalPause);
        return r;
    }

    TextToNtpOptions options;
    std::map<std::string, PronEntry> user, builtin;
};

TextToNtpResolver::TextToNtpResolver(TextToNtpOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
TextToNtpResolver::~TextToNtpResolver() = default;
TextToNtpResolver::TextToNtpResolver(TextToNtpResolver&&) noexcept = default;
TextToNtpResolver& TextToNtpResolver::operator=(TextToNtpResolver&&) noexcept = default;
TextToNtpResult TextToNtpResolver::resolveSource(const std::string& textOrFile) {
    return impl_->resolve(textOrFile);
}

TextToNtpResult TextToNtpResolver::resolveText(const std::string& text) {
    return impl_->resolve(text, true);
}

TextToNtpResult textToNtpFromSource(const std::string& textOrFile, const TextToNtpOptions& options) {
    TextToNtpResolver resolver(options);
    return resolver.resolveSource(textOrFile);
}

void writeNtpFile(const std::filesystem::path& path, const std::string& ntp) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write .ntp file: " + path.string());
    out << ntp << "\n";
}

void writeTextToNtpSourceReport(const std::filesystem::path& path, const TextToNtpResult& result) {
    if (path.empty()) return;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write source report: " + path.string());
    out << "index\tword\tsource\tpronunciation\n";
    for (const auto& wi : result.wordInfo) {
        out << wi.index << '\t' << tsvEscapeText(wi.word) << '\t' << tsvEscapeText(wi.source)
            << '\t' << tsvEscapeText(wi.pronunciation) << '\n';
    }
}

void writeTextToNtpUnknownReport(const std::filesystem::path& path, const TextToNtpResult& result) {
    if (path.empty()) return;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write unknown-word report: " + path.string());
    out << "word\tsuggested_override_line\n";
    for (const auto& word : result.unknownWords) {
        out << tsvEscapeText(word) << '\t' << tsvEscapeText(word) << " = ;\n";
    }
}

} // namespace cslu
