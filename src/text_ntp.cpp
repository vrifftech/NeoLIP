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

static std::string sourceNameForPath(const std::string& prefix, const std::filesystem::path& p) {
    return prefix + ":" + p.filename().string();
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

static std::uint32_t be32(const std::vector<unsigned char>& data, std::size_t pos) {
    if (pos + 4 > data.size()) throw std::runtime_error("truncated .ob dictionary file");
    return (static_cast<std::uint32_t>(data[pos]) << 24) |
           (static_cast<std::uint32_t>(data[pos + 1]) << 16) |
           (static_cast<std::uint32_t>(data[pos + 2]) << 8) |
           static_cast<std::uint32_t>(data[pos + 3]);
}

static std::string stripStress(std::string pron) {
    pron = std::regex_replace(pron, std::regex("_[123]"), "");
    return pron;
}

static std::map<std::string, PronEntry> parseObDictionaryFile(const std::filesystem::path& file) {
    std::map<std::string, PronEntry> out;
    if (!std::filesystem::exists(file)) return out;
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read CMU .ob dictionary: " + file.string());
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t pos = 0;
    while (pos < data.size()) {
        const std::size_t wordStart = pos;
        while (pos < data.size() && data[pos] != 0) ++pos;
        if (pos >= data.size()) break;
        std::string word(reinterpret_cast<const char*>(&data[wordStart]), pos - wordStart);
        ++pos;
        if (word.empty()) break;
        if (pos + 13 > data.size()) break;
        if (data[pos] != 0x0a) break;
        pos += 1;
        pos += 4;
        pos += 4;
        if (pos + 5 > data.size()) break;
        if (!(data[pos] == 'r' && data[pos + 1] == 'a' && data[pos + 2] == 'w' && data[pos + 3] == 0 && data[pos + 4] == 0x0a)) {
            break;
        }
        pos += 5;
        const std::uint32_t len = be32(data, pos);
        pos += 4;
        if (pos + len > data.size()) break;
        std::string pron(reinterpret_cast<const char*>(&data[pos]), len);
        pos += len;
        if (!pron.empty() && pron.back() == '\0') pron.pop_back();
        if (pos < data.size() && data[pos] == 0) ++pos;
        if (!word.empty()) out[word] = PronEntry{stripStress(pron), "cmu_ob:" + file.filename().string()};
    }
    return out;
}

class CmuObDictionary {
public:
    explicit CmuObDictionary(std::filesystem::path dir) : dir_(std::move(dir)) {}

    bool lookup(const std::string& word, PronEntry& entry) {
        if (word.empty()) return false;
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(word[0])));
        if (c < 'a' || c > 'z') return false;
        auto it = cache_.find(c);
        if (it == cache_.end()) {
            const std::filesystem::path file = dir_ / (std::string(1, c) + ".ob");
            it = cache_.emplace(c, parseObDictionaryFile(file)).first;
        }
        auto jt = it->second.find(word);
        if (jt == it->second.end()) return false;
        entry = jt->second;
        return true;
    }

private:
    std::filesystem::path dir_;
    std::map<char, std::map<std::string, PronEntry>> cache_;
};

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

static bool extractFirstQuotedString(const std::string& text, std::string& value) {
    const std::size_t q = text.find('"');
    if (q == std::string::npos) return false;
    value.clear();
    bool esc = false;
    for (std::size_t i = q + 1; i < text.size(); ++i) {
        if ((i & 63) == 0) checkWork();
        const char ch = text[i];
        if (esc) {
            if (ch == 'n') value.push_back('\n');
            else if (ch == 't') value.push_back('\t');
            else value.push_back(ch);
            esc = false;
        } else if (ch == '\\') {
            esc = true;
        } else if (ch == '"') {
            return true;
        } else {
            value.push_back(ch);
        }
    }
    return false;
}

static std::vector<std::string> extractSyllablePhones(const std::string& text) {
    std::vector<std::string> phones;
    static const std::regex syll(R"(\(\s*\(([^()]*)\)\s+[0-9]+\s*\))");
    auto begin = std::sregex_iterator(text.begin(), text.end(), syll);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string group = (*it)[1].str();
        auto toks = splitWhitespace(group);
        phones.insert(phones.end(), toks.begin(), toks.end());
    }
    return phones;
}

static std::vector<std::string> convertOgiPhonesToCsluPhones(const std::vector<std::string>& in) {
    std::vector<std::string> out;
    for (const std::string& raw : in) {
        const std::string p = trim(raw);
        if (p.empty()) continue;
        if (p == "p") { out.push_back("pc"); out.push_back("ph"); }
        else if (p == "t") { out.push_back("tc"); out.push_back("th"); }
        else if (p == "k") { out.push_back("kc"); out.push_back("kh"); }
        else if (p == "b") { out.push_back("bc"); out.push_back("b"); }
        else if (p == "d") { out.push_back("dc"); out.push_back("d"); }
        else if (p == "g") { out.push_back("gc"); out.push_back("g"); }
        else if (p == "tS") { out.push_back("tSc"); out.push_back("tS"); }
        else if (p == "dZ") { out.push_back("dZc"); out.push_back("dZ"); }
        else { out.push_back(p); }
    }
    return out;
}

static std::string ogiPhonesToCsluPron(const std::vector<std::string>& phones) {
    return joinWords(convertOgiPhonesToCsluPhones(phones), " ");
}

static std::vector<std::string> extractBalancedLexAddEntryChunks(const std::string& text) {
    std::vector<std::string> chunks;
    std::size_t pos = 0;
    const std::string needle = "(lex.add.entry";
    while (true) {
        const std::size_t start = text.find(needle, pos);
        if (start == std::string::npos) break;
        int depth = 0;
        bool inString = false;
        bool esc = false;
        for (std::size_t i = start; i < text.size(); ++i) {
            if ((i & 63) == 0) checkWork();
            const char ch = text[i];
            if (inString) {
                if (esc) esc = false;
                else if (ch == '\\') esc = true;
                else if (ch == '"') inString = false;
            } else {
                if (ch == '"') inString = true;
                else if (ch == '(') ++depth;
                else if (ch == ')') {
                    --depth;
                    if (depth == 0) {
                        chunks.push_back(text.substr(start, i - start + 1));
                        pos = i + 1;
                        break;
                    }
                }
            }
        }
        if (pos <= start) break;
    }
    return chunks;
}

class OgiDictionary {
public:
    OgiDictionary(std::filesystem::path lexiconPath, std::filesystem::path addendaPath, bool useLex, bool useAddenda)
        : lexiconPath_(std::move(lexiconPath)), addendaPath_(std::move(addendaPath)), useLex_(useLex), useAddenda_(useAddenda) {}

    bool lookup(const std::string& word, PronEntry& entry) {
        ensureLoaded();
        if (useAddenda_) {
            auto ai = addenda_.find(word);
            if (ai != addenda_.end()) { entry = ai->second; return true; }
        }
        if (useLex_) {
            auto li = lexicon_.find(word);
            if (li != lexicon_.end()) { entry = li->second; return true; }
        }
        return false;
    }

    std::size_t lexiconCount() { ensureLoaded(); return lexicon_.size(); }
    std::size_t addendaCount() { ensureLoaded(); return addenda_.size(); }

private:
    void ensureLoaded() {
        if (loaded_) return;
        loaded_ = true;
        if (useLex_) loadLexicon();
        if (useAddenda_) loadAddenda();
    }

    void loadLexicon() {
        if (lexiconPath_.empty() || !std::filesystem::exists(lexiconPath_)) return;
        std::ifstream in(lexiconPath_);
        if (!in) throw std::runtime_error("cannot read OGI lexicon: " + lexiconPath_.string());
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string word;
            if (!extractFirstQuotedString(line, word)) continue;
            auto phones = extractSyllablePhones(line);
            if (phones.empty()) continue;
            std::string pron = ogiPhonesToCsluPron(phones);
            if (pron.empty()) continue;
            const std::string key = toLowerAscii(word);
            if (lexicon_.find(key) == lexicon_.end()) {
                lexicon_[key] = PronEntry{pron, "ogi_lex.out"};
            }
        }
    }

    void loadAddenda() {
        if (addendaPath_.empty() || !std::filesystem::exists(addendaPath_)) return;
        std::ifstream in(addendaPath_, std::ios::binary);
        if (!in) throw std::runtime_error("cannot read OGI addenda: " + addendaPath_.string());
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (const std::string& chunk : extractBalancedLexAddEntryChunks(text)) {
            std::string word;
            if (!extractFirstQuotedString(chunk, word)) continue;
            auto phones = extractSyllablePhones(chunk);
            if (phones.empty()) continue;
            std::string pron = ogiPhonesToCsluPron(phones);
            if (pron.empty()) continue;
            addenda_[toLowerAscii(word)] = PronEntry{pron, "ogi_lex_addenda.scm"};
        }
    }

    std::filesystem::path lexiconPath_;
    std::filesystem::path addendaPath_;
    bool useLex_ = true;
    bool useAddenda_ = true;
    bool loaded_ = false;
    std::map<std::string, PronEntry> lexicon_;
    std::map<std::string, PronEntry> addenda_;
};

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

static std::string simpleLinearFormat(const std::vector<std::pair<std::string, std::string>>& wordPron,
                                      bool includeInitialPause,
                                      bool includeFinalPause) {
    std::vector<std::string> phones;
    if (includeInitialPause) phones.push_back(".pau");
    for (const auto& wp : wordPron) {
        const std::string& word = wp.first;
        const std::string& pron = wp.second;
        if (word == ".pau") {
            if (phones.empty() || phones.back() != ".pau") phones.push_back(".pau");
            continue;
        }
        if (word == ".garbage") {
            phones.push_back(".garbage");
            continue;
        }
        const std::vector<std::string> flat = linearizePronunciation(pron);
        phones.insert(phones.end(), flat.begin(), flat.end());
    }
    if (includeFinalPause && (phones.empty() || phones.back() != ".pau")) phones.push_back(".pau");
    return joinWords(phones, " ");
}

static bool lookupInMaps(const std::string& word,
                         const std::vector<std::map<std::string, PronEntry>>& maps,
                         PronEntry& entry) {
    for (const auto& m : maps) {
        auto it = m.find(word);
        if (it != m.end()) { entry = it->second; return true; }
    }
    return false;
}

static bool lookupAtomicPronunciation(const std::string& word,
                                      const std::vector<std::map<std::string, PronEntry>>& overrideMaps,
                                      const std::map<std::string, PronEntry>& builtinCustom,
                                      CmuObDictionary& cmu,
                                      OgiDictionary& ogi,
                                      const TextToNtpOptions& options,
                                      PronEntry& entry) {
    if (word == ".pau") { entry = PronEntry{".pau", "literal"}; return true; }
    if (word == ".garbage") { entry = PronEntry{".garbage", "literal"}; return true; }

    if (lookupInMaps(word, overrideMaps, entry)) return true;

    auto ci = builtinCustom.find(word);
    if (options.customFirst && ci != builtinCustom.end()) {
        entry = ci->second;
        return true;
    }

    if (cmu.lookup(word, entry)) {
        if (!options.customFirst && ci != builtinCustom.end()) {
            entry = ci->second;
            return true;
        }
        return true;
    }

    if (ci != builtinCustom.end()) {
        entry = ci->second;
        return true;
    }

    if ((options.useOgiLexicon || options.useOgiAddenda) && ogi.lookup(word, entry)) return true;

    return false;
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
                                    const std::vector<std::map<std::string, PronEntry>>& overrideMaps,
                                    const std::map<std::string, PronEntry>& builtinCustom,
                                    CmuObDictionary& cmu,
                                    OgiDictionary& ogi,
                                    const TextToNtpOptions& options,
                                    std::vector<std::string>& componentWords,
                                    std::vector<std::string>& phones) {
    PronEntry partEntry;
    if (!lookupAtomicPronunciation(part, overrideMaps, builtinCustom, cmu, ogi, options, partEntry)) return false;
    componentWords.push_back(part);
    const std::vector<std::string> flat = linearizePronunciation(partEntry.pron);
    phones.insert(phones.end(), flat.begin(), flat.end());
    return true;
}

static bool composeFromDelimitedParts(const std::string& word,
                                      char delimiter,
                                      const std::vector<std::map<std::string, PronEntry>>& overrideMaps,
                                      const std::map<std::string, PronEntry>& builtinCustom,
                                      CmuObDictionary& cmu,
                                      OgiDictionary& ogi,
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
        if (!appendCompositionalPart(part, overrideMaps, builtinCustom, cmu, ogi, options, componentWords, phones)) return false;
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
                                 const std::vector<std::map<std::string, PronEntry>>& overrideMaps,
                                 const std::map<std::string, PronEntry>& builtinCustom,
                                 CmuObDictionary& cmu,
                                 OgiDictionary& ogi,
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
        if (!lookupAtomicPronunciation(part, overrideMaps, builtinCustom, cmu, ogi, options, partEntry)) return false;
        path.push_back(CompositionPart{part, partEntry});
        return true;
    }

    const std::size_t maxEnd = n - minPartLen * static_cast<std::size_t>(remainingParts - 1);
    for (std::size_t end = pos + minPartLen; end <= maxEnd; ++end) {
        const std::string part = word.substr(pos, end - pos);
        PronEntry partEntry;
        if (!isAllowedClosedCompoundPart(part)) continue;
        if (!lookupAtomicPronunciation(part, overrideMaps, builtinCustom, cmu, ogi, options, partEntry)) continue;
        path.push_back(CompositionPart{part, partEntry});
        if (segmentAlphaCompound(word, end, remainingParts - 1,
                                 overrideMaps, builtinCustom, cmu, ogi, options, path)) {
            return true;
        }
        path.pop_back();
    }
    return false;
}

static bool composeFromAlphaCompound(const std::string& word,
                                     const std::vector<std::map<std::string, PronEntry>>& overrideMaps,
                                     const std::map<std::string, PronEntry>& builtinCustom,
                                     CmuObDictionary& cmu,
                                     OgiDictionary& ogi,
                                     const TextToNtpOptions& options,
                                     PronEntry& entry) {
    if (word.size() < 6 || word.size() > 40 || !isAsciiAlphaString(word)) return false;

    for (int partCount = 2; partCount <= 4; ++partCount) {
        std::vector<CompositionPart> path;
        if (!segmentAlphaCompound(word, 0, partCount, overrideMaps, builtinCustom, cmu, ogi, options, path)) continue;

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
                                    const std::vector<std::map<std::string, PronEntry>>& overrideMaps,
                                    const std::map<std::string, PronEntry>& builtinCustom,
                                    CmuObDictionary& cmu,
                                    OgiDictionary& ogi,
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
                                      const std::vector<std::map<std::string, PronEntry>>& overrideMaps,
                                      const std::map<std::string, PronEntry>& builtinCustom,
                                      CmuObDictionary& cmu,
                                      OgiDictionary& ogi,
                                      const TextToNtpOptions& options,
                                      PronEntry& entry) {
    if (base.empty() || base == word ||
        !std::isalpha(static_cast<unsigned char>(base.front())) ||
        !std::isalpha(static_cast<unsigned char>(base.back()))) return false;
    PronEntry baseEntry;
    if (!lookupRootPronunciation(base, overrideMaps, builtinCustom, cmu, ogi, options, baseEntry)) return false;
    std::vector<std::string> phones = linearizePronunciation(baseEntry.pron);
    std::string suffixLabel;
    const std::vector<std::string> suffixPhones = suffixPhonesForSibilantAwareS(phones, suffixLabel);
    phones.insert(phones.end(), suffixPhones.begin(), suffixPhones.end());
    entry = PronEntry{joinWords(phones, " "), "inflection:" + base + "+" + writtenSuffix + ":" + suffixLabel};
    return true;
}

static bool lookupInflectionPronunciation(const std::string& word,
                                          const std::vector<std::map<std::string, PronEntry>>& overrideMaps,
                                          const std::map<std::string, PronEntry>& builtinCustom,
                                          CmuObDictionary& cmu,
                                          OgiDictionary& ogi,
                                          const TextToNtpOptions& options,
                                          PronEntry& entry) {
    if (!options.useInflectionPronunciations) return false;
    if (word.size() < 4) return false;

    if (word.size() > 2 && word.compare(word.size() - 2, 2, "'s") == 0) {
        const std::string base = word.substr(0, word.size() - 2);
        if (composeInflectionFromBase(word, base, "'s", overrideMaps, builtinCustom, cmu, ogi, options, entry)) return true;
    }

    if (word.size() > 3 && word.compare(word.size() - 3, 3, "ies") == 0) {
        const std::string base = word.substr(0, word.size() - 3) + "y";
        if (composeInflectionFromBase(word, base, "ies", overrideMaps, builtinCustom, cmu, ogi, options, entry)) return true;
    }

    if (word.size() > 2 && word.compare(word.size() - 2, 2, "es") == 0) {
        const std::string base = word.substr(0, word.size() - 2);
        if (composeInflectionFromBase(word, base, "es", overrideMaps, builtinCustom, cmu, ogi, options, entry)) return true;
    }

    if (word.size() > 1 && word.back() == 's' && !(word.size() > 2 && word[word.size() - 2] == 's')) {
        const std::string base = word.substr(0, word.size() - 1);
        if (composeInflectionFromBase(word, base, "s", overrideMaps, builtinCustom, cmu, ogi, options, entry)) return true;
    }

    return false;
}

static bool lookupCompositionalPronunciation(const std::string& word,
                                             const std::vector<std::map<std::string, PronEntry>>& overrideMaps,
                                             const std::map<std::string, PronEntry>& builtinCustom,
                                             CmuObDictionary& cmu,
                                             OgiDictionary& ogi,
                                             const TextToNtpOptions& options,
                                             PronEntry& entry) {
    if (!options.useCompositionalPronunciations) return false;

    // Hyphenated compounds are common in the raw TLK scan, e.g. right-most and left-most.
    if (composeFromDelimitedParts(word, '-', overrideMaps, builtinCustom, cmu, ogi, options, entry)) return true;

    // Closed compounds such as lightsaber are resolved only when every component is already known.
    if (composeFromAlphaCompound(word, overrideMaps, builtinCustom, cmu, ogi, options, entry)) return true;

    return false;
}

static bool lookupRootPronunciation(const std::string& word,
                                    const std::vector<std::map<std::string, PronEntry>>& overrideMaps,
                                    const std::map<std::string, PronEntry>& builtinCustom,
                                    CmuObDictionary& cmu,
                                    OgiDictionary& ogi,
                                    const TextToNtpOptions& options,
                                    PronEntry& entry) {
    if (lookupAtomicPronunciation(word, overrideMaps, builtinCustom, cmu, ogi, options, entry)) return true;
    if (lookupCompositionalPronunciation(word, overrideMaps, builtinCustom, cmu, ogi, options, entry)) return true;
    return false;
}

static bool lookupPronunciation(const std::string& word,
                                const std::vector<std::map<std::string, PronEntry>>& overrideMaps,
                                const std::map<std::string, PronEntry>& builtinCustom,
                                CmuObDictionary& cmu,
                                OgiDictionary& ogi,
                                const TextToNtpOptions& options,
                                PronEntry& entry) {
    if (lookupRootPronunciation(word, overrideMaps, builtinCustom, cmu, ogi, options, entry)) return true;
    if (lookupInflectionPronunciation(word, overrideMaps, builtinCustom, cmu, ogi, options, entry)) return true;
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

static bool isReadableDictionaryFile(const std::filesystem::path& path) {
    if (path.empty()) return false;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return false;
    const auto size = std::filesystem::file_size(path, error);
    return !error && size > 0;
}

static bool isCmuDictionaryComplete(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) return false;
    for (char letter = 'a'; letter <= 'z'; ++letter) {
        if (!isReadableDictionaryFile(directory / (std::string(1, letter) + ".ob"))) return false;
    }
    return true;
}

static void configureStandaloneDictionaryPaths(TextToNtpOptions& options,
                                                const std::filesystem::path& root) {
    options.customDictionary = root / "custom_dictionary.txt";
    options.cmuDictionaryDir = root / "cmu";
    options.ogiLexiconPath = root / "ogi_lex.out";
    options.ogiAddendaPath = root / "ogi_lex_addenda.scm";
}

static void configureLegacyDictionaryPaths(TextToNtpOptions& options,
                                            const std::filesystem::path& faRoot) {
    options.customDictionary = faRoot / "custom_dictionary.txt";
    options.cmuDictionaryDir = faRoot / ".." / ".." / "script" / "dictionary_1.0" / "cmu";
    options.ogiLexiconPath = faRoot / ".." / ".." / ".." / ".." / "Festival" / "1.4.2" / "festival" / "lib" / "dicts" / "ogi_lex.out";
    options.ogiAddendaPath = faRoot / ".." / ".." / ".." / ".." / "Festival" / "1.4.2" / "festival" / "lib" / "ogi_lex_addenda.scm";
}

static bool hasCompleteBaseDictionaries(const TextToNtpOptions& options) {
    return isReadableDictionaryFile(options.customDictionary) &&
           isCmuDictionaryComplete(options.cmuDictionaryDir) &&
           isReadableDictionaryFile(options.ogiLexiconPath) &&
           isReadableDictionaryFile(options.ogiAddendaPath);
}

static void appendOverrideDictionary(TextToNtpOptions& options,
                                     const std::filesystem::path& path,
                                     bool requireFile) {
    if (!requireFile && !isReadableDictionaryFile(path)) return;
    const std::filesystem::path normalized = path.lexically_normal();
    for (const auto& existing : options.overrideDictionaries) {
        if (existing.lexically_normal() == normalized) return;
    }
    options.overrideDictionaries.push_back(path);
}

static std::vector<std::filesystem::path> missingDictionaryAssets(const TextToNtpOptions& options) {
    std::vector<std::filesystem::path> missing;
    if (!isReadableDictionaryFile(options.customDictionary)) {
        missing.push_back(options.customDictionary);
    }
    for (char letter = 'a'; letter <= 'z'; ++letter) {
        const std::filesystem::path file = options.cmuDictionaryDir / (std::string(1, letter) + ".ob");
        if (!isReadableDictionaryFile(file)) missing.push_back(file);
    }
    if (options.useOgiLexicon && !isReadableDictionaryFile(options.ogiLexiconPath)) {
        missing.push_back(options.ogiLexiconPath);
    }
    if (options.useOgiAddenda && !isReadableDictionaryFile(options.ogiAddendaPath)) {
        missing.push_back(options.ogiAddendaPath);
    }
    for (const auto& path : options.overrideDictionaries) {
        if (!isReadableDictionaryFile(path)) missing.push_back(path);
    }
    return missing;
}

static void requireDictionaryAssets(const TextToNtpOptions& options) {
    const auto missing = missingDictionaryAssets(options);
    if (missing.empty()) return;

    std::ostringstream message;
    message << "NeoLIP pronunciation dictionaries are incomplete. Missing required asset";
    if (missing.size() != 1) message << 's';
    message << ':';
    for (const auto& path : missing) message << "\n  " << path.string();
    message << "\nReinstall NeoLIP or pass --dict-root <dictionaries_dir> containing "
               "custom_dictionary.txt, cmu/a.ob through cmu/z.ob, "
               "ogi_lex.out, ogi_lex_addenda.scm, and kotor_override.dict.";
    throw std::runtime_error(message.str());
}

} // namespace

TextToNtpOptions makeDefaultTextToNtpOptions(const std::filesystem::path& faRoot) {
    TextToNtpOptions options;
    options.faRoot = faRoot;

    // Supports both layouts:
    //   legacy:      <CSLU>/Toolkit/2.0/apps/fa
    //   standalone:  <package>/models with sibling <package>/dictionaries
    // A directory merely existing is not enough: incomplete package folders must
    // not silently shadow the complete CSLU Toolkit dictionaries.
    const std::filesystem::path siblingRoot = faRoot.parent_path() / "dictionaries";

    TextToNtpOptions standalone = options;
    configureStandaloneDictionaryPaths(standalone, siblingRoot);

    TextToNtpOptions legacy = options;
    configureLegacyDictionaryPaths(legacy, faRoot);

    if (hasCompleteBaseDictionaries(standalone)) {
        options = std::move(standalone);
        // Standalone NeoLIP packages always include the built-in KOTOR layer.
        // Keep the expected path even if the installation is damaged so the
        // resolver reports the missing file instead of treating KOTOR names as
        // ordinary unknown words.
        appendOverrideDictionary(options, siblingRoot / "kotor_override.dict", true);
    } else if (hasCompleteBaseDictionaries(legacy)) {
        options = std::move(legacy);
        // A raw CSLU Toolkit does not contain NeoLIP's override. Accept it when
        // supplied beside the models or in a sibling package dictionary folder.
        appendOverrideDictionary(options, siblingRoot / "kotor_override.dict", false);
        appendOverrideDictionary(options, faRoot / "kotor_override.dict", false);
    } else if (std::filesystem::exists(siblingRoot)) {
        options = std::move(standalone);
        appendOverrideDictionary(options, siblingRoot / "kotor_override.dict", true);
    } else {
        options = std::move(legacy);
    }

    options.mode = NtpOutputMode::CsluAutomatic;
    options.customFirst = true;
    options.useOgiLexicon = true;
    options.useOgiAddenda = true;
    options.failOnUnknown = true;
    return options;
}

struct TextToNtpResolver::Impl {
    explicit Impl(TextToNtpOptions opts)
        : options(std::move(opts)), cmu(options.cmuDictionaryDir),
          ogi(options.ogiLexiconPath, options.ogiAddendaPath, options.useOgiLexicon, options.useOgiAddenda) {
        requireDictionaryAssets(options);
        for (const auto& p : options.overrideDictionaries) {
            overrideMaps.push_back(readCustomDictionary(p, sourceNameForPath("override", p)));
        }
        builtinCustom = readCustomDictionary(options.customDictionary, sourceNameForPath("custom", options.customDictionary));
    }

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
            if (lookupPronunciation(word, overrideMaps, builtinCustom, cmu, ogi, options, entry)) {
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

        if (options.mode == NtpOutputMode::CsluAutomatic) {
            r.ntp = csluAutomaticFormat(
                wordPron,
                options.includeInitialPause,
                options.includeFinalPause);
        } else {
            r.ntp = simpleLinearFormat(wordPron, options.includeInitialPause, options.includeFinalPause);
        }
        return r;
    }

    TextToNtpOptions options;
    std::vector<std::map<std::string, PronEntry>> overrideMaps;
    std::map<std::string, PronEntry> builtinCustom;
    CmuObDictionary cmu;
    OgiDictionary ogi;
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
