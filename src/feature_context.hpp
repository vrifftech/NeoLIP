#pragma once

#include "tsv.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace cslu {

struct MatrixCompareStats {
    std::size_t rows = 0;
    std::size_t cols = 0;
    double meanAbsError = 0.0;
    double maxAbsError = 0.0;
    std::size_t maxRow = 0;
    std::size_t maxCol = 0;
};

struct FeatureParts {
    Matrix plpcms;     // all_feat columns 0..25
    Matrix burstPulse; // all_feat column 26
    Matrix intensity;  // all_feat columns 27..42
    Matrix energy;     // all_feat column 43
};

Matrix sliceColumns(const Matrix& m, std::size_t firstCol, std::size_t colCount);
Matrix takeRows(const Matrix& m, std::size_t rowCount);
Matrix joinColumns(const std::vector<Matrix>& parts);

// CSLU collect behavior for the feature-context stage:
// for each frame r and spec {offset,width}, average source rows
// r+offset ... r+offset+width-1. Out-of-range rows contribute zeros
// and are still counted in the denominator.
Matrix collectAverageWithZeroPadding(const Matrix& source,
                                     const std::vector<std::pair<int, int>>& frameSpecs);

FeatureParts splitAllFeat(const Matrix& allFeat);
Matrix composeAllFeat(const Matrix& plpFeat16k,
                      const Matrix& burstPulsesCol7,
                      const Matrix& intensity0To15,
                      const Matrix& energyCol0);
Matrix composeNetInFromAllFeat(const Matrix& allFeat);
MatrixCompareStats compareMatrixValues(const Matrix& actual, const Matrix& expected);

void writeNumericTsv(const Matrix& m, const std::filesystem::path& path, int precision = 9);
void ensureDirectory(const std::filesystem::path& dir);

} // namespace cslu
