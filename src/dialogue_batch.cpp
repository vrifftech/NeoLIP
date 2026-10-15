#include "dialogue_batch.hpp"
#include <neoshared/PathUtf8.hpp>
#include <neoshared/game/Installation.hpp>
#include "fingerprint.hpp"
#include <neotlk/TlkLookup.hpp>
#include <algorithm>
#include <map>
#include <stdexcept>

namespace neolip {
namespace {
std::string lower(std::string text) { for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return text; }
fs::path canonicalPath(const fs::path& path) { return fs::weakly_canonical(fs::absolute(path)); }
bool contains(const fs::path& root, const fs::path& path) {
    const auto base = canonicalPath(root), candidate = canonicalPath(path);
    auto p = candidate.begin();
    for (auto r = base.begin(); r != base.end(); ++r, ++p) {
        if (p == candidate.end()) return false;
#ifdef _WIN32
        if (lower(r->u8string()) != lower(p->u8string())) return false;
#else
        if (*r != *p) return false;
#endif
    }
    return true;
}
bool regular(const fs::path& path) { std::error_code ec; return !path.empty() && fs::is_regular_file(path, ec) && !ec; }
bool safeResRef(const std::string& value) {
    if (value.empty() || value.size() > 16) return false;
    for (unsigned char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    // Windows device basenames must not become output files on another platform.
    const auto name = lower(value);
    if (name == "con" || name == "prn" || name == "aux" || name == "nul") return false;
    if (name.size() == 4 && (name.rfind("com", 0) == 0 || name.rfind("lpt", 0) == 0) && name[3] >= '1' && name[3] <= '9') return false;
    return true;
}
std::string lipRef(const DialogueRow& row) { return row.line.voiceResRef.empty() ? row.line.soundResRef : row.line.voiceResRef; }
std::vector<std::string> audioRefs(const DialogueRow& row) {
    std::vector<std::string> refs;
    if (!row.line.soundResRef.empty()) refs.push_back(lower(row.line.soundResRef));
    const auto vo = lower(row.line.voiceResRef);
    if (!vo.empty() && (refs.empty() || vo != refs.front())) refs.push_back(vo);
    return refs;
}
std::string requestedAudio(const DialogueRow& row) {
    std::string result;
    if (!row.line.soundResRef.empty()) result = "Sound '" + row.line.soundResRef + "'";
    if (!row.line.voiceResRef.empty()) {
        if (!result.empty()) result += " / ";
        result += "VO_ResRef '" + row.line.voiceResRef + "'";
    }
    return result;
}
bool dynamicText(const std::string& text) {
    // Never let the transcript tokenizer silently erase runtime substitutions.
    // The user confirms/replaces these words in the in-memory preview instead.
    return text.find('<') != std::string::npos || text.find('>') != std::string::npos;
}
std::vector<fs::path> named(const fs::path& directory, const std::string& basename) {
    std::vector<fs::path> result; std::error_code ec;
    fs::directory_iterator it(directory, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (lower(neoshared::pathToUtf8(it->path().filename())) == basename && regular(it->path())) result.push_back(canonicalPath(it->path()));
    }
    return result;
}
struct AudioMatches {
    std::vector<fs::path> paths;
    fs::path selected;
    bool checked = false;
};
struct FolderIndex {
    std::map<std::string, AudioMatches> audio;
    std::vector<fs::path> talkTables;
    std::size_t recordings = 0;
};
FolderIndex indexFolder(const fs::path& root, const Context& context) {
    // A game installation, a voice subfolder and an arbitrary project folder
    // follow exactly the same rule. No hard-coded Steam paths or filename guesses.
    FolderIndex index;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, ec), end;
    if (ec) throw std::runtime_error("Cannot scan selected folder: " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec) throw std::runtime_error("Could not finish scanning selected folder: " + ec.message());
        context.work.check();
        const auto entry = *it;
        // Keep the search inside the explicitly selected tree; do not follow
        // links to backups, another installation or a directory cycle.
        if (entry.is_symlink()) { it.disable_recursion_pending(); continue; }
        if (!entry.is_regular_file()) continue;
        const auto ext = lower(neoshared::pathToUtf8(entry.path().extension()));
        if (ext == ".wav" || ext == ".mp3") {
            index.audio[lower(neoshared::pathToUtf8(entry.path().stem()))].paths.push_back(canonicalPath(entry.path()));
            ++index.recordings;
        } else if (lower(neoshared::pathToUtf8(entry.path().filename())) == "dialog.tlk") {
            index.talkTables.push_back(canonicalPath(entry.path()));
        }
    }
    if (ec) throw std::runtime_error("Could not finish scanning selected folder: " + ec.message());
    for (auto& item : index.audio) std::sort(item.second.paths.begin(), item.second.paths.end());
    std::sort(index.talkTables.begin(), index.talkTables.end());
    return index;
}
fs::path uniqueTable(const std::vector<fs::path>& paths, const fs::path& root) {
    if (paths.size() > 1) throw std::runtime_error(
        "More than one dialog.tlk in " + neoshared::pathToUtf8(root) +
        "; select the intended game folder or choose its talk table explicitly.");
    return paths.empty() ? fs::path{} : paths.front();
}
std::optional<fs::path> installationTalkTable(const fs::path& source) {
    fs::path current = neoshared::game::normalizeConfiguredPath(source);
    if (!neoshared::game::isDirectoryPath(current)) current = current.parent_path();
    while (!current.empty()) {
        if (const auto install = neoshared::game::inspectGameInstallation(
                current, std::nullopt, "NeoLIP selected folder")) {
            if (neoshared::game::isRegularFilePath(install->tlkPath)) return install->tlkPath;
        }
        const auto parent = current.parent_path();
        if (parent.empty() || parent == current) break;
        current = parent;
    }
    return std::nullopt;
}

fs::path findTlk(const DialogueRequest& request, const FolderIndex& index) {
    // Prefer a validated installation table. This handles game roots, voice
    // subfolders, macOS bundles, and Linux paths containing spaces without
    // shell parsing or directory-name guesses.
    if (const auto table = installationTalkTable(request.audioRoot)) return *table;
    if (const auto table = installationTalkTable(request.dialogue)) return *table;
    // A root-level installation table beats any nested backup. A single nested
    // table also works when the user selects a containing folder or macOS bundle.
    std::vector<fs::path> direct;
    for (const auto& table : index.talkTables)
        if (table.parent_path() == request.audioRoot) direct.push_back(table);
    if (!direct.empty()) return uniqueTable(direct, request.audioRoot);
    if (!index.talkTables.empty()) return uniqueTable(index.talkTables, request.audioRoot);
    // Only a validated installation may contribute a table from outside the
    // explicitly selected audio tree. An arbitrary parent directory is not an
    // installation merely because it contains a file named dialog.tlk.
    const auto directory = request.dialogue.parent_path();
    return uniqueTable(named(directory, "dialog.tlk"), directory);
}
void chooseIdenticalRecording(AudioMatches& matches, const Context& context) {
    if (matches.checked) return;
    matches.checked = true;
    if (matches.paths.empty()) return;
    if (matches.paths.size() == 1) { matches.selected = matches.paths.front(); return; }
    const auto bytes = fs::file_size(matches.paths.front());
    for (const auto& path : matches.paths) {
        context.work.check();
        if (fs::file_size(path) != bytes) return;
    }
    // Hash only colliding, referenced resources; not an installation's entire
    // audio collection. The fingerprint helper also checks mid-read mutation.
    const auto fingerprint = fileFingerprint(matches.paths.front());
    for (std::size_t i = 1; i < matches.paths.size(); ++i) {
        context.work.check();
        if (fileFingerprint(matches.paths[i]) != fingerprint) return;
    }
    matches.selected = matches.paths.front();
}
void resolveAudio(DialogueRow& row, FolderIndex& index, const Context& context) {
    AudioMatches* preferred = nullptr;
    std::string chosenRef;
    for (const auto& ref : audioRefs(row)) {
        if (!safeResRef(ref)) continue;
        const auto found = index.audio.find(ref);
        if (found == index.audio.end()) continue;
        auto& matches = found->second;
        row.audioCandidates.insert(row.audioCandidates.end(), matches.paths.begin(), matches.paths.end());
        if (!preferred) { preferred = &matches; chosenRef = ref; }
    }
    if (!preferred) return;
    chooseIdenticalRecording(*preferred, context);
    // An ambiguous Sound is not a missing Sound: do not dodge a real collision
    // by silently selecting an unrelated VO candidate with the other name.
    row.audio = preferred->selected;
    if (row.audio.empty()) return;
    const bool sound = !row.line.soundResRef.empty() && chosenRef == lower(row.line.soundResRef);
    row.audioResolution = sound ? "Sound match" : "VO_ResRef match";
    if (!sound && !row.line.soundResRef.empty()) row.audioResolution += " (Sound recording not found)";
    if (preferred->paths.size() > 1) row.audioResolution += " (identical copies)";
}
void state(DialogueRow& row, DialogueState value, const std::string& detail) { row.state = value; row.detail = detail; }
} // namespace
fs::path defaultDialogueOutput(const fs::path& dialogue) {
    auto folder = dialogue.stem(); folder += "_lips";
    return dialogue.parent_path() / folder;
}
std::string dialogueStateName(DialogueState state) {
    switch (state) {
    case DialogueState::Ready: return "Ready";
    case DialogueState::NoVoice: return "No voice resource";
    case DialogueState::MissingAudio: return "Missing audio";
    case DialogueState::AmbiguousAudio: return "Choose audio";
    case DialogueState::MissingText: return "Missing text";
    case DialogueState::NeedsReview: return "Review text";
    case DialogueState::Duplicate: return "Shared line";
    case DialogueState::Conflict: return "Conflicting LIP";
    case DialogueState::Excluded: return "Excluded";
    }
    return "Excluded";
}
DialoguePlan planDialogue(const DialogueRequest& supplied, const Context& context) {
    cslu::ScopedWorkControl control(context.work);
    context.work.check();
    DialoguePlan result; result.request = supplied;
    auto& request = result.request;
    if (request.dialogue.empty() || !regular(request.dialogue)) throw std::runtime_error("Choose a DLG file.");
    if (request.audioRoot.empty() || !fs::is_directory(request.audioRoot)) throw std::runtime_error("Choose a game directory or any folder containing the recordings.");
    request.dialogue = canonicalPath(request.dialogue); request.audioRoot = canonicalPath(request.audioRoot);
    request.outputRoot = canonicalPath(request.outputRoot.empty() ? defaultDialogueOutput(request.dialogue) : request.outputRoot);
    if (context.progress) context.progress({"Indexing selected folder and all subfolders", 0, 0});
    auto index = indexFolder(request.audioRoot, context);
    result.recordingsIndexed = index.recordings;
    request.talkTable = request.talkTable.empty() ? findTlk(request, index) : canonicalPath(request.talkTable);
    result.batch.inputRoot = request.audioRoot; result.batch.outputRoot = request.outputRoot;
    result.batch.dialogueSource = request.dialogue;
    result.batch.sourceSnapshots.push_back({request.dialogue, fileFingerprint(request.dialogue)});
    neotlk::TlkLookup tlk;
    if (!request.talkTable.empty()) {
        if (!regular(request.talkTable)) throw std::runtime_error("The selected TLK does not exist.");
        result.batch.sourceSnapshots.push_back({request.talkTable, fileFingerprint(request.talkTable)});
        if (context.progress) context.progress({"Reading dialogue text from TLK", 0, 0});
        tlk.load(request.talkTable);
    }
    if (context.progress) context.progress({"Reading dialogue entries and replies", 0, 0});
    neoshared::dlg::ReadOptions options; options.talkTable = tlk.loaded() ? &tlk : nullptr;
    options.checkpoint = [&]() { context.work.check(); };
    const auto dialogue = neoshared::dlg::read(request.dialogue, options);
    if (context.progress) context.progress({"Resolving dialogue Sound and VO_ResRef recordings", 0, 0});
    for (const auto& line : dialogue.lines) {
        context.work.check();
        DialogueRow row; row.line = line; row.spokenText = line.text;
        if (safeResRef(lipRef(row))) row.output = request.outputRoot / (lower(lipRef(row)) + ".lip");
        if (!line.malformed) resolveAudio(row, index, context);
        result.rows.push_back(std::move(row));
    }
    for (const auto& snapshot : result.batch.sourceSnapshots)
        if (fileFingerprint(snapshot.path) != snapshot.fingerprint)
            throw std::runtime_error("Dialogue or TLK changed during scanning. Scan again.");
    rebuildDialogueBatch(result);
    return result;
}
void rebuildDialogueBatch(DialoguePlan& plan) {
    auto& batch = plan.batch; batch.pairs.clear(); batch.conflicts.clear(); batch.problems.clear();
    std::map<std::string, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < plan.rows.size(); ++i) {
        auto& row = plan.rows[i];
        if (row.excluded) { state(row, DialogueState::Excluded, "Excluded from this run"); continue; }
        if (row.line.malformed) { state(row, DialogueState::NeedsReview, row.line.textProblem + "; correct this node in the dialogue editor"); continue; }
        const auto ref = lipRef(row);
        if (ref.empty()) { state(row, DialogueState::NoVoice, "No VO_ResRef or Sound; no speech was invented"); continue; }
        if (!safeResRef(ref) || (!row.line.soundResRef.empty() && !safeResRef(row.line.soundResRef))) {
            state(row, DialogueState::NeedsReview, "Invalid resource name in the DLG; correct it in the dialogue editor"); continue;
        }
        row.output = batch.outputRoot / (lower(ref) + ".lip");
        if (row.audio.empty()) {
            state(row, row.audioCandidates.size() > 1 ? DialogueState::AmbiguousAudio : DialogueState::MissingAudio,
                row.audioCandidates.size() > 1 ? "Different recordings match " + requestedAudio(row) + "; choose the intended audio" : "No WAV/MP3 under the selected folder matches " + requestedAudio(row));
        } else if (!regular(row.audio) || !contains(batch.inputRoot, row.audio) || fs::is_symlink(row.audio)) {
            state(row, DialogueState::MissingAudio, "Choose a regular audio file inside the selected search folder");
        } else if (lower(neoshared::pathToUtf8(row.audio.extension())) != ".wav" && lower(neoshared::pathToUtf8(row.audio.extension())) != ".mp3") {
            state(row, DialogueState::MissingAudio, "Choose a WAV or MP3 recording");
        } else if (!contains(batch.outputRoot, row.output)) {
            state(row, DialogueState::Conflict, "Output path leaves the selected folder");
        } else if (cslu::tokenizeTranscript(row.spokenText).empty()) {
            state(row, DialogueState::MissingText, row.line.textProblem.empty() ? "No spoken words; enter the recorded text" : row.line.textProblem);
        } else if (row.spokenText.find('\0') != std::string::npos) {
            state(row, DialogueState::NeedsReview, "Transcript contains an invalid NUL character");
        } else if ((!row.textConfirmed && !row.line.textProblem.empty()) || dynamicText(row.spokenText)) {
            state(row, DialogueState::NeedsReview, dynamicText(row.spokenText) ? "Replace game tokens/markup with the actual recorded words" : row.line.textProblem);
        } else {
            const std::string textDetail = row.textConfirmed ? "Reviewed transcript" :
                row.line.textSource == neoshared::dlg::TextSource::Embedded ? "Embedded DLG text" : "TLK StrRef " + std::to_string(row.line.strref);
            state(row, DialogueState::Ready, row.audioResolution.empty() ? textDetail : row.audioResolution + "; " + textDetail);
        }
        // Include unresolved rows in output-conflict checks: a ready row cannot
        // silently win over a second node that claims the same output resource.
        groups[lower(neoshared::pathToUtf8(row.output))].push_back(i);
    }
    for (const auto& group : groups) {
        const auto& ids = group.second; auto& first = plan.rows[ids.front()];
        bool same = first.state == DialogueState::Ready;
        for (auto id : ids) {
            const auto& row = plan.rows[id];
            same = same && row.state == DialogueState::Ready && row.audio == first.audio && row.spokenText == first.spokenText;
        }
        if (ids.size() > 1 && !same) {
            for (auto id : ids) state(plan.rows[id], DialogueState::Conflict,
                "Nodes share this LIP name but differ or are unresolved; exclude the unintended contenders");
            batch.problems.push_back("Conflicting dialogue destination: " + neoshared::pathToUtf8(first.output.filename()));
            continue;
        }
        if (first.state != DialogueState::Ready) continue;
        BatchItem item; item.audio = first.audio; item.output = first.output;
        item.spokenText = first.spokenText; item.sourceLabel = first.line.label();
        batch.pairs.push_back(std::move(item));
        for (std::size_t j = 1; j < ids.size(); ++j) state(plan.rows[ids[j]], DialogueState::Duplicate,
            "Same recording/text as " + first.line.label() + "; one LIP will be generated");
    }
}
} // namespace neolip
