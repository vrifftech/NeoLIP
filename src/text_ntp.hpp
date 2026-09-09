#pragma once

#include <filesystem>
#include "transcript.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cslu {

enum class NtpOutputMode {
    SimpleLinear,
    CsluAutomatic
};

struct TextToNtpOptions {
    std::filesystem::path faRoot;
    std::vector<std::filesystem::path> overrideDictionaries;
    std::filesystem::path customDictionary;
    std::filesystem::path cmuDictionaryDir;
    std::filesystem::path ogiLexiconPath;
    std::filesystem::path ogiAddendaPath;
    NtpOutputMode mode = NtpOutputMode::CsluAutomatic;
    bool customFirst = true;
    bool includeInitialPause = true;
    bool includeFinalPause = true;
    bool useOgiLexicon = true;
    bool useOgiAddenda = true;
    bool failOnUnknown = true;
    bool useCompositionalPronunciations = true;
    bool useInflectionPronunciations = true;
    bool cleanupTlkMarkup = true;
    bool stripBraceStageDirections = true;
};

struct WordPronunciationInfo {
    std::size_t index = 0;
    std::string word;
    std::string pronunciation;
    std::string source;
};

struct TextToNtpResult {
    std::string originalText;
    std::string normalizedText;
    std::vector<TranscriptToken> tokens; // UTF-8 byte spans in originalText.
    std::vector<std::string> words;
    std::vector<std::string> unknownWords;
    std::vector<std::string> notes;
    std::vector<WordPronunciationInfo> wordInfo;
    std::string ntp;
};


class TextToNtpResolver {
public:
    explicit TextToNtpResolver(TextToNtpOptions options);
    ~TextToNtpResolver();
    TextToNtpResolver(TextToNtpResolver&&) noexcept;
    TextToNtpResolver& operator=(TextToNtpResolver&&) noexcept;
    TextToNtpResolver(const TextToNtpResolver&) = delete;
    TextToNtpResolver& operator=(const TextToNtpResolver&) = delete;

    TextToNtpResult resolveSource(const std::string& textOrFile);
    // Literal text: never interpret edited dialogue as a filesystem path.
    TextToNtpResult resolveText(const std::string& text);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

TextToNtpOptions makeDefaultTextToNtpOptions(const std::filesystem::path& faRoot);
TextToNtpResult textToNtpFromSource(const std::string& textOrFile, const TextToNtpOptions& options);
void writeNtpFile(const std::filesystem::path& path, const std::string& ntp);
void writeTextToNtpSourceReport(const std::filesystem::path& path, const TextToNtpResult& result);
void writeTextToNtpUnknownReport(const std::filesystem::path& path, const TextToNtpResult& result);

} // namespace cslu
