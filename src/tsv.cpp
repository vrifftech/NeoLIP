#include "tsv.hpp"
#include "asset_input.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace cslu {

static std::size_t parseSize(const std::map<std::string, std::string>& kv, const std::string& key) {
    auto it = kv.find(key);
    if (it == kv.end()) {
        return 0;
    }
    return static_cast<std::size_t>(std::stoull(it->second));
}

std::string trimCR(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}

std::string readTextFile(const std::filesystem::path& path) {
    AssetInput in(path);
    if (!in) {
        throw std::runtime_error("cannot open file: " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> splitTab(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : line) {
        if (ch == '\t') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    out.push_back(cur);
    return out;
}

Matrix readNumericTsv(const std::filesystem::path& path, std::optional<std::size_t> maxRows) {
    AssetInput in(path);
    if (!in) {
        throw std::runtime_error("cannot open numeric TSV: " + path.string());
    }

    Matrix m;
    std::string line;
    std::size_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = trimCR(line);
        if (line.empty()) {
            continue;
        }
        if (maxRows && m.rows >= *maxRows) {
            break;
        }

        auto parts = splitTab(line);
        if (m.rows == 0) {
            m.cols = parts.size();
            if (m.cols == 0) {
                throw std::runtime_error("first nonempty row has zero columns: " + path.string());
            }
        } else if (parts.size() != m.cols) {
            std::ostringstream err;
            err << "ragged numeric TSV at " << path << ": line " << lineNo
                << " has " << parts.size() << " columns; expected " << m.cols;
            throw std::runtime_error(err.str());
        }

        for (const auto& p : parts) {
            try {
                m.data.push_back(std::stod(p));
            } catch (const std::exception&) {
                std::ostringstream err;
                err << "invalid number in " << path << " at line " << lineNo << ": '" << p << "'";
                throw std::runtime_error(err.str());
            }
        }
        ++m.rows;
    }
    return m;
}

std::map<std::string, std::string> readKeyValueTsv(const std::filesystem::path& path) {
    AssetInput in(path);
    if (!in) {
        throw std::runtime_error("cannot open key/value TSV: " + path.string());
    }

    std::map<std::string, std::string> kv;
    std::string line;
    std::size_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = trimCR(line);
        if (line.empty()) {
            continue;
        }
        auto parts = splitTab(line);
        if (parts.size() < 2) {
            continue;
        }
        if (lineNo == 1 && parts[0] == "key" && parts[1] == "value") {
            continue;
        }
        kv[parts[0]] = parts[1];
    }
    return kv;
}

std::vector<std::string> readIndexValueList(const std::filesystem::path& path) {
    AssetInput in(path);
    if (!in) {
        throw std::runtime_error("cannot open list TSV: " + path.string());
    }
    std::vector<std::string> values;
    std::string line;
    std::size_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = trimCR(line);
        if (line.empty()) {
            continue;
        }
        auto parts = splitTab(line);
        if (lineNo == 1 && parts.size() >= 2 && parts[0] == "index") {
            continue;
        }
        if (parts.size() >= 2) {
            values.push_back(parts[1]);
        }
    }
    return values;
}

MetaMatrixInfo readMatrixMeta(const std::filesystem::path& metaPath) {
    const auto kv = readKeyValueTsv(metaPath);
    MetaMatrixInfo info;
    if (auto it = kv.find("name"); it != kv.end()) {
        info.name = it->second;
    }
    info.rows = parseSize(kv, "rows");
    info.cols = parseSize(kv, "cols");
    info.dumped_rows = parseSize(kv, "dumped_rows");
    info.dumped_cols = parseSize(kv, "dumped_cols");
    info.truncated_rows = parseSize(kv, "truncated_rows") != 0;
    info.truncated_cols = parseSize(kv, "truncated_cols") != 0;
    return info;
}

MatrixStats stats(const Matrix& m) {
    MatrixStats s;
    s.rows = m.rows;
    s.cols = m.cols;
    if (m.data.empty()) {
        return s;
    }
    s.min = std::numeric_limits<double>::infinity();
    s.max = -std::numeric_limits<double>::infinity();
    long double sum = 0.0L;
    for (double v : m.data) {
        s.min = std::min(s.min, v);
        s.max = std::max(s.max, v);
        sum += v;
    }
    s.mean = static_cast<double>(sum / static_cast<long double>(m.data.size()));
    return s;
}

std::filesystem::path tracePath(const std::filesystem::path& dumpDir, const std::string& logicalName, const std::string& suffix) {
    return dumpDir / std::filesystem::path(logicalName + suffix);
}

std::filesystem::path traceMetaPath(const std::filesystem::path& dumpDir, const std::string& logicalName) {
    return tracePath(dumpDir, logicalName, ".meta.tsv");
}

void requireFile(const std::filesystem::path& path, const std::string& label) {
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("missing " + label + ": " + path.string());
    }
}

} // namespace cslu
