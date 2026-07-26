#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace cslu {

struct PhnInterval {
    int startMs = 0;
    int endMs = 0;
    std::string label;
};

struct PhnFile {
    double millisecondsPerFrame = 1.0;
    std::vector<PhnInterval> intervals;
};

PhnFile readPhn(const std::filesystem::path& path);
void writePhn(const std::filesystem::path& path, const PhnFile& phn);

} // namespace cslu
