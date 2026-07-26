#include "phn.hpp"
#include "tsv.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace cslu {

PhnFile readPhn(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open PHN file: " + path.string());
    }

    PhnFile phn;
    std::string line;
    bool inBody = false;
    while (std::getline(in, line)) {
        line = trimCR(line);
        if (line.empty()) {
            continue;
        }
        if (!inBody) {
            if (line.rfind("MillisecondsPerFrame:", 0) == 0) {
                auto pos = line.find(':');
                phn.millisecondsPerFrame = std::stod(line.substr(pos + 1));
            } else if (line == "END OF HEADER") {
                inBody = true;
            }
            continue;
        }

        std::istringstream ss(line);
        PhnInterval iv;
        ss >> iv.startMs >> iv.endMs >> iv.label;
        if (!ss || iv.label.empty()) {
            throw std::runtime_error("invalid PHN interval line in " + path.string() + ": " + line);
        }
        phn.intervals.push_back(iv);
    }
    return phn;
}

void writePhn(const std::filesystem::path& path, const PhnFile& phn) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("cannot write PHN file: " + path.string());
    }
    out << "MillisecondsPerFrame: " << std::fixed << std::setprecision(5) << phn.millisecondsPerFrame << "\r\n";
    out << "END OF HEADER\r\n";
    for (const auto& iv : phn.intervals) {
        out << iv.startMs << ' ' << iv.endMs << ' ' << iv.label << "\r\n";
    }
}

} // namespace cslu
