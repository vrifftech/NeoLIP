#pragma once
#include "audio.hpp"
#include "backend.hpp"
namespace neolip {
// Conservative review flags, not a calibrated confidence score. No diagnostic
// files are written. The caller presents time ranges as navigable UI warnings.
std::vector<ReviewWarning> reviewAlignment(const AudioBuffer& audio,
    const cslu::ViterbiAlignment& alignment,const cslu::TraceGraph& graph);
}
