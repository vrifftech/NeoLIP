#include "dialogue_batch.hpp"
#include "dialogue_fixtures.hpp"
#include "fingerprint.hpp"
#include "wav.hpp"
#include <neotlk/TlkLookup.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace neolip;
namespace {
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F f, const std::string& message) { bool failed = false; try { f(); } catch (const std::exception&) { failed = true; } require(failed, message); }
void put(const fs::path& path, const std::string& bytes) { fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary); out << bytes; }
void wave(const fs::path& path, double frequency = 170) {
    cslu::DecodedWaveform audio; audio.sampleRate = 16000; audio.mono.resize(16000);
    for (std::size_t i = 0; i < audio.mono.size(); ++i) audio.mono[i] = static_cast<float>(.15 * std::sin(i * 2 * 3.141592653589793 * frequency / 16000));
    fs::create_directories(path.parent_path()); cslu::writePcm16WaveformSegment(path, audio, 0, 1);
}
struct Scratch {
    fs::path path = fs::temp_directory_path() / fs::u8path(u8"NeoLIP DLG é spaces-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { fs::create_directories(path); }
    ~Scratch() { std::error_code ec; fs::remove_all(path, ec); }
};
void resolutionTests() {
    Scratch scratch; const auto root = scratch.path;
    const auto selected = root / "selected folder";
    const auto game = selected / "KotOR.app/Contents/Resources/GameData";
    const auto dlg = root / "work outside install/scene.dlg";
    dialogue_fixture::tlk(game / "DiAlOg.TlK", {"hello there"});
    wave(game / "StreamWaves/module/character/SOUND_REF.WAV");
    wave(game / "StreamVoice/output_name.wav", 200);
    wave(game / "StreamVoice/vo_fallback.wav");
    wave(game / "any custom name/deep/deeper/sound_only.wav");
    wave(game / "StreamSounds/vo_only.wav");
    wave(game / "OVERRIDE/vO_CaSe.WaV");
    wave(game / "StreamVoice/same.wav");
    fs::create_directories(game / "copies");
    fs::copy_file(game / "StreamVoice/same.wav", game / "copies/SAME.WAV");
    wave(game / "StreamVoice/different.wav");
    wave(game / "copies/DIFFERENT.wav", 250);
    wave(game / "StreamVoice/ambig_output.wav");
    wave(game / "suffix/full_name_extra.wav");
    wave(game / "mp3_names/raw_mp3.MP3"); // Name-index test; payload is deliberately PCM.
    dialogue_fixture::write(dlg, {
        {"output_name", "sound_ref", "", 0},
        {"vo_fallback", "absent_sound", "hello there"},
        {"", "sound_only", "hello there"},
        {"vo_only", "", "hello there"},
        {"VoCaseLip", "vo_CASE", "hello there"},
        {"same", "", "hello there"},
        {"different", "", "hello there"},
        {"ambig_output", "different", "hello there"},
        {"absent_vo", "absent_other", "hello there"},
        {"full_name", "", "hello there"},
        {"vo_case", "../outside", "hello there"},
        {"raw_mp3", "", "hello there"}
    });
    auto plan = planDialogue({dlg, selected, {}, root / "out"});
    require(plan.request.audioRoot == selected, "Selected containing folder was silently replaced");
    require(plan.request.talkTable == game / "DiAlOg.TlK", "Nested case-insensitive TLK not found");
    require(plan.recordingsIndexed == 13, "Single-pass recursive index missed recordings");
    require(plan.batch.pairs.size() == 7, "Unexpected auto-resolution count");
    require(plan.rows[0].audio == game / "StreamWaves/module/character/SOUND_REF.WAV" &&
        plan.rows[0].output.filename() == "output_name.lip", "Sound priority or distinct LIP name lost");
    require(plan.rows[0].audioCandidates.size() == 2, "Explicit alternate VO match unavailable for review");
    require(plan.rows[1].audio == game / "StreamVoice/vo_fallback.wav" &&
        plan.rows[1].audioResolution.find("Sound recording not found") != std::string::npos, "Populated missing Sound blocked VO fallback");
    require(plan.rows[2].state == DialogueState::Ready && plan.rows[2].output.filename() == "sound_only.lip", "Sound-only line needs per-row approval");
    require(plan.rows[3].state == DialogueState::Ready && plan.rows[4].state == DialogueState::Ready, "Resource lookup depends on folder/case");
    require(plan.rows[5].state == DialogueState::Ready && plan.rows[5].audioResolution.find("identical copies") != std::string::npos,
        "Identical backups caused unnecessary ambiguity");
    require(plan.rows[6].state == DialogueState::AmbiguousAudio && plan.rows[6].audio.empty(), "Different recordings selected by enumeration order");
    require(plan.rows[7].state == DialogueState::AmbiguousAudio && plan.rows[7].audio.empty() && plan.rows[7].audioCandidates.size() == 3,
        "Ambiguous Sound was bypassed by a unique VO candidate");
    require(plan.rows[8].state == DialogueState::MissingAudio && plan.rows[8].detail.find("absent_vo") != std::string::npos &&
        plan.rows[8].detail.find("absent_other") != std::string::npos, "Missing result does not identify both attempted names");
    require(plan.rows[9].state == DialogueState::MissingAudio, "Fuzzy/suffix match selected unrelated speech");
    require(plan.rows[10].state == DialogueState::NeedsReview, "Unsafe resource name bypassed by fallback");
    require(plan.rows[11].state == DialogueState::Ready, "Case-insensitive MP3 name not indexed");
    require(!fs::exists(plan.batch.outputRoot), "Automatic resolution created sidecars or outputs");
    const auto repeat = planDialogue({dlg, selected, {}, root / "out"});
    for (std::size_t i = 0; i < plan.rows.size(); ++i)
        require(plan.rows[i].audio == repeat.rows[i].audio && plan.rows[i].state == repeat.rows[i].state,
            "Resource resolution is nondeterministic");

    // Explicit selection is scoped: a subfolder does not expand its audio search
    // to siblings, even though its parent's dialog.tlk remains discoverable.
    const auto sub = planDialogue({dlg, game / "StreamWaves", {}, root / "sub out"});
    require(sub.request.talkTable == game / "DiAlOg.TlK" && sub.rows[0].state == DialogueState::Ready,
        "Voice subfolder lost installation TLK");
    require(sub.rows[1].state == DialogueState::MissingAudio, "Subfolder audio search escaped the selected scope");

    // Root TLK beats nested backup tables; a wrapper containing two installations
    // cannot safely choose a TLK. An explicit selection resolves that question.
    dialogue_fixture::tlk(selected / "DIALOG.TLK", {"hello there"});
    auto rootTable = planDialogue({dlg, selected, {}, root / "out"});
    require(rootTable.request.talkTable == selected / "DIALOG.TLK", "Nested table overrode root installation table");
    fs::remove(selected / "DIALOG.TLK");
    dialogue_fixture::tlk(selected / "other game/dialog.tlk", {"different text"});
    rejects([&] { planDialogue({dlg, selected, {}, root / "out"}); }, "Chose a random nested TLK from multiple installs");
    require(planDialogue({dlg, selected, game / "DiAlOg.TlK", root / "out"}).rows[0].spokenText == "hello there",
        "Explicit TLK did not resolve multi-game containing folder");
    fs::remove_all(selected / "other game");

    // No directory cycles or imports from symlinked files outside the selection.
    const auto outside = root / "outside/linked.wav"; wave(outside);
    std::error_code linkError;
    fs::create_symlink(outside, selected / "linked.wav", linkError);
    if (!linkError) {
        dialogue_fixture::write(root / "link.dlg", {{"linked", "", "hello there"}});
        require(planDialogue({root / "link.dlg", selected, {}, root / "out"}).rows[0].state == DialogueState::MissingAudio,
            "Followed linked audio outside the selected folder");
    }
    fs::create_directory_symlink(selected, game / "loop", linkError);
    if (!linkError) require(planDialogue({dlg, selected, {}, root / "out"}).recordingsIndexed == 13, "Directory symlink cycle followed");

    std::atomic<bool> cancelled{false}; Context context; context.work.cancelled = &cancelled;
    context.progress = [&](const Progress& step) { if (step.message == "Indexing selected folder and all subfolders") cancelled.store(true); };
    rejects([&] { planDialogue({dlg, selected, {}, root / "out"}, context); }, "Cancelled folder scan continued");
}
void test(const fs::path& source) {
    Scratch scratch; const auto root = scratch.path;
    const auto game = root / "game folder", voices = game / "StreamVoice", dlg = game / "override/conversation.dlg";
    dialogue_fixture::tlk(game / "dialog.tlk", {"hello there", "welcome", "different words"});
    wave(voices / "nested/LINE_A.WAV"); wave(voices / "nested/audio_b.wav"); wave(voices / "reply_a.wav");
    wave(voices / "bad_text.wav"); wave(voices / "token.wav"); wave(voices / "sound_only.wav");
    dialogue_fixture::write(dlg, {
        {"line_a", "", "hello there"},
        {"lip_b", "audio_b", "", 0},
        {"line_a", "", "hello there"},
        {"", "", "unvoiced choice"},
        {"missing", "", "hello"},
        {"bad_text", "", "", 0xfffffffeu},
        {"token", "", "Hello <FullName>"},
        {"", "sound_only", "hello"},
        {"reply_a", "", "welcome", 0xffffffffu, true},
        {"", "", "unvoiced reply", 0xffffffffu, true}
    });
    const auto sourceDlgHash = fileFingerprint(dlg), sourceTlkHash = fileFingerprint(game / "dialog.tlk");
    auto plan = planDialogue({dlg, voices, {}, root / "generated lips"});
    require(plan.rows.size() == 10, "Reader followed logical links or missed a list");
    require(plan.request.talkTable == game / "dialog.tlk", "TLK not discovered above voice tree");
    require(plan.batch.pairs.size() == 4, "Sound-only and VO lines should resolve automatically");
    require(plan.rows[1].audio.filename() == "audio_b.wav" && plan.rows[1].output.filename() == "lip_b.lip", "Sound and VO resource names conflated");
    require(plan.rows[1].spokenText == "hello there" && plan.rows[1].line.strref == 0, "TLK StrRef zero not resolved");
    require(plan.rows[2].state == DialogueState::Duplicate, "Shared node not deduplicated");
    require(plan.rows[3].state == DialogueState::NoVoice && plan.rows[9].state == DialogueState::NoVoice, "Unvoiced text generated speech");
    require(plan.rows[4].state == DialogueState::MissingAudio, "Missing audio invented");
    require(plan.rows[5].state == DialogueState::MissingText, "Invalid StrRef became transcript");
    require(plan.rows[6].state == DialogueState::NeedsReview, "Game substitution silently dropped");
    require(plan.rows[8].line.kind == neoshared::dlg::NodeKind::Reply, "Voiced replies ignored");
    require(!fs::exists(plan.batch.outputRoot), "Scan created output directory");
    require(plan.batch.pairs.front().spokenText.has_value() && plan.batch.pairs.front().transcript.empty(), "DLG import requires sidecar");

    // Duplicated physical names are never resolved by filesystem enumeration order.
    wave(voices / "backup/line_a.wav", 220);
    auto ambiguous = planDialogue({dlg, voices, {}, root / "ambiguous"});
    require(ambiguous.rows[0].audio.empty() && ambiguous.rows[0].state == DialogueState::Conflict, "Ambiguous shared resource chosen arbitrarily");
    ambiguous.rows[0].audio = voices / "nested/LINE_A.WAV";
    ambiguous.rows[2].excluded = true; rebuildDialogueBatch(ambiguous);
    require(ambiguous.rows[0].state == DialogueState::Ready, "Explicit audio/exclusion did not resolve duplicate");
    fs::remove_all(voices / "backup");

    // Conflicting transcripts sharing an output quarantine every contender.
    plan.rows[2].spokenText = "welcome"; rebuildDialogueBatch(plan);
    require(plan.rows[0].state == DialogueState::Conflict && plan.rows[2].state == DialogueState::Conflict, "First conflicting transcript won");
    plan.rows[2].excluded = true; rebuildDialogueBatch(plan);
    require(plan.rows[0].state == DialogueState::Ready, "Explicit exclusion did not restore ready row");
    require(plan.rows[7].state == DialogueState::Ready, "Sound-only voiced line unnecessarily requires confirmation");
    plan.rows[7].textConfirmed = true;
    // In-memory correction; original DLG remains untouched.
    plan.rows[6].spokenText = "hello there"; plan.rows[6].textConfirmed = true; rebuildDialogueBatch(plan);
    require(plan.rows[6].state == DialogueState::Ready, "Reviewed game token not accepted");
    require(fileFingerprint(dlg) == sourceDlgHash && fileFingerprint(game / "dialog.tlk") == sourceTlkHash, "Import modified source");
    require(plan.batch.pairs.size() == 5, "Corrected line missing");

    // Actual engine path uses the selected DLG/TLK text, even if a wrong sidecar exists.
    put(voices / "nested/LINE_A.txt", "notawordzzzz");
    Assets assets{source / "third_party/cslu_runtime/Toolkit/2.0/apps/fa", source / "dictionaries", root / "user/pronunciations.dict"};
    Engine engine(assets);
    const auto result = engine.batch(plan.batch, MouthMovement::Natural, false);
    require(result.created == 5 && result.failed == 0, "Real DLG batch alignment failed");
    std::set<std::string> outputs;
    for (const auto& entry : fs::recursive_directory_iterator(plan.batch.outputRoot)) if (entry.is_regular_file()) {
        require(entry.path().extension() == ".lip", "Non-LIP output leaked"); outputs.insert(entry.path().filename().u8string());
    }
    require(outputs == std::set<std::string>{"line_a.lip", "lip_b.lip", "reply_a.lip", "sound_only.lip", "token.lip"}, "Wrong LIP resource filenames");
    for (const auto& item : result.items) require(!item.transcript.empty() && !item.sourceLabel.empty(), "Review/retry lost literal transcript/node association");
    const auto skipped = engine.batch(plan.batch, MouthMovement::Natural, false);
    require(skipped.skipped == 5 && skipped.failed == 0, "Existing LIP policy bypassed");
    for (const auto& item : skipped.items) require(!item.transcript.empty(), "Skipped DLG line lost text");
    require(fileFingerprint(dlg) == sourceDlgHash && fileFingerprint(game / "dialog.tlk") == sourceTlkHash, "Engine modified DLG/TLK");
    // One recording may legitimately supply differently named LIPs.
    auto repeatedAudio = plan.batch;
    repeatedAudio.outputRoot = root / "shared recording outputs";
    repeatedAudio.pairs = {plan.batch.pairs.front(), plan.batch.pairs.front()};
    repeatedAudio.pairs[0].output = repeatedAudio.outputRoot / "one.lip";
    repeatedAudio.pairs[1].output = repeatedAudio.outputRoot / "two.lip";
    const auto repeatedResult = engine.batch(repeatedAudio, MouthMovement::Natural, false);
    require(repeatedResult.created == 2 && repeatedResult.items[0].output != repeatedResult.items[1].output, "Batch keyed results by audio instead of output");
    const auto talk = game / "dialog.tlk";
    std::ifstream talkInput(talk, std::ios::binary);
    std::string talkBytes((std::istreambuf_iterator<char>(talkInput)), {}); talkInput.close();
    auto modifiedTalk = talkBytes; modifiedTalk.back() = modifiedTalk.back() == 'x' ? 'y' : 'x'; put(talk, modifiedTalk);
    rejects([&] { engine.batch(plan.batch, MouthMovement::Natural, true); }, "Stale TLK snapshot accepted");
    put(talk, talkBytes);
    auto stale = plan; put(dlg, "changed");
    rejects([&] { engine.batch(stale.batch, MouthMovement::Natural, true); }, "Stale DLG snapshot accepted");

    // Read-only adapter uses real shared GFF and TLK serialization/decoding.
    const auto local = root / "standalone/local.dlg"; wave(root / "isolated/line.wav");
    dialogue_fixture::write(local, {{"line", "", std::string("I\x92m ready")}});
    const auto decoded = neoshared::dlg::read(local);
    require(decoded.lines[0].text == u8"I’m ready", "Embedded Windows-1252 text not decoded by shared codec");
    auto noTable = planDialogue({local, root / "isolated", {}, {}});
    require(noTable.request.talkTable.empty() && noTable.batch.pairs.size() == 1, "Embedded-only DLG requires TLK");
    dialogue_fixture::write(local, {{"line", "", "hello", 2}});
    auto disagreement = planDialogue({local, root / "isolated", game / "dialog.tlk", {}});
    require(disagreement.rows[0].state == DialogueState::NeedsReview, "DLG/TLK disagreement silently chosen");
    disagreement.rows[0].textConfirmed = true; rebuildDialogueBatch(disagreement);
    require(disagreement.batch.pairs.size() == 1, "Explicit text confirmation ignored");
    dialogue_fixture::write(local, {{"line", "", "", 0}});
    dialogue_fixture::tlk(root / "French.tlk", {"bonjour"}, 1);
    auto wrongLanguage = planDialogue({local, root / "isolated", root / "French.tlk", {}});
    require(wrongLanguage.batch.pairs.empty() && wrongLanguage.rows[0].spokenText.empty(), "Foreign TLK aligned as English");
    dialogue_fixture::write(local, {{"line", "", "alternate", 0xffffffffu, false, 1}});
    require(neoshared::dlg::read(local).lines[0].text.empty(), "Alternate local text silently used");
    neoshared::dlg::ReadOptions variant; variant.gender = 1;
    require(neoshared::dlg::read(local, variant).lines[0].text == "alternate", "Shared reader lost explicit variant support");
    dialogue_fixture::write(local, {{"../escape", "", "hello"}});
    auto unsafe = planDialogue({local, root / "isolated", {}, {}});
    require(unsafe.batch.pairs.empty() && unsafe.rows[0].output.empty(), "Unsafe resource escaped output root");
    dialogue_fixture::write(local, {{"line", "", "hello"}});
    auto outside = planDialogue({local, root / "isolated", {}, {}});
    outside.rows[0].audio = voices / "reply_a.wav"; rebuildDialogueBatch(outside);
    require(outside.batch.pairs.empty(), "Reviewed audio escaped selected root");
    put(root / "broken.tlk", "broken"); rejects([&] { planDialogue({local, root / "isolated", root / "broken.tlk", {}}); }, "Invalid explicit TLK ignored");
    put(root / "broken.dlg", std::string(64, '\0')); rejects([&] { planDialogue({root / "broken.dlg", root / "isolated", {}, {}}); }, "Malformed GFF accepted");
    // A bad resource-field type is a visible invalid node, not a quiet skip.
    const auto malformed = root / "malformed node.dlg";
    {
        neogff::GffFile bad; bad.NewFile("DLG ");
        auto entries = std::make_unique<neogff::GffList>("EntryList");
        auto node = std::make_unique<neogff::GffStruct>();
        node->AddField(std::make_unique<neogff::GffUInt32Field>("VO_ResRef", 0));
        entries->AddStruct(std::move(node)); bad.root()->AddField(std::move(entries)); bad.SaveFile(malformed);
    }
    const auto invalidNode = planDialogue({malformed, root / "isolated", {}, {}});
    require(invalidNode.rows[0].state == DialogueState::NeedsReview && invalidNode.rows[0].line.malformed,
        "Invalid node silently skipped as unvoiced");
    // Corrupt an actual classic GFF structural reference into a cycle. Logical
    // DLG Index links were already tested above and must remain permitted.
    const auto cycle = root / "cyclic.dlg";
    dialogue_fixture::write(cycle, {{"line", "", "hello"}});
    std::ifstream cycleInput(cycle, std::ios::binary);
    std::string cycleBytes((std::istreambuf_iterator<char>(cycleInput)), {}); cycleInput.close();
    const auto read32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(cycleBytes.at(at + i))) << (8 * i);
        return v;
    };
    std::size_t at = read32(48); bool patched = false;
    const auto end = at + read32(52);
    while (at + 4 <= end) {
        const auto count = read32(at);
        if (count) { for (unsigned i = 0; i < 4; ++i) cycleBytes.at(at + 4 + i) = 0; patched = true; break; }
        at += 4;
    }
    require(patched, "Cycle fixture has no list entries"); put(cycle, cycleBytes);
    rejects([&] { neoshared::dlg::read(cycle); }, "Cyclic GFF structure was followed indefinitely");
    std::atomic<bool> cancelled{true}; Context context; context.work.cancelled = &cancelled;
    rejects([&] { planDialogue({local, root / "isolated", {}, {}}, context); }, "Cancelled scan continued");
    auto cancelledPlan = outside.batch;
    const auto cancelledResult = engine.batch(cancelledPlan, MouthMovement::Natural, false);
    require(cancelledResult.created == 0, "Empty plan created output");
    for (const auto& entry : fs::recursive_directory_iterator(root))
        require(entry.path().filename().u8string().rfind(".neolip-tmp-", 0) != 0, "Temporary file leaked");
}
} // namespace
int main(int argc, char** argv) {
    try { if (argc != 2) throw std::runtime_error("Expected NeoLIP source path"); resolutionTests(); test(fs::u8path(argv[1]));
        std::cout << "PASS: shared DLG/TLK reader, dialogue planning, and real LIP-only engine batch\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
