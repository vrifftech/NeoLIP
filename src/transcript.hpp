#pragma once
#include <cstddef>
#include <string>
#include <vector>
namespace cslu {
struct TranscriptToken {
    std::string word; // Normalized lookup form; visible text is never rewritten.
    std::size_t beginByte = 0, endByte = 0;
};
// Strict UTF-8, or BOM-marked UTF-16 LE/BE. No guessed legacy encoding.
std::string decodeTranscriptBytes(const std::string& bytes);
std::vector<TranscriptToken> tokenizeTranscript(const std::string& utf8,
    bool cleanupMarkup = true, bool stripBraceDirections = true);
}
