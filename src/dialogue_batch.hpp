#pragma once
#include "engine.hpp"
#include <dlg/Dialogue.hpp>

namespace neolip {
struct DialogueRequest {
    fs::path dialogue, audioRoot, talkTable, outputRoot;
};
enum class DialogueState { Ready, NoVoice, MissingAudio, AmbiguousAudio, MissingText,
                           NeedsReview, Duplicate, Conflict, Excluded };
struct DialogueRow {
    neoshared::dlg::Line line;
    fs::path audio, output;
    std::string spokenText;
    bool excluded = false;
    bool textConfirmed = false;
    DialogueState state = DialogueState::NoVoice;
    std::string detail;
    std::string audioResolution; // Which DLG field supplied the automatically selected recording.
    std::vector<fs::path> audioCandidates;
};
struct DialoguePlan {
    DialogueRequest request;
    std::vector<DialogueRow> rows;
    BatchPlan batch;
    std::size_t recordingsIndexed = 0;
};
// No archive mounting, script execution, guessed speech, or transcript sidecars.
// The selected tree is indexed once for loose WAV/MP3 resources and dialog.tlk.
// Sound is tried first; when absent on disk, VO_ResRef is tried independently.
// Identical copies are one recording; different copies require an explicit choice.
DialoguePlan planDialogue(const DialogueRequest& request, const Context& context = {});
// Revalidate edited/omitted rows and quarantine every conflicting destination.
// Original source snapshots remain attached; Engine rejects stale plans.
void rebuildDialogueBatch(DialoguePlan& plan);
std::string dialogueStateName(DialogueState state);
fs::path defaultDialogueOutput(const fs::path& dialogue);
} // namespace neolip
