#include "engine.hpp"
#include "wav.hpp"
#include "fingerprint.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs = std::filesystem;
namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F f, const std::string& message) {
    bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
std::string contents(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read test file " + path.u8string());
    return {std::istreambuf_iterator<char>(in), {}};
}
void put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary);
    out << text; out.close(); require(!out.fail(), "Cannot write test file");
}
void audio(const fs::path& path) {
    fs::create_directories(path.parent_path());
    cslu::DecodedWaveform wave; wave.sampleRate = 16000; wave.mono.resize(16000);
    for (std::size_t i = 0; i < wave.mono.size(); ++i) {
        const double t = static_cast<double>(i) / wave.sampleRate;
        wave.mono[i] = (t < .1 || t > .85) ? 0.f : static_cast<float>(.2 * std::sin(2.0 * 3.141592653589793 * 170 * t));
    }
    cslu::writePcm16WaveformSegment(path, wave, 0, 2);
}
std::set<std::string> files(const fs::path& root) {
    std::set<std::string> names;
    if (!fs::exists(root)) return names;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file()) names.insert(entry.path().lexically_relative(root).generic_u8string());
    return names;
}
void noTemporary(const fs::path& root) {
    for (const auto& entry : fs::recursive_directory_iterator(root))
        require(entry.path().filename().u8string().rfind(".neolip-tmp-", 0) != 0, "Temporary output was not cleaned");
}
struct Scratch {
    fs::path root;
    Scratch() {
        root = fs::temp_directory_path() / fs::u8path("NeoLIP regression spaces \xc3\xa9-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
    }
    ~Scratch() { std::error_code ec; fs::remove_all(root, ec); }
};
void test(const fs::path& source) {
    Scratch scratch; const auto& root = scratch.root;
    const neolip::Assets assets{source / "third_party/cslu_runtime/Toolkit/2.0/apps/fa", source / "dictionaries", root / "user data/pronunciations.dict"};
    neolip::requireAssets(assets);
    { auto broken = assets; broken.models = root / "missing";
      rejects([&]() { neolip::Engine engine(broken); }, "Incomplete install accepted"); }
    // Match the actual native package and build-tree executable layouts.
    const auto package = root / "portable app";
    fs::create_directories(package);
    fs::copy(assets.models, package / "models", fs::copy_options::recursive);
    fs::copy(assets.dictionaries, package / "dictionaries", fs::copy_options::recursive);
    for (const auto& relative : {"NeoLIP", "bin/NeoLIP", "libexec/neolip/NeoLIP"}) {
        put(package / relative, "executable fixture");
        const auto found = neolip::discoverAssets(package / relative, root / "managed user");
        require(found.models == fs::weakly_canonical(package / "models"), "Packaged model discovery failed");
        require(found.dictionaries == fs::weakly_canonical(package / "dictionaries"), "Packaged dictionary discovery failed");
    }
    const auto app = root / "macOS layout/NeoLIP.app/Contents";
    fs::create_directories(app / "Resources");
    fs::copy(assets.models, app / "Resources/models", fs::copy_options::recursive);
    fs::copy(assets.dictionaries, app / "Resources/dictionaries", fs::copy_options::recursive);
    put(app / "MacOS/NeoLIP", "executable fixture");
    const auto macAssets = neolip::discoverAssets(app / "MacOS/NeoLIP", root / "managed user");
    require(macAssets.models == fs::weakly_canonical(app / "Resources/models"), "macOS bundle resource discovery failed");
    fs::remove(app / "Resources/dictionaries/cmu/w.ob");
    rejects([&]() { neolip::requireAssets(macAssets); }, "Damaged packaged dictionary was accepted");
    rejects([&]() { (void)neolip::discoverAssets(app / "MacOS/NeoLIP", root / "managed user"); }, "Incomplete bundle was silently selected");
    neolip::Engine engine(assets);
    const auto spoken = engine.pronounce("Why hello there welcome to the domain of Bogga the Hutt how may one serve you today");
    require(spoken.unknownWords.empty() && spoken.words.size() == 17, "Bundled dictionary layering changed");
    require(spoken.ntp.find("[.pau]") != std::string::npos, "Automatic pause graph is not used");
    for (const auto& example : std::vector<std::pair<std::string, std::vector<std::string>>>{
        {"I am a Jedi.", {"i", "am", "a", "jedi"}},
        {"I'm ready.", {"i'm", "ready"}},
        {"I can't go.", {"i", "can't", "go"}},
        {"I'd already left.", {"i'd", "already", "left"}},
        {"I\xe2\x80\x99m ready.", {"i'm", "ready"}}}) {
        const auto pronunciation = engine.pronounce(example.first);
        require(pronunciation.words == example.second, "Transcript silently rewritten: " + example.first);
        require(pronunciation.unknownWords.empty(), "Bundled contracted pronunciation was not used: " + example.first);
    }

    const auto oldCwd = fs::current_path();
    put(root / "hello", "thisisnotaknownwordzz"); fs::current_path(root);
    try { require(engine.pronounce("hello").unknownWords.empty(), "Literal transcript was treated as a filename"); }
    catch (...) { fs::current_path(oldCwd); throw; }
    fs::current_path(oldCwd);
    const auto natural = neolip::qualityFor(neolip::MouthMovement::Natural);
    const auto minimal = neolip::qualityFor(neolip::MouthMovement::MinimalSmoothing);
    require(natural.smoothing == cslu::LipSmoothingPreset::Light && natural.maxBlendMs == 40, "Natural preset changed");
    require(minimal.smoothing == cslu::LipSmoothingPreset::None && minimal.maxBlendMs == 10, "Minimal smoothing blend window changed");
    require(natural.anticipateClosuresMs == minimal.anticipateClosuresMs && natural.closeOnPauseMs == minimal.closeOnPauseMs,
        "Mouth preset changed pause/anticipation policy");
    const auto wav = root / "audio folder/hello.wav"; audio(wav);
    put(root / "audio folder/hello.txt", "hello"); put(root / "audio folder/hello.lab", "there");
    require(neolip::matchingTranscript(wav).extension() == ".txt", "TXT/LAB priority changed");
    require(neolip::defaultLipPath(wav) == root / "audio folder/hello.lip", "Default output path is incorrect");
    require(neolip::matchingAudio(root / "audio folder/hello.lip") == wav, "Editor did not match WAV");
    put(root / "utf8.txt", "\xef\xbb\xbfhello");
    require(neolip::readTranscript(root / "utf8.txt") == "hello", "UTF-8 BOM not removed");
    put(root / "utf16.txt", std::string("h\0i\0", 4));
    rejects([&]() { (void)neolip::readTranscript(root / "utf16.txt"); }, "UTF-16 was silently treated as text");

    const auto out = root / "single result/hello.lip";
    neolip::CreateRequest request{wav, "hello there", out};
    std::vector<std::string> stages; neolip::Context context;
    context.progress = [&](const neolip::Progress& progress) { stages.push_back(progress.message); };
    auto result = engine.create(request, context);
    require(result.status == neolip::Status::Created, "Single creation failed: " + result.message);
    require(neolip::validExistingLip(out), "Created LIP is invalid");
    require(files(out.parent_path()) == std::set<std::string>{"hello.lip"}, "Single creation left sidecar files");
    require(std::find(stages.begin(), stages.end(), "Aligning speech") != stages.end(), "Typed engine stages not delivered");
    const auto before = contents(out);
    require(result.prepared && result.audio && !result.reusedAlignment, "Fresh alignment was not retained in RAM");
    const auto initialPrepared = result.prepared;
    auto comparisonRequest = request; comparisonRequest.movement = neolip::MouthMovement::MinimalSmoothing;
    auto comparison = engine.preview(comparisonRequest);
    require(comparison.status == neolip::Status::Previewed && comparison.reusedAlignment && comparison.prepared == initialPrepared,
        "Preset comparison repeated or discarded alignment");
    require(comparison.audio == result.audio && contents(out) == before, "Preset preview copied audio or wrote output");
    require(files(out.parent_path()) == std::set<std::string>{"hello.lip"}, "Preview wrote files");
    comparisonRequest.spokenText = "hello";
    require(engine.preview(comparisonRequest).status == neolip::Status::Failed, "Changed transcript used stale timing");
    comparisonRequest = request; comparisonRequest.audioChannel = 0;
    require(engine.preview(comparisonRequest).status == neolip::Status::Failed, "Changed channel used stale timing");
    const auto wavBytes = contents(wav); const auto wavTime = fs::last_write_time(wav);
    auto changed = wavBytes; changed[44] = static_cast<char>(changed[44] ^ 1); put(wav, changed); fs::last_write_time(wav, wavTime);
    require(engine.preview(request).status == neolip::Status::Failed, "Same-size/same-time WAV mutation reused stale timing");
    put(wav, wavBytes); fs::last_write_time(wav, wavTime);
    require(engine.preview(request).status == neolip::Status::Previewed, "Unchanged content could not reuse cache");
    const auto literalNumbers = engine.pronounce("100 HK-47");
    require(literalNumbers.words == std::vector<std::string>({"100", "hk-47"}), "Ambiguous numerals were silently expanded");
    require(!literalNumbers.unknownWords.empty(), "Unknown numbers require a spoken form");

    require(engine.create(request).status == neolip::Status::Failed && contents(out) == before, "Default creation overwrote an existing LIP");
    request.replaceExisting = true; request.spokenText = "quuxneolipunknownzz quuxneolipunknownzz";
    result = engine.create(request);
    require(result.status == neolip::Status::UnknownWords && result.unknownWords.size() == 1 && contents(out) == before,
        "Unknown words modified an existing output or were not deduplicated");
    request.output = root / "unknown/no.lip";
    require(engine.create(request).status == neolip::Status::UnknownWords && !fs::exists(request.output), "Unknown-word failure wrote output");
    request.spokenText = "hello there"; request.output = out;
    std::atomic<bool> cancelled{true}; neolip::Context cancelContext; cancelContext.work.cancelled = &cancelled;
    require(engine.create(request, cancelContext).status == neolip::Status::Cancelled && contents(out) == before, "Pre-cancelled job changed output");
    cancelled.store(false); std::atomic<int> checks{0};
    cancelContext.work.yield = [&]() { if (++checks > 20) cancelled.store(true); };
    require(engine.create(request, cancelContext).status == neolip::Status::Cancelled && contents(out) == before, "Hot-loop cancellation failed");
    cancelled.store(false); cancelContext.work.yield = {};
    cancelContext.progress = [&](const neolip::Progress& p) { if (p.message == "Saving LIP") cancelled.store(true); };
    require(engine.create(request, cancelContext).status == neolip::Status::Cancelled && contents(out) == before, "Pre-commit cancellation replaced output");
    noTemporary(root);
    request.output = root / "minimal result/hello.lip"; request.movement = neolip::MouthMovement::MinimalSmoothing;
    result = engine.create(request);
    require(result.status == neolip::Status::Created, "Minimal preset creation failed");
    const auto a = cslu::readLip(out), b = cslu::readLip(request.output);
    require(a.durationSeconds == b.durationSeconds, "Preset changed recording duration");
    require(b.frames.size() >= a.frames.size(), "Minimal smoothing produced fewer keys than Natural");

    auto entry = neolip::resolvePronunciation(assets, {"neoliptestword", "hello there", {}});
    require(!entry.phones.empty(), "Sounds-like guide did not resolve");
    neolip::saveUserPronunciation(assets, entry);
    { neolip::Engine fresh(assets); require(fresh.pronounce("neoliptestword").unknownWords.empty(), "User pronunciation not loaded"); }
    auto saved = neolip::readUserPronunciations(assets.userDictionary);
    require(saved.size() == 1 && saved[0].soundsLike == "hello there", "Sounds-like metadata did not survive save");
    const auto dictBefore = contents(assets.userDictionary);
    rejects([&]() { neolip::saveUserPronunciation(assets, {"newword", "unknownnonsenszz", {}}); }, "Unknown sounds-like guide accepted");
    require(contents(assets.userDictionary) == dictBefore, "Invalid guide changed dictionary");
    rejects([&]() { neolip::saveUserPronunciation(assets, {"newword", {}, "invalidphonexxx"}); }, "Invalid CSLU phone accepted");
    rejects([&]() { neolip::importUserPronunciations(assets, {{"newword", "hello", {}}, {"badword", {}, "invalidphonexxx"}}); }, "Invalid bulk import accepted");
    require(contents(assets.userDictionary) == dictBefore, "Bulk import was not atomic");
    neolip::importUserPronunciations(assets, {{"neoliptestword", "there", {}}}, false);
    require(contents(assets.userDictionary) == dictBefore, "Migration overwrote a newer personal entry");
    neolip::saveUserPronunciation(assets, {"hello", "there", {}});
    { neolip::Engine fresh(assets); require(fresh.pronounce("hello").wordInfo[0].pronunciation == fresh.pronounce("there").wordInfo[0].pronunciation,
        "User dictionary did not override bundled word"); }
    neolip::removeUserPronunciation(assets, "hello");
    neolip::removeUserPronunciation(assets, "neoliptestword");
    require(neolip::readUserPronunciations(assets.userDictionary).empty(), "Removing user entry failed");

    // Changing content, not just file metadata, invalidates loaded models and pronunciations.
    const neolip::Assets isolatedAssets{package / "models", package / "dictionaries", root / "isolated-user.dict"};
    neolip::Engine isolated(isolatedAssets);
    auto isolatedRequest = request; isolatedRequest.output = root / "isolated result.lip"; isolatedRequest.replaceExisting = true;
    require(isolated.create(isolatedRequest).status == neolip::Status::Created, "Isolated cache baseline failed");
    const auto dictionaryFile = package / "dictionaries/kotor_override.dict";
    const auto dictionaryBytes = contents(dictionaryFile); const auto dictionaryTime = fs::last_write_time(dictionaryFile);
    auto dictionaryChanged = dictionaryBytes; dictionaryChanged[0] = dictionaryChanged[0] == '#' ? ';' : '#';
    put(dictionaryFile, dictionaryChanged); fs::last_write_time(dictionaryFile, dictionaryTime);
    require(isolated.preview(isolatedRequest).status == neolip::Status::Failed, "Dictionary content change did not invalidate cache");
    put(dictionaryFile, dictionaryBytes); fs::last_write_time(dictionaryFile, dictionaryTime);
    require(isolated.create(isolatedRequest).status == neolip::Status::Created, "Dictionary restoration failed");
    const auto modelFile = package / "models/bc_16k/manner_nnet.45";
    const auto modelBytes = contents(modelFile); const auto modelTime = fs::last_write_time(modelFile);
    auto modelChanged = modelBytes; modelChanged.back() = static_cast<char>(modelChanged.back() ^ 1);
    put(modelFile, modelChanged); fs::last_write_time(modelFile, modelTime);
    require(isolated.preview(isolatedRequest).status == neolip::Status::Failed, "Model content change did not invalidate cache");
    put(modelFile, modelBytes); fs::last_write_time(modelFile, modelTime);
    require(isolated.create(isolatedRequest).status == neolip::Status::Created, "Model restoration failed");
    put(isolatedAssets.userDictionary, "# user dictionary changed\n");
    require(isolated.preview(isolatedRequest).status == neolip::Status::Failed, "Personal dictionary change did not invalidate cache");

    const auto input = root / "batch input", output = root / "batch output";
    audio(input / "a.wav"); put(input / "a.txt", "hello");
    audio(input / "nested/b.WAV"); put(input / "nested/b.LAB", "hello there");
    audio(input / "missing.wav");
    require(neolip::planBatch(input, output, false).pairs.size() == 1, "Nonrecursive batch traversed a subfolder");
    const auto plan = neolip::planBatch(input, output, true);
    require(plan.pairs.size() == 2 && plan.missingTranscripts.size() == 1 && plan.problems.empty(), "Batch matching/progress denominator incorrect");
    std::size_t callbacks = 0; // The engine serializes item callbacks.
    auto batch = engine.batch(plan, neolip::MouthMovement::Natural, false, {}, [&](const neolip::Result&) { ++callbacks; });
    require(batch.created == 2 && batch.failed == 0 && callbacks == 2, "Automatic native batch failed");
    require(files(output) == std::set<std::string>{"a.lip", "nested/b.lip"}, "Batch retained non-LIP outputs");
    const auto batchBefore = contents(output / "a.lip");
    batch = engine.batch(plan, neolip::MouthMovement::Natural, false);
    require(batch.skipped == 2 && batch.created == 0 && contents(output / "a.lip") == batchBefore, "Skip-existing policy failed");
    put(output / "a.lip", "damaged existing file");
    batch = engine.batch(plan, neolip::MouthMovement::Natural, false);
    require(batch.failed == 1 && batch.skipped == 1 && contents(output / "a.lip") == "damaged existing file", "Invalid existing output was silently replaced");
    batch = engine.batch(plan, neolip::MouthMovement::Natural, true);
    require(batch.created == 2 && batch.failed == 0, "Explicit batch replace failed");
    cancelled.store(true); cancelContext.progress = {};
    batch = engine.batch(plan, neolip::MouthMovement::Natural, true, cancelContext);
    require(batch.cancelled && batch.created == 0, "Cancelled batch started jobs");
    rejects([&]() { engine.batch(plan, neolip::MouthMovement::Natural, false, {}, [](const neolip::Result&) {
        throw std::runtime_error("deliberate callback failure"); }); }, "Callback exception was not propagated safely");
#ifndef _WIN32
    const auto collision = root / "case collision";
    audio(collision / "same.wav"); audio(collision / "SAME.wav"); put(collision / "same.txt", "hello");
    if (!fs::equivalent(collision / "same.wav", collision / "SAME.wav"))
        require(!neolip::planBatch(collision, root / "colliding output", false).problems.empty(), "Case-folded output collision accepted");
    const auto linked = root / "linked input"; audio(linked / "link.wav");
    fs::create_symlink(input / "a.txt", linked / "link.txt");
    require(!neolip::planBatch(linked, root / "linked output", true).problems.empty(), "Transcript symlink escape accepted");
#endif
    put(root / "scan inputs/a.txt", "quuxscanwordzz quuxscanwordzz hello");
    const auto scan = neolip::scanUnknownWords(assets, root / "scan inputs", true);
    require(scan.files == 1 && scan.words.size() == 1 && scan.words[0].occurrences == 2, "In-memory dictionary scan is incorrect");
    require(files(root / "scan inputs") == std::set<std::string>{"a.txt"}, "Dictionary scan wrote unsolicited reports");
    neolip::exportUnknownWords(root / "requested report.tsv", scan);
    require(contents(root / "requested report.tsv").find("quuxscanwordzz\t2") != std::string::npos, "Explicit report export failed");
    neolip::saveEditedLip(root / "editor/saved.lip", cslu::readKotorKeyframes(out), a.durationSeconds, false);
    require(neolip::validExistingLip(root / "editor/saved.lip"), "Atomic editor save failed");
    rejects([&]() { neolip::saveEditedLip(root / "editor/saved.lip", {}, 1.0, false); }, "Editor silently replaced output");
    noTemporary(root);
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "Expected repository path"); test(fs::u8path(argv[1]));
        std::cout << "PASS: engine, presets, literal text, pairing, atomic output, cancellation, dictionaries and LIP-only regression tests\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
