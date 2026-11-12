#pragma once
#include "lip.hpp"
#include "audio.hpp"
#include "text_ntp.hpp"
#include "work_control.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cslu { struct TraceGraph; struct ViterbiAlignment; }

namespace neolip {
namespace fs = std::filesystem;
enum class MouthMovement { Natural, MinimalSmoothing };
cslu::LipQualityOptions qualityFor(MouthMovement movement);

struct Assets {
    fs::path models;
    fs::path dictionaries;
    fs::path userDictionary; // Writable application-data path, not an install asset.
};
Assets discoverAssets(const fs::path& executable, const fs::path& userDataDirectory);

struct Progress {
    std::string message;
    std::size_t completed = 0;
    std::size_t total = 0; // Zero means indeterminate, not an invented percentage.
};
struct Context {
    cslu::WorkControl work;
    std::function<void(const Progress&)> progress;
};
enum class Status { Previewed, ChannelChoice, Created, Skipped, UnknownWords, Cancelled, Failed };
struct AlignedWord {
    std::string text, lookup;
    std::size_t beginByte = 0, endByte = 0;
    double startSeconds = 0, endSeconds = 0;
};
std::vector<AlignedWord> alignedWords(const cslu::TextToNtpResult& text,
    const cslu::TraceGraph& graph, const cslu::ViterbiAlignment& alignment);

struct PreparedAlignment {
    std::string fingerprint;
    cslu::PhnFile phonemes;
    std::string transcript;
    std::vector<AlignedWord> words;
    std::shared_ptr<const AudioBuffer> audio;
    std::vector<ReviewWarning> warnings;
};
struct Result {
    Status status = Status::Failed;
    fs::path input;
    fs::path output;
    double durationSeconds = 0;
    std::size_t mouthKeys = 0;
    std::vector<std::string> unknownWords;
    std::string message;
    std::vector<ReviewWarning> warnings;
    std::shared_ptr<const PreparedAlignment> prepared;
    std::shared_ptr<const AudioBuffer> audio;
    std::shared_ptr<const cslu::LipFile> previewLip;
    bool reusedAlignment = false;
    int audioChannel = -1;
    std::string transcript;
    std::string sourceLabel; // Dialogue node or another in-memory batch source.
    std::vector<AlignedWord> words; // Small metadata retained for in-session batch review.
};
struct CreateRequest {
    fs::path audio;
    std::string spokenText; // Always the visible, edited transcript, never a filename.
    fs::path output;
    MouthMovement movement = MouthMovement::Natural;
    bool replaceExisting = false;
    int audioChannel = -1;
};

fs::path defaultLipPath(const fs::path& audio);
fs::path matchingTranscript(const fs::path& audio);
fs::path matchingAudio(const fs::path& lip);
std::string readTranscript(const fs::path& file);
bool validExistingLip(const fs::path& file);

struct BatchItem {
    fs::path audio, transcript, output;
    int audioChannel = -1;
    std::optional<std::string> spokenText = std::nullopt; // Inline batch transcript; no temporary TXT.
    std::string sourceLabel{};
};
struct SourceSnapshot { fs::path path; std::string fingerprint; };
struct BatchPlan {
    fs::path inputRoot, outputRoot;
    fs::path dialogueSource;
    std::vector<SourceSnapshot> sourceSnapshots;
    std::vector<BatchItem> pairs;
    std::vector<fs::path> missingTranscripts;
    std::vector<std::string> problems;
    std::vector<BatchItem> conflicts; // Quarantined, never passed to workers.
};
BatchPlan planBatch(const fs::path& input, const fs::path& output,
                    bool recursive, const Context& context = {});
BatchPlan planCsvBatch(const fs::path& csv, const fs::path& input,
                       const fs::path& output, const Context& context = {});
struct BatchResult {
    std::vector<Result> items;
    std::size_t created = 0, skipped = 0, failed = 0;
    bool cancelled = false;
};

// No process execution, global streams, command-line parsing or trace writers.
// Each instance is confined to one worker. Batch workers own separate instances.
class Engine final {
public:
    explicit Engine(Assets assets);
    ~Engine();
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Result create(const CreateRequest& request, const Context& context = {});
    Result preview(const CreateRequest& request, const Context& context = {});
    void clearCache();
    cslu::TextToNtpResult pronounce(const std::string& literalText,
                                   const Context& context = {});
    BatchResult batch(const BatchPlan& plan, MouthMovement movement,
                      bool replaceExisting, const Context& context = {},
                      std::function<void(const Result&)> onItem = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct UserPronunciation {
    std::string word;
    std::string soundsLike;
    std::string phones;
};
std::vector<UserPronunciation> readUserPronunciations(const fs::path& dictionary);
// Empty phones means resolve soundsLike using familiar dictionary words.
// A supplied phone expression is verified by the same graph builder as alignment.
UserPronunciation resolvePronunciation(const Assets& assets, UserPronunciation entry);
void saveUserPronunciation(const Assets& assets, const UserPronunciation& entry);
// Validate the entire import, then replace the owned dictionary atomically.
// replaceExisting=false is used for safe migration from an older release.
void importUserPronunciations(const Assets& assets,
                              const std::vector<UserPronunciation>& entries,
                              bool replaceExisting = true);
void removeUserPronunciation(const Assets& assets, const std::string& word);
struct UnknownWord { std::string word; std::size_t occurrences = 0; };
struct ScanResult {
    std::vector<UnknownWord> words;
    std::vector<std::string> errors;
    std::size_t files = 0;
};
ScanResult scanUnknownWords(const Assets& assets, const fs::path& folder,
                           bool recursive, const Context& context = {});
void exportUnknownWords(const fs::path& output, const ScanResult& result);
// Editor saves use the same validated, atomic replacement path as Create.
void saveEditedLip(const fs::path& output,
                   const std::vector<cslu::KotorKeyframe>& frames,
                   double durationSeconds, bool replaceExisting);
} // namespace neolip
