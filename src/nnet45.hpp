#pragma once

#include "tsv.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace cslu {

struct NNet45 {
    int version = 0;
    int layers = 0;
    int inputCount = 0;
    int hiddenCount = 0;
    int outputCount = 0;
    std::vector<float> wInputHidden; // hidden-major: hiddenCount rows x inputCount cols
    std::vector<float> wHiddenOutput; // output-major: outputCount rows x hiddenCount cols

    static NNet45 load(const std::filesystem::path& path);

    std::vector<double> forwardOne(const std::vector<double>& acousticInput) const;
    Matrix forwardMatrix(const Matrix& acousticInputs) const;
    std::size_t expectedFloatCount() const;
};

struct NNetCompareStats {
    std::size_t rows = 0;
    std::size_t cols = 0;
    double meanAbsError = 0.0;
    double maxAbsError = 0.0;
    std::size_t maxRow = 0;
    std::size_t maxCol = 0;
    std::size_t top1Matches = 0;
};

NNetCompareStats compareMatrices(const Matrix& actual, const Matrix& expected);

} // namespace cslu
