#pragma once

#include <filesystem>
#include "transcript.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cslu {

struct TextToNtpOptions {
    std::filesystem::path dictionary;
    std::filesystem::path userDictionary;
    bool includeInitialPause = true;
    bool includeFinalPause = true;
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

TextToNtpResult textToNtpFromSource(const std::string& textOrFile, const TextToNtpOptions& options);
void writeNtpFile(const std::filesystem::path& path, const std::string& ntp);
void writeTextToNtpSourceReport(const std::filesystem::path& path, const TextToNtpResult& result);
void writeTextToNtpUnknownReport(const std::filesystem::path& path, const TextToNtpResult& result);

} // namespace cslu
