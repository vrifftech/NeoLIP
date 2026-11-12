#include "work_control.hpp"
#include "nnet45.hpp"
#include "asset_input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace cslu {

static std::uint32_t readU32BE(std::istream& in) {
    std::array<unsigned char, 4> b{};
    in.read(reinterpret_cast<char*>(b.data()), 4);
    if (!in) {
        throw std::runtime_error("unexpected EOF while reading big-endian u32");
    }
    return (static_cast<std::uint32_t>(b[0]) << 24U) |
           (static_cast<std::uint32_t>(b[1]) << 16U) |
           (static_cast<std::uint32_t>(b[2]) << 8U) |
           static_cast<std::uint32_t>(b[3]);
}

static int readI32BE(std::istream& in) {
    return static_cast<int>(readU32BE(in));
}

static float readF32BE(std::istream& in) {
    std::uint32_t bits = readU32BE(in);
    float f = 0.0f;
    static_assert(sizeof(float) == 4, "float must be 32-bit");
    std::memcpy(&f, &bits, sizeof(float));
    return f;
}

static double sigmoid(double x) {
    if (x >= 0.0) {
        const double z = std::exp(-x);
        return 1.0 / (1.0 + z);
    }
    const double z = std::exp(x);
    return z / (1.0 + z);
}

NNet45 NNet45::load(const std::filesystem::path& path) {
    AssetInput in(path);
    if (!in) {
        throw std::runtime_error("cannot open .45 model: " + path.string());
    }

    NNet45 net;
    net.version = readI32BE(in);
    net.layers = readI32BE(in);
    net.inputCount = readI32BE(in);
    net.hiddenCount = readI32BE(in);
    net.outputCount = readI32BE(in);

    if (net.version != 45) {
        throw std::runtime_error("not a CSLU .45 model, version field is " + std::to_string(net.version));
    }
    if (net.layers != 3) {
        throw std::runtime_error("unsupported .45 layer count: " + std::to_string(net.layers));
    }
    if (net.inputCount <= 0 || net.hiddenCount <= 0 || net.outputCount <= 0) {
        throw std::runtime_error("invalid .45 dimensions");
    }

    net.wInputHidden.resize(static_cast<std::size_t>(net.inputCount) * static_cast<std::size_t>(net.hiddenCount));
    net.wHiddenOutput.resize(static_cast<std::size_t>(net.hiddenCount) * static_cast<std::size_t>(net.outputCount));

    for (float& v : net.wInputHidden) {
        v = readF32BE(in);
    }
    for (float& v : net.wHiddenOutput) {
        v = readF32BE(in);
    }

    char extra = 0;
    if (in.read(&extra, 1)) {
        throw std::runtime_error("extra bytes at end of .45 model: " + path.string());
    }

    return net;
}

std::size_t NNet45::expectedFloatCount() const {
    return static_cast<std::size_t>(inputCount) * static_cast<std::size_t>(hiddenCount) +
           static_cast<std::size_t>(hiddenCount) * static_cast<std::size_t>(outputCount);
}

std::vector<double> NNet45::forwardOne(const std::vector<double>& acousticInput) const {
    std::vector<double> x;
    if (acousticInput.size() == static_cast<std::size_t>(inputCount)) {
        x = acousticInput;
    } else if (acousticInput.size() + 1 == static_cast<std::size_t>(inputCount)) {
        x = acousticInput;
        x.push_back(1.0); // CSLU's dumped net inputs are 158 cols; .45 models expect 159 incl. bias.
    } else {
        std::ostringstream err;
        err << "network expected " << inputCount << " inputs, or " << (inputCount - 1)
            << " acoustic inputs plus implicit bias; got " << acousticInput.size();
        throw std::runtime_error(err.str());
    }

    std::vector<double> h(static_cast<std::size_t>(hiddenCount), 0.0);
    for (int j = 0; j < hiddenCount; ++j) {
        double sum = 0.0;
        const std::size_t row = static_cast<std::size_t>(j) * static_cast<std::size_t>(inputCount);
        for (int i = 0; i < inputCount; ++i) {
            if ((i & 63) == 0) checkWork();
            sum += static_cast<double>(wInputHidden[row + static_cast<std::size_t>(i)]) * x[static_cast<std::size_t>(i)];
        }
        h[static_cast<std::size_t>(j)] = sigmoid(sum);
    }

    std::vector<double> y(static_cast<std::size_t>(outputCount), 0.0);
    for (int k = 0; k < outputCount; ++k) {
        double sum = 0.0;
        const std::size_t row = static_cast<std::size_t>(k) * static_cast<std::size_t>(hiddenCount);
        for (int j = 0; j < hiddenCount; ++j) {
            sum += static_cast<double>(wHiddenOutput[row + static_cast<std::size_t>(j)]) * h[static_cast<std::size_t>(j)];
        }
        y[static_cast<std::size_t>(k)] = sigmoid(sum);
    }
    return y;
}

Matrix NNet45::forwardMatrix(const Matrix& acousticInputs) const {
    Matrix out;
    out.rows = acousticInputs.rows;
    out.cols = static_cast<std::size_t>(outputCount);
    out.data.resize(out.rows * out.cols);

    std::vector<double> row(acousticInputs.cols);
    for (std::size_t r = 0; r < acousticInputs.rows; ++r) {
        if ((r & 63) == 0) checkWork();
        for (std::size_t c = 0; c < acousticInputs.cols; ++c) {
            row[c] = acousticInputs(r, c);
        }
        auto y = forwardOne(row);
        for (std::size_t c = 0; c < out.cols; ++c) {
            out(r, c) = y[c];
        }
    }
    return out;
}

static std::size_t argmaxRow(const Matrix& m, std::size_t row) {
    std::size_t best = 0;
    double bestVal = m(row, 0);
    for (std::size_t c = 1; c < m.cols; ++c) {
        const double v = m(row, c);
        if (v > bestVal) {
            bestVal = v;
            best = c;
        }
    }
    return best;
}

NNetCompareStats compareMatrices(const Matrix& actual, const Matrix& expected) {
    if (actual.rows != expected.rows || actual.cols != expected.cols) {
        std::ostringstream err;
        err << "matrix dimension mismatch: actual " << actual.rows << "x" << actual.cols
            << ", expected " << expected.rows << "x" << expected.cols;
        throw std::runtime_error(err.str());
    }

    NNetCompareStats s;
    s.rows = actual.rows;
    s.cols = actual.cols;
    long double sumAbs = 0.0L;
    s.maxAbsError = -1.0;
    for (std::size_t r = 0; r < actual.rows; ++r) {
        if ((r & 63) == 0) checkWork();
        if (argmaxRow(actual, r) == argmaxRow(expected, r)) {
            ++s.top1Matches;
        }
        for (std::size_t c = 0; c < actual.cols; ++c) {
            const double e = std::abs(actual(r, c) - expected(r, c));
            sumAbs += e;
            if (e > s.maxAbsError) {
                s.maxAbsError = e;
                s.maxRow = r;
                s.maxCol = c;
            }
        }
    }
    s.meanAbsError = static_cast<double>(sumAbs / static_cast<long double>(actual.data.size()));
    if (s.maxAbsError < 0.0) {
        s.maxAbsError = 0.0;
    }
    return s;
}

} // namespace cslu
