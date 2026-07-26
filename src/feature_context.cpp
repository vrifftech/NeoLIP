#include "feature_context.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace cslu {

static void requireNonEmpty(const Matrix& m, const std::string& label) {
    if (m.rows == 0 || m.cols == 0) {
        throw std::runtime_error(label + " is empty");
    }
}

Matrix sliceColumns(const Matrix& m, std::size_t firstCol, std::size_t colCount) {
    requireNonEmpty(m, "sliceColumns input");
    if (firstCol > m.cols || colCount > m.cols - firstCol) {
        std::ostringstream err;
        err << "column slice out of range: firstCol=" << firstCol
            << " colCount=" << colCount << " sourceCols=" << m.cols;
        throw std::runtime_error(err.str());
    }
    Matrix out;
    out.rows = m.rows;
    out.cols = colCount;
    out.data.assign(out.rows * out.cols, 0.0);
    for (std::size_t r = 0; r < out.rows; ++r) {
        for (std::size_t c = 0; c < out.cols; ++c) {
            out(r, c) = m(r, firstCol + c);
        }
    }
    return out;
}

Matrix takeRows(const Matrix& m, std::size_t rowCount) {
    requireNonEmpty(m, "takeRows input");
    if (rowCount > m.rows) {
        throw std::runtime_error("takeRows rowCount exceeds input rows");
    }
    Matrix out;
    out.rows = rowCount;
    out.cols = m.cols;
    out.data.assign(out.rows * out.cols, 0.0);
    std::copy(m.data.begin(), m.data.begin() + static_cast<std::ptrdiff_t>(rowCount * m.cols), out.data.begin());
    return out;
}

Matrix joinColumns(const std::vector<Matrix>& parts) {
    if (parts.empty()) {
        throw std::runtime_error("joinColumns requires at least one matrix");
    }
    const std::size_t rows = parts.front().rows;
    std::size_t cols = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        requireNonEmpty(parts[i], "joinColumns part");
        if (parts[i].rows != rows) {
            std::ostringstream err;
            err << "joinColumns row mismatch at part " << i
                << ": got " << parts[i].rows << ", expected " << rows;
            throw std::runtime_error(err.str());
        }
        cols += parts[i].cols;
    }

    Matrix out;
    out.rows = rows;
    out.cols = cols;
    out.data.assign(rows * cols, 0.0);
    for (std::size_t r = 0; r < rows; ++r) {
        std::size_t dstCol = 0;
        for (const Matrix& part : parts) {
            for (std::size_t c = 0; c < part.cols; ++c) {
                out(r, dstCol++) = part(r, c);
            }
        }
    }
    return out;
}

Matrix collectAverageWithZeroPadding(const Matrix& source,
                                     const std::vector<std::pair<int, int>>& frameSpecs) {
    requireNonEmpty(source, "collect source");
    if (frameSpecs.empty()) {
        throw std::runtime_error("collect requires at least one frame spec");
    }
    for (const auto& spec : frameSpecs) {
        if (spec.second <= 0) {
            throw std::runtime_error("collect frame width must be positive");
        }
    }

    Matrix out;
    out.rows = source.rows;
    out.cols = source.cols * frameSpecs.size();
    out.data.assign(out.rows * out.cols, 0.0);

    for (std::size_t r = 0; r < source.rows; ++r) {
        for (std::size_t specIndex = 0; specIndex < frameSpecs.size(); ++specIndex) {
            const int offset = frameSpecs[specIndex].first;
            const int width = frameSpecs[specIndex].second;
            for (std::size_t c = 0; c < source.cols; ++c) {
                double sum = 0.0;
                for (int k = 0; k < width; ++k) {
                    const long long srcRow = static_cast<long long>(r) + static_cast<long long>(offset) + k;
                    if (srcRow >= 0 && srcRow < static_cast<long long>(source.rows)) {
                        sum += source(static_cast<std::size_t>(srcRow), c);
                    }
                }
                out(r, specIndex * source.cols + c) = sum / static_cast<double>(width);
            }
        }
    }
    return out;
}

FeatureParts splitAllFeat(const Matrix& allFeat) {
    requireNonEmpty(allFeat, "all_feat");
    if (allFeat.cols != 44) {
        std::ostringstream err;
        err << "expected all_feat to have 44 columns, got " << allFeat.cols;
        throw std::runtime_error(err.str());
    }
    FeatureParts p;
    p.plpcms = sliceColumns(allFeat, 0, 26);
    p.burstPulse = sliceColumns(allFeat, 26, 1);
    p.intensity = sliceColumns(allFeat, 27, 16);
    p.energy = sliceColumns(allFeat, 43, 1);
    return p;
}

Matrix composeAllFeat(const Matrix& plpFeat16k,
                      const Matrix& burstPulsesCol7,
                      const Matrix& intensity0To15,
                      const Matrix& energyCol0) {
    requireNonEmpty(plpFeat16k, "plpFeat16k");
    requireNonEmpty(burstPulsesCol7, "burstPulsesCol7");
    requireNonEmpty(intensity0To15, "intensity0To15");
    requireNonEmpty(energyCol0, "energyCol0");
    if (plpFeat16k.cols != 26) {
        throw std::runtime_error("plpFeat16k must have 26 columns");
    }
    if (burstPulsesCol7.cols != 1) {
        throw std::runtime_error("burstPulsesCol7 must have 1 column");
    }
    if (intensity0To15.cols != 16) {
        throw std::runtime_error("intensity0To15 must have 16 columns");
    }
    if (energyCol0.cols != 1) {
        throw std::runtime_error("energyCol0 must have 1 column");
    }
    const std::size_t rows = std::min({plpFeat16k.rows, burstPulsesCol7.rows, intensity0To15.rows, energyCol0.rows});
    return joinColumns({takeRows(plpFeat16k, rows),
                        takeRows(burstPulsesCol7, rows),
                        takeRows(intensity0To15, rows),
                        takeRows(energyCol0, rows)});
}

Matrix composeNetInFromAllFeat(const Matrix& allFeat) {
    const auto parts = splitAllFeat(allFeat);
    const Matrix plpWide = collectAverageWithZeroPadding(parts.plpcms,
        {{-12, 2}, {-6, 2}, {0, 2}, {6, 2}, {12, 2}});
    const Matrix burst = collectAverageWithZeroPadding(parts.burstPulse,
        {{-3, 1}, {-2, 1}, {-1, 1}, {0, 1}, {1, 1}, {2, 1}, {3, 1}});
    const Matrix intensity = collectAverageWithZeroPadding(parts.intensity,
        {{0, 1}});
    const Matrix energy = collectAverageWithZeroPadding(parts.energy,
        {{-8, 1}, {-4, 1}, {0, 1}, {4, 1}, {8, 1}});
    Matrix out = joinColumns({plpWide, burst, intensity, energy});
    if (out.cols != 158) {
        throw std::runtime_error("internal error: netin should have 158 columns");
    }
    return out;
}

MatrixCompareStats compareMatrixValues(const Matrix& actual, const Matrix& expected) {
    if (actual.rows != expected.rows || actual.cols != expected.cols) {
        std::ostringstream err;
        err << "matrix shape mismatch: actual " << actual.rows << "x" << actual.cols
            << ", expected " << expected.rows << "x" << expected.cols;
        throw std::runtime_error(err.str());
    }
    MatrixCompareStats s;
    s.rows = actual.rows;
    s.cols = actual.cols;
    long double sum = 0.0L;
    for (std::size_t r = 0; r < actual.rows; ++r) {
        for (std::size_t c = 0; c < actual.cols; ++c) {
            const double e = std::abs(actual(r, c) - expected(r, c));
            sum += e;
            if (e > s.maxAbsError) {
                s.maxAbsError = e;
                s.maxRow = r;
                s.maxCol = c;
            }
        }
    }
    if (!actual.data.empty()) {
        s.meanAbsError = static_cast<double>(sum / static_cast<long double>(actual.data.size()));
    }
    return s;
}

void ensureDirectory(const std::filesystem::path& dir) {
    if (dir.empty()) {
        return;
    }
    std::filesystem::create_directories(dir);
}

void writeNumericTsv(const Matrix& m, const std::filesystem::path& path, int precision) {
    ensureDirectory(path.parent_path());
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("cannot write TSV: " + path.string());
    }
    out << std::setprecision(precision) << std::fixed;
    for (std::size_t r = 0; r < m.rows; ++r) {
        for (std::size_t c = 0; c < m.cols; ++c) {
            if (c) {
                out << '\t';
            }
            out << m(r, c);
        }
        out << '\n';
    }
}

} // namespace cslu
