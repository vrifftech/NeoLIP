#include "engine.hpp"
#include <neoshared/PathUtf8.hpp>
#include "fingerprint.hpp"
#include "review.hpp"
#include "graph_build.hpp"
#include "nnet45.hpp"
#include "raw_frontend.hpp"
#include "wav.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>
#ifndef __EMSCRIPTEN__
#include <thread>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif

namespace neolip {
namespace {
std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \r\n\t");
    if (begin == std::string::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \r\n\t") - begin + 1);
}
bool file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}
std::string displayPath(const fs::path& path) { return neoshared::pathToUtf8(path); }
void report(const Context& context, const std::string& message,
            std::size_t completed = 0, std::size_t total = 0) {
    context.work.check();
    if (context.progress) context.progress({message, completed, total});
}
fs::path normalized(const fs::path& path) {
    std::error_code ec;
    auto result = fs::weakly_canonical(path, ec);
    if (!ec) return result;
    return fs::absolute(path).lexically_normal();
}
bool contained(const fs::path& root, const fs::path& child) {
    auto rel = normalized(child).lexically_relative(normalized(root));
    if (rel.empty() || rel.is_absolute()) return false;
    for (const auto& part : rel) if (part == "..") return false;
    return true;
}

// The temporary directory is reserved atomically on the destination volume.
// Its destructor removes it on errors and cancellation. Existing output is not
// removed first: readers see the old complete file or the new complete file.
class AtomicOutput final {
    fs::path directory_, output_;
public:
    fs::path temporary;
    explicit AtomicOutput(const fs::path& output) : output_(fs::absolute(output)) {
        if (output.empty() || output.filename().empty()) throw std::runtime_error("Choose an output filename.");
        fs::create_directories(output_.parent_path());
        std::error_code ec;
        if (fs::is_symlink(fs::symlink_status(output_, ec)))
            throw std::runtime_error("The output is a symbolic link. Choose a regular file instead.");
        static std::atomic<unsigned long long> counter{0};
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        bool reserved = false;
        for (int attempt = 0; attempt < 32; ++attempt) {
            directory_ = output_.parent_path() /
                (".neolip-tmp-" + std::to_string(ticks) + "-" + std::to_string(counter++));
            ec.clear();
            if (fs::create_directory(directory_, ec)) { reserved = true; break; }
            if (ec && ec != std::errc::file_exists) throw fs::filesystem_error("Cannot prepare output", directory_, ec);
        }
        if (!reserved) throw std::runtime_error("Cannot reserve a temporary output file.");
        temporary = directory_ / "output";
    }
    ~AtomicOutput() { std::error_code ec; fs::remove_all(directory_, ec); }
    AtomicOutput(const AtomicOutput&) = delete;
    AtomicOutput& operator=(const AtomicOutput&) = delete;
    void commit(bool replace) {
#ifdef _WIN32
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(temporary.c_str(), output_.c_str(), flags))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot save output");
#elif defined(__linux__) && !defined(__EMSCRIPTEN__) && defined(SYS_renameat2)
        // RENAME_NOREPLACE is 1; unlike exists()+rename(), this cannot clobber a
        // file another application creates while alignment is running.
        if (syscall(SYS_renameat2, -100, temporary.c_str(), -100, output_.c_str(), replace ? 0 : 1) != 0) {
            if (errno != ENOSYS) throw std::system_error(errno, std::generic_category(), "Cannot save output");
            if (replace) fs::rename(temporary, output_);
            else { fs::create_hard_link(temporary, output_); fs::remove(temporary); }
        }
#elif defined(__APPLE__)
        if (renamex_np(temporary.c_str(), output_.c_str(), replace ? 0 : RENAME_EXCL) != 0)
            throw std::system_error(errno, std::generic_category(), "Cannot save output");
#elif defined(__EMSCRIPTEN__)
        // MEMFS has one thread and no external filesystem writers. No yield is
        // permitted between this test and rename.
        if (!replace && fs::exists(output_)) throw std::runtime_error("Output already exists; it was not replaced.");
        fs::rename(temporary, output_);
#else
        if (replace) fs::rename(temporary, output_);
        else { fs::create_hard_link(temporary, output_); fs::remove(temporary); }
#endif
    }
};

const std::array<const char*, 6> networks = {{
    "bc_16k/manner_nnet.45", "bc_16k/place_nnet.45", "bc_16k/height_nnet.45",
    "trans_16k/manner_nnet.45", "trans_16k/place_nnet.45", "trans_16k/height_nnet.45"
}};
std::vector<fs::path> modelFiles(const fs::path& root) {
    std::vector<fs::path> paths = {root / "phonemes_new_16k.parts", root / "phonemes_new_16k.train.spec"};
    for (const char* kind : {"manner", "place", "height"}) {
        for (const char* suffix : {"_nnet.45", ".train.spec", ".map", ".parts"})
            paths.push_back(root / "bc_16k" / (std::string(kind) + suffix));
        for (const char* suffix : {"_nnet.45", ".train.spec"})
            paths.push_back(root / "trans_16k" / (std::string(kind) + suffix));
    }
    return paths;
}
std::vector<fs::path> dictionaryFiles(const fs::path& root) {
    return {root / "lexicon.tsv"};
}
struct AssetTreeFingerprints {
    std::string models;
    std::string dictionaries;
};

AssetTreeFingerprints cachedAssetTreeFingerprints(const Assets& assets) {
    static std::mutex mutex;
    static std::map<std::string, AssetTreeFingerprints> cache;
    const std::string key = displayPath(normalized(assets.models)) + "\n" +
                            displayPath(normalized(assets.dictionaries));
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = cache.find(key);
    if (found != cache.end()) return found->second;

    AssetTreeFingerprints value{
        treeFingerprint(assets.models),
        treeFingerprint(assets.dictionaries)
    };
    cache.emplace(key, value);
    return value;
}
bool complete(const fs::path& root, bool models) {
    const auto paths = models ? modelFiles(root) : dictionaryFiles(root);
    return std::all_of(paths.begin(), paths.end(), [](const fs::path& path) {
        std::error_code ec;
        return file(path) && fs::file_size(path, ec) > 0 && !ec;
    });
}
cslu::TextToNtpOptions textOptions(const Assets& assets) {
    cslu::TextToNtpOptions opt;
    opt.dictionary = assets.dictionaries / "lexicon.tsv";
    opt.userDictionary = assets.userDictionary;
    opt.failOnUnknown = false;
    return opt;
}
fs::path sameStem(const fs::path& source, const std::vector<std::string>& extensions) {
    const auto parent = source.has_parent_path() ? source.parent_path() : fs::path(".");
    // Stable priority: .txt before .lab, exact case before a case-insensitive
    // directory match. Never silently select the last directory entry.
    for (const auto& ext : extensions) {
        auto candidate = source; candidate.replace_extension(ext);
        if (file(candidate)) return candidate;
        std::vector<fs::path> candidates;
        for (const auto& entry : fs::directory_iterator(parent)) {
            if (file(entry.path()) && lower(neoshared::pathToUtf8(entry.path().stem())) == lower(neoshared::pathToUtf8(source.stem())) &&
                lower(neoshared::pathToUtf8(entry.path().extension())) == ext) candidates.push_back(entry.path());
        }
        std::sort(candidates.begin(), candidates.end());
        if (candidates.size() > 1) throw std::runtime_error("More than one matching file for " + displayPath(source));
        if (!candidates.empty()) return candidates.front();
    }
    return {};
}
std::vector<std::string> unique(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}
void requireOutput(const CreateRequest& request) {
    if (!file(request.audio)) throw std::runtime_error("The selected audio file does not exist.");
    if (request.output.empty() || lower(neoshared::pathToUtf8(request.output.extension())) != ".lip")
        throw std::runtime_error("Choose an output filename ending in .lip.");
    if (normalized(request.audio) == normalized(request.output)) throw std::runtime_error("Audio and output paths must be different.");
    if (!request.replaceExisting && fs::exists(request.output))
        throw std::runtime_error("A LIP already exists at the destination. It has not been replaced.");
}
cslu::LipFile validateWritten(const fs::path& path) {
    auto lip = cslu::readLip(path);
    const auto validation = cslu::validateLipForKotorRuntime(lip, fs::file_size(path));
    if (!validation.ok) {
        std::string why = "The generated mouth animation could not be saved safely.";
        for (const auto& e : validation.errors) why += "\n" + e;
        throw std::runtime_error(why);
    }
    return lip;
}
} // namespace

cslu::LipQualityOptions qualityFor(MouthMovement movement) {
    cslu::LipQualityOptions quality;
    // Both modes use the same phoneme alignment and preserve pose/hold order.
    // Only the bounded blend window differs; neither drops short phonemes.
    if (movement == MouthMovement::MinimalSmoothing) {
        quality.maxBlendMs = 10;
    }
    return quality;
}
Assets discoverAssets(const fs::path& executable, const fs::path& userDataDirectory) {
#ifdef __EMSCRIPTEN__
    (void)executable;
    return {"/models", "/dictionaries", userDataDirectory / "pronunciations.dict"};
#else
    const fs::path base = normalized(executable).parent_path();
    std::vector<fs::path> candidates = {base / "../Resources", base, base / "..",
        base / "../share/NeoLIP", base / "../share/neolip", base / "../.."};
#ifdef NEOLIP_SOURCE_ASSET_ROOT
    // Available only in uninstalled developer builds, never searched first.
    candidates.push_back(neoshared::pathFromUtf8(NEOLIP_SOURCE_ASSET_ROOT));
#endif
    for (const auto& path : candidates) {
        const auto root = normalized(path);
        if (complete(root / "models", true) && complete(root / "dictionaries", false))
            return {root / "models", root / "dictionaries", userDataDirectory / "pronunciations.dict"};
    }
    throw std::runtime_error("This NeoLIP installation is incomplete. Reinstall NeoLIP; its bundled models or pronunciation dictionaries are missing.");
#endif
}
void requireAssets(const Assets& assets) {
    std::vector<fs::path> paths = modelFiles(assets.models);
    const auto dictionaries = dictionaryFiles(assets.dictionaries);
    paths.insert(paths.end(), dictionaries.begin(), dictionaries.end());
    for (const auto& path : paths) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream || stream.peek() == std::char_traits<char>::eof())
            throw std::runtime_error("This NeoLIP installation is incomplete. Reinstall NeoLIP.\nMissing or unreadable asset: " + displayPath(path));
    }
}
fs::path defaultLipPath(const fs::path& audio) { auto path = audio; path.replace_extension(".lip"); return path; }
fs::path matchingTranscript(const fs::path& audio) { return sameStem(audio, {".txt", ".lab"}); }
fs::path matchingAudio(const fs::path& lip) { return sameStem(lip, {".wav", ".mp3"}); }
std::string readTranscript(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read transcript: " + displayPath(path));
    // Bound malformed/unintended imports. A dialogue transcript is not a corpus.
    std::error_code ec;
    if (fs::file_size(path, ec) > 4 * 1024 * 1024 || ec)
        throw std::runtime_error("The transcript is too large (maximum 4 MiB).");
    std::string text((std::istreambuf_iterator<char>(in)), {});
    return cslu::decodeTranscriptBytes(text);
}
bool validExistingLip(const fs::path& path) {
    try { return file(path) && cslu::validateLipForKotorRuntime(cslu::readLip(path), fs::file_size(path)).ok; }
    catch (...) { return false; }
}

std::vector<AlignedWord> alignedWords(const cslu::TextToNtpResult& text,
    const cslu::TraceGraph& graph, const cslu::ViterbiAlignment& alignment) {
    if (alignment.segmentNodes.size()!=alignment.stateSegments.size() ||
        graph.stateWords.size()!=graph.statesInitial.size())
        throw std::runtime_error("Missing word alignment metadata.");
    std::vector<AlignedWord> result(text.tokens.size());
    std::vector<bool> found(text.tokens.size(),false);
    for(std::size_t i=0;i<result.size();++i) {
        const auto& token=text.tokens[i];auto& word=result[i];
        word.lookup=token.word;word.beginByte=token.beginByte;word.endByte=token.endByte;
        word.text=text.originalText.substr(token.beginByte,token.endByte-token.beginByte);
    }
    for(std::size_t i=0;i<alignment.segmentNodes.size();++i) {
        const auto node=alignment.segmentNodes[i];
        if(node<0 || static_cast<std::size_t>(node)>=graph.stateWords.size()) throw std::runtime_error("Invalid aligned node.");
        const auto index=graph.stateWords[static_cast<std::size_t>(node)];
        if(index<0)continue;
        if(static_cast<std::size_t>(index)>=result.size())throw std::runtime_error("Invalid aligned word.");
        auto& word=result[static_cast<std::size_t>(index)];const auto& segment=alignment.stateSegments[i];
        if(!found[static_cast<std::size_t>(index)])word.startSeconds=segment.startMs/1000.0;
        word.endSeconds=segment.endMs/1000.0;found[static_cast<std::size_t>(index)]=true;
    }
    std::vector<AlignedWord> emitted;
    for(std::size_t i=0;i<result.size();++i)if(found[i])emitted.push_back(std::move(result[i]));
    return emitted;
}

struct Engine::Impl {
    Assets assets;
    std::unique_ptr<cslu::TextToNtpResolver> resolver;
    std::vector<cslu::NNet45> models;
    std::string modelFingerprint, dictionaryBaseFingerprint, dictionaryFingerprint;
    std::shared_ptr<const PreparedAlignment> prepared;
    std::string refreshInputs(const CreateRequest& request) {
        const auto user = file(assets.userDictionary) ? fileFingerprint(assets.userDictionary) : sha256("<absent>");
        const auto nextDictionary = sha256(dictionaryBaseFingerprint + user);
        if (dictionaryFingerprint != nextDictionary) { resolver.reset(); prepared.reset(); dictionaryFingerprint = nextDictionary; }
        return sha256(fileFingerprint(request.audio) + modelFingerprint + nextDictionary +
            std::to_string(request.audioChannel) + ":" + request.spokenText);
    }
    void refreshDictionary() {
        const auto user = file(assets.userDictionary) ? fileFingerprint(assets.userDictionary) : sha256("<absent>");
        const auto next = sha256(dictionaryBaseFingerprint + user);
        if (next != dictionaryFingerprint) { resolver.reset(); prepared.reset(); dictionaryFingerprint = next; }
    }
    explicit Impl(Assets value) : assets(std::move(value)) {
        requireAssets(assets);
        const auto fingerprints = cachedAssetTreeFingerprints(assets);
        modelFingerprint = fingerprints.models;
        dictionaryBaseFingerprint = fingerprints.dictionaries;
    }
    cslu::TextToNtpResolver& text() {
        if (!resolver) resolver = std::make_unique<cslu::TextToNtpResolver>(textOptions(assets));
        return *resolver;
    }
    void loadModels() {
        if (models.size() == networks.size()) return;
        models.clear();
        for (const char* network : networks) {
            cslu::checkWork();
            models.push_back(cslu::NNet45::load(assets.models / network));
        }
    }
};
Engine::Engine(Assets assets) : impl_(std::make_unique<Impl>(std::move(assets))) {}
Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;
cslu::TextToNtpResult Engine::pronounce(const std::string& text, const Context& context) {
    cslu::ScopedWorkControl control(context.work);
    context.work.check();
    impl_->refreshDictionary();
    return impl_->text().resolveText(text);
}
void Engine::clearCache() { impl_->prepared.reset(); }
Result Engine::preview(const CreateRequest& request, const Context& context) {
    Result result; result.input=request.audio;result.output=request.output;result.audioChannel=request.audioChannel;
    cslu::ScopedWorkControl control(context.work);
    try {
        report(context,"Checking pronunciations");
        if(trim(request.spokenText).empty())throw std::runtime_error("Enter the words spoken in the recording.");
        const auto key=impl_->refreshInputs(request);
        if(!impl_->prepared || impl_->prepared->fingerprint!=key) {
            impl_->prepared.reset();
            const auto text=impl_->text().resolveText(request.spokenText);
            if(text.words.empty())throw std::runtime_error("The transcript does not contain any spoken words.");
            if(!text.unknownWords.empty()) {
                result.status=Status::UnknownWords;result.unknownWords=unique(text.unknownWords);
                result.message="Add pronunciations, or select unfamiliar names/numbers and use Say as to enter what is spoken.";
                return result;
            }
            report(context,"Reading audio");
            const auto audio=decodeAudio(request.audio,request.audioChannel);result.audio=audio;
            if(audio->channelChoiceRequired) {
                result.status=Status::ChannelChoice;result.warnings=audio->warnings;result.message=audio->warnings.front().message;return result;
            }
            if(audio->rms==0)throw std::runtime_error("The selected recording is silent. Choose the spoken audio instead.");
            cslu::Pcm16Mono wave;wave.sampleRate=audio->waveform.sampleRate;wave.samples.reserve(audio->waveform.mono.size());
            for(const float sample:audio->waveform.mono)wave.samples.push_back(double(sample)*32768.0);
            report(context,"Analysing audio");auto frontend=cslu::computeFullFrontendFromOriginalWav(wave);wave={};
            auto graph=cslu::buildGraphBundleFromWords(text,impl_->assets.models);
            impl_->loadModels();cslu::BackendTraceData data;
            data.featStatesM=graph.mappings.featStatesM;data.featStatesP=graph.mappings.featStatesP;data.featStatesH=graph.mappings.featStatesH;
            data.depList=graph.mappings.depList;data.vcList=graph.mappings.vcList;
            data.transStatesM=graph.mappings.transStatesM;data.transStatesP=graph.mappings.transStatesP;data.transStatesH=graph.mappings.transStatesH;
            std::array<cslu::Matrix*,6> outputs{{&data.bcMOrig,&data.bcPOrig,&data.bcHOrig,&data.transMOrig,&data.transPOrig,&data.transHOrig}};
            for(std::size_t i=0;i<networks.size();++i){report(context,"Aligning speech");*outputs[i]=impl_->models[i].forwardMatrix(frontend.netin);}
            frontend={};report(context,"Aligning speech");
            auto probabilities=cslu::composeBackendProbabilitiesFromOrig(data,20);data={};
            auto alignment=cslu::csluViterbiAlign(graph.graph,probabilities.phnProb,probabilities.transProb);probabilities={};
            auto prepared=std::make_shared<PreparedAlignment>();prepared->fingerprint=key;prepared->audio=audio;
            prepared->warnings=reviewAlignment(*audio,alignment,graph.graph);
            prepared->words=alignedWords(text,graph.graph,alignment);prepared->transcript=request.spokenText;
            prepared->phonemes=std::move(alignment.phn);
            // The operation owns the decoded audio/transcript snapshot. Avoid
            // rehashing immutable assets and the audio after a long alignment
            // solely for concurrent-mutation detection.
            impl_->prepared=std::move(prepared);
        } else result.reusedAlignment=true;
        result.prepared=impl_->prepared;result.audio=impl_->prepared->audio;result.warnings=impl_->prepared->warnings;
        result.words=impl_->prepared->words;result.transcript=impl_->prepared->transcript;
        auto lip=std::make_shared<const cslu::LipFile>(cslu::phnToLip(impl_->prepared->phonemes,{},qualityFor(request.movement)));
        result.previewLip=lip;result.status=Status::Previewed;result.durationSeconds=lip->durationSeconds;result.mouthKeys=lip->frames.size();
        result.message="Generated LIP. Preview or edit it, then Save.";
    } catch(const cslu::OperationCancelled&) {result.status=Status::Cancelled;result.message="Cancelled. No partial LIP was saved.";}
      catch(const std::exception& e) {result.status=Status::Failed;result.message=e.what();}
    return result;
}

Result Engine::create(const CreateRequest& request, const Context& context) {
    Result result;
    result.input = request.audio; result.output = request.output;
    try {
        requireOutput(request);
        result = preview(request, context);
        if (result.status != Status::Previewed) return result;
        report(context, "Saving LIP");
        AtomicOutput output(request.output);
        cslu::writeLip(output.temporary, *result.previewLip);
        validateWritten(output.temporary); context.work.check(); output.commit(request.replaceExisting);
        result.status = Status::Created;
        result.message = "Created " + displayPath(request.output.filename());
    } catch (const cslu::OperationCancelled&) { result.status = Status::Cancelled; result.message = "Cancelled. No partial LIP was saved."; }
      catch (const std::exception& e) { result.status = Status::Failed; result.message = e.what(); }
    return result;
}

BatchPlan planBatch(const fs::path& input, const fs::path& output, bool recursive, const Context& context) {
    cslu::ScopedWorkControl control(context.work);
    BatchPlan plan; plan.inputRoot = normalized(input); plan.outputRoot = normalized(output.empty() ? input : output);
    if (!fs::is_directory(plan.inputRoot)) throw std::runtime_error("Choose a batch input folder.");
    std::vector<fs::path> wavs;
    std::map<std::string, std::vector<fs::path>> transcriptIndex;
    const auto transcriptKey = [](const fs::path& path) {
        return lower(neoshared::pathToUtf8(normalized(path.parent_path()))) + "|" +
               lower(neoshared::pathToUtf8(path.stem()));
    };
    const auto consider = [&](const fs::directory_entry& entry) {
        context.work.check();
        if (entry.is_symlink() || !entry.is_regular_file()) return;
        const auto extension = lower(neoshared::pathToUtf8(entry.path().extension()));
        if (extension == ".wav" || extension == ".mp3") {
            wavs.push_back(entry.path());
        } else if (extension == ".txt" || extension == ".lab") {
            transcriptIndex[transcriptKey(entry.path())].push_back(entry.path());
        }
    };
    report(context, "Finding WAV/transcript pairs");
    if (recursive) for (const auto& entry : fs::recursive_directory_iterator(plan.inputRoot)) consider(entry);
    else for (const auto& entry : fs::directory_iterator(plan.inputRoot)) consider(entry);
    std::sort(wavs.begin(), wavs.end());
    std::map<std::string, std::vector<BatchItem>> destinations;
    for (const auto& audio : wavs) {
        context.work.check();
        try {
            fs::path transcript;
            for (const auto& ext : {std::string(".txt"), std::string(".lab")}) {
                auto candidate = audio;
                candidate.replace_extension(ext);
                if (file(candidate)) {
                    transcript = candidate;
                    break;
                }
            }
            if (transcript.empty()) {
                const auto found = transcriptIndex.find(transcriptKey(audio));
                if (found != transcriptIndex.end()) {
                    auto candidates = found->second;
                    std::stable_sort(candidates.begin(), candidates.end(),
                        [](const fs::path& left, const fs::path& right) {
                            const auto leftExt = lower(neoshared::pathToUtf8(left.extension()));
                            const auto rightExt = lower(neoshared::pathToUtf8(right.extension()));
                            if (leftExt != rightExt) return leftExt == ".txt";
                            return left < right;
                        });
                    if (candidates.size() > 2 ||
                        (candidates.size() == 2 &&
                         lower(neoshared::pathToUtf8(candidates[0].extension())) ==
                         lower(neoshared::pathToUtf8(candidates[1].extension())))) {
                        throw std::runtime_error("More than one matching file for " + displayPath(audio));
                    }
                    if (!candidates.empty()) transcript = candidates.front();
                }
            }
            if (transcript.empty()) { plan.missingTranscripts.push_back(audio); continue; }
            const auto rel = audio.lexically_relative(plan.inputRoot);
            auto destination = plan.outputRoot / rel;
            destination.replace_extension(".lip");
            if (!contained(plan.inputRoot, transcript) || !contained(plan.outputRoot, destination))
                throw std::runtime_error("A linked path leaves the selected folder: " + displayPath(audio));
            // Quarantine every contender. Never silently let the first one win.
            destinations[lower(neoshared::pathToUtf8(destination))].push_back({audio, transcript, destination});
        } catch (const std::exception& error) { plan.problems.push_back(error.what()); }
    }
    for (auto& entry : destinations) {
        auto& items=entry.second;
        if(items.size()==1) plan.pairs.push_back(std::move(items.front()));
        else {
            plan.problems.push_back("Conflicting inputs excluded: " + displayPath(items.front().output));
            plan.conflicts.insert(plan.conflicts.end(),items.begin(),items.end());
        }
    }
    std::sort(plan.pairs.begin(),plan.pairs.end(),[](const auto& a,const auto& b){return a.audio<b.audio;});
    return plan;
}
BatchResult Engine::batch(const BatchPlan& plan, MouthMovement movement, bool replace,
                         const Context& context, std::function<void(const Result&)> onItem) {
    BatchResult result;
    cslu::ScopedWorkControl batchControl(context.work);
    for (const auto& source : plan.sourceSnapshots) {
        context.work.check();
        if (fileFingerprint(source.path) != source.fingerprint)
            throw std::runtime_error("Dialogue or TLK changed after scanning. Scan again before creating LIPs.");
    }
    // Even callers supplying their own plan cannot race two workers into the same file.
    std::set<std::string> targets;
    for(const auto& pair : plan.pairs)
        if(!targets.insert(lower(neoshared::pathToUtf8(normalized(pair.output)))).second)
            throw std::runtime_error("The batch plan contains conflicting output paths.");
    result.items.resize(plan.pairs.size());
    std::atomic<std::size_t> next{0};
    std::size_t completed = 0;
    std::mutex resultsMutex;
    std::atomic<bool> stopped{false};
    std::exception_ptr workerFailure;
    const auto worker = [&]() {
        Engine engine(impl_->assets);
        Context jobContext; jobContext.work = context.work;
        while (!stopped.load() && !(context.work.cancelled && context.work.cancelled->load())) {
            const auto index = next.fetch_add(1);
            if (index >= plan.pairs.size()) break;
            const auto& item = plan.pairs[index];
            Result itemResult; itemResult.input = item.audio; itemResult.output = item.output; itemResult.audioChannel=item.audioChannel;
            try {
                context.work.check();
                if (!contained(plan.outputRoot, item.output) || (!item.spokenText && !contained(plan.inputRoot, item.transcript)) ||
                    !contained(plan.inputRoot, item.audio))
                    throw std::runtime_error("A linked path changed after scanning the batch.");
                if (!replace && fs::exists(item.output)) {
                    if (!validExistingLip(item.output))
                        throw std::runtime_error("An existing LIP is invalid. Choose Replace existing to regenerate it.");
                    itemResult.status = Status::Skipped; itemResult.message = "Kept existing LIP";
                } else {
                    itemResult = engine.create({item.audio, (item.spokenText ? *item.spokenText : readTranscript(item.transcript)), item.output, movement, replace, item.audioChannel}, jobContext);
                }
            } catch (const cslu::OperationCancelled&) {
                itemResult.status = Status::Cancelled; itemResult.message = "Cancelled";
            } catch (const std::exception& error) { itemResult.message = error.what(); }
            itemResult.sourceLabel = item.sourceLabel;
            if (itemResult.transcript.empty() && item.spokenText) itemResult.transcript = *item.spokenText;
            {
                std::lock_guard<std::mutex> lock(resultsMutex);
                // Batch summaries retain warnings but not every decoded WAV.
                itemResult.prepared.reset(); itemResult.audio.reset(); itemResult.previewLip.reset();
                result.items[index] = itemResult;
                ++completed;
                if (onItem) onItem(itemResult);
                if (context.progress) context.progress({plan.dialogueSource.empty() ? "Processing audio/transcript pairs" : "Creating dialogue LIPs", completed, plan.pairs.size()});
            }
        }
    };
    // std::thread must never allow an exception to escape its entrypoint.
    const auto guardedWorker = [&]() {
        try { worker(); }
        catch (...) {
            stopped.store(true);
            std::lock_guard<std::mutex> lock(resultsMutex);
            if (!workerFailure) workerFailure = std::current_exception();
        }
    };
#ifdef __EMSCRIPTEN__
    guardedWorker();
#else
    // Automatic, deliberately conservative: large acoustic matrices dominate
    // memory usage. No model/frontend state is shared between worker threads.
    const unsigned count = std::min<unsigned>(2, std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> threads;
    try {
        for (std::size_t i = 0; i < std::min<std::size_t>(count, plan.pairs.size()); ++i) threads.emplace_back(guardedWorker);
    } catch (...) {
        stopped.store(true);
        for (auto& thread : threads) thread.join();
        throw;
    }
    for (auto& thread : threads) thread.join();
#endif
    if (workerFailure) std::rethrow_exception(workerFailure);
    for (std::size_t i = 0; i < result.items.size(); ++i) {
        auto& item = result.items[i];
        if (item.input.empty()) {
            item.input = plan.pairs[i].audio; item.output = plan.pairs[i].output; item.audioChannel = plan.pairs[i].audioChannel;
            item.status = Status::Cancelled; item.message = "Not started; batch cancelled";
            item.sourceLabel = plan.pairs[i].sourceLabel;
            if (plan.pairs[i].spokenText) item.transcript = *plan.pairs[i].spokenText;
        }
        if (item.status == Status::Created) ++result.created;
        else if (item.status == Status::Skipped) ++result.skipped;
        else if (item.status == Status::Cancelled) result.cancelled = true;
        else ++result.failed;
    }
    return result;
}

std::vector<UserPronunciation> readUserPronunciations(const fs::path& dictionary) {
    std::vector<UserPronunciation> entries;
    if (!file(dictionary)) return entries;
    std::ifstream input(dictionary);
    if (!input) throw std::runtime_error("Cannot read your pronunciation dictionary.");
    std::string line, sounds;
    while (std::getline(input, line)) {
        cslu::checkWork();
        line = trim(line);
        if (line.rfind("# sounds-like: ", 0) == 0) { sounds = line.substr(15); continue; }
        if (line.empty() || line[0] == '#') continue;
        const auto equals = line.find('='); const auto end = line.find(';');
        if (equals == std::string::npos || end == std::string::npos || end <= equals)
            throw std::runtime_error("Your custom pronunciation dictionary contains a damaged entry.");
        entries.push_back({trim(line.substr(0, equals)), sounds, trim(line.substr(equals + 1, end - equals - 1))});
        sounds.clear();
    }
    return entries;
}
UserPronunciation resolvePronunciation(const Assets& assets, UserPronunciation entry) {
    entry.word = lower(trim(entry.word)); entry.soundsLike = trim(entry.soundsLike); entry.phones = trim(entry.phones);
    if (entry.word.empty() || entry.word.find_first_of(" \t\r\n=;#()[]|") != std::string::npos)
        throw std::runtime_error("Enter one word, without spaces or dictionary punctuation.");
    Engine engine(assets);
    const auto name = engine.pronounce(entry.word);
    if (name.words.size() != 1 || name.words.front() != entry.word)
        throw std::runtime_error("Enter a single word as it appears in the transcript.");
    if (entry.phones.empty()) {
        if (entry.soundsLike.empty()) throw std::runtime_error("Enter familiar words in Sounds like, or a phone sequence in Advanced.");
        const auto result = engine.pronounce(entry.soundsLike);
        if (!result.unknownWords.empty()) {
            std::string message = "Use familiar dictionary words in Sounds like. Unknown:";
            for (const auto& word : unique(result.unknownWords)) message += " " + word;
            throw std::runtime_error(message);
        }
        for (const auto& word : result.wordInfo) {
            if (!entry.phones.empty()) entry.phones += " ";
            entry.phones += word.pronunciation;
        }
    }
    if (entry.phones.empty() || entry.phones.find_first_of("\r\n=;#") != std::string::npos)
        throw std::runtime_error("The pronunciation must be one valid CSLU phone expression.");
    const auto graph = cslu::buildGraphBundleFromNtpText("[.pau] " + entry.phones + " [.pau]", assets.models);
    if (graph.phones.empty()) throw std::runtime_error("The pronunciation does not contain any valid speech sounds.");
    for (char& c : entry.soundsLike) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return entry;
}
namespace {
void writeUserEntries(const fs::path& destination, std::vector<UserPronunciation> entries) {
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.word < b.word; });
    AtomicOutput output(destination);
    {
        std::ofstream out(output.temporary, std::ios::binary);
        out << "# NeoLIP user pronunciations. Bundled CSLU and KOTOR dictionaries are unchanged.\n";
        for (const auto& entry : entries) {
            cslu::checkWork();
            if (!entry.soundsLike.empty()) out << "# sounds-like: " << entry.soundsLike << '\n';
            out << entry.word << " = " << entry.phones << " ;\n";
        }
        out.flush();
        if (!out) throw std::runtime_error("Cannot save your pronunciation dictionary.");
    }
    cslu::checkWork();
    output.commit(true);
}
}
void saveUserPronunciation(const Assets& assets, const UserPronunciation& value) {
    if (assets.userDictionary.empty()) throw std::runtime_error("The application-data directory is unavailable.");
    const auto entry = resolvePronunciation(assets, value);
    auto entries = readUserPronunciations(assets.userDictionary);
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& old) { return lower(old.word) == entry.word; }), entries.end());
    entries.push_back(entry);
    writeUserEntries(assets.userDictionary, std::move(entries));
}
void importUserPronunciations(const Assets& assets, const std::vector<UserPronunciation>& imported, bool replaceExisting) {
    if (assets.userDictionary.empty()) throw std::runtime_error("The application-data directory is unavailable.");
    if (imported.empty()) throw std::runtime_error("This dictionary does not contain any pronunciations.");
    auto entries = readUserPronunciations(assets.userDictionary);
    for (const auto& value : imported) {
        cslu::checkWork();
        const auto entry = resolvePronunciation(assets, value);
        cslu::checkWork();
        const auto old = std::find_if(entries.begin(), entries.end(), [&](const auto& item) { return lower(item.word) == entry.word; });
        if (old == entries.end()) entries.push_back(entry);
        else if (replaceExisting) *old = entry;
    }
    writeUserEntries(assets.userDictionary, std::move(entries));
}
void removeUserPronunciation(const Assets& assets, const std::string& word) {
    auto entries = readUserPronunciations(assets.userDictionary);
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& old) { return lower(old.word) == lower(word); }), entries.end());
    writeUserEntries(assets.userDictionary, std::move(entries));
}
ScanResult scanUnknownWords(const Assets& assets, const fs::path& folder, bool recursive, const Context& context) {
    cslu::ScopedWorkControl control(context.work);
    if (!fs::is_directory(folder)) throw std::runtime_error("Choose a transcript folder.");
    std::vector<fs::path> files;
    const auto add = [&](const fs::directory_entry& entry) {
        context.work.check();
        if (!entry.is_symlink() && entry.is_regular_file()) {
            const auto ext = lower(neoshared::pathToUtf8(entry.path().extension()));
            if (ext == ".txt" || ext == ".lab") files.push_back(entry.path());
        }
    };
    if (recursive) for (const auto& entry : fs::recursive_directory_iterator(folder)) add(entry);
    else for (const auto& entry : fs::directory_iterator(folder)) add(entry);
    std::sort(files.begin(), files.end());
    Engine engine(assets); ScanResult result; std::map<std::string, std::size_t> counts;
    for (const auto& path : files) {
        report(context, "Checking transcript pronunciations", result.files, files.size());
        try { for (const auto& word : engine.pronounce(readTranscript(path), context).unknownWords) ++counts[word]; }
        catch (const cslu::OperationCancelled&) { throw; }
        catch (const std::exception& error) { result.errors.push_back(displayPath(path) + ": " + error.what()); }
        ++result.files;
    }
    for (const auto& item : counts) result.words.push_back({item.first, item.second});
    return result;
}
void exportUnknownWords(const fs::path& path, const ScanResult& result) {
    AtomicOutput output(path);
    {
        std::ofstream out(output.temporary, std::ios::binary);
        out << "word\toccurrences\n";
        for (const auto& word : result.words) out << word.word << '\t' << word.occurrences << '\n';
        out.flush(); if (!out) throw std::runtime_error("Cannot export the selected report.");
    }
    output.commit(true);
}
void saveEditedLip(const fs::path& path, const std::vector<cslu::KotorKeyframe>& frames, double durationSeconds, bool replace) {
    if (lower(neoshared::pathToUtf8(path.extension())) != ".lip") throw std::runtime_error("Choose a .lip filename.");
    AtomicOutput output(path);
    cslu::writeKotorKeyframes(output.temporary, frames, durationSeconds);
    (void)validateWritten(output.temporary);
    output.commit(replace);
}
} // namespace neolip
