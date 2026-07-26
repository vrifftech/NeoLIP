#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cslu {

struct Matrix {
    std::size_t rows = 0;
    std::size_t cols = 0;
    std::vector<double> data;

    double operator()(std::size_t r, std::size_t c) const {
        return data.at(r * cols + c);
    }

    double& operator()(std::size_t r, std::size_t c) {
        return data.at(r * cols + c);
    }
};

struct MatrixStats {
    std::size_t rows = 0;
    std::size_t cols = 0;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
};

struct MetaMatrixInfo {
    std::string name;
    std::size_t rows = 0;
    std::size_t cols = 0;
    std::size_t dumped_rows = 0;
    std::size_t dumped_cols = 0;
    bool truncated_rows = false;
    bool truncated_cols = false;
};

std::string readTextFile(const std::filesystem::path& path);
std::vector<std::string> splitTab(const std::string& line);
std::string trimCR(std::string s);

Matrix readNumericTsv(const std::filesystem::path& path, std::optional<std::size_t> maxRows = std::nullopt);
std::map<std::string, std::string> readKeyValueTsv(const std::filesystem::path& path);
std::vector<std::string> readIndexValueList(const std::filesystem::path& path);
MetaMatrixInfo readMatrixMeta(const std::filesystem::path& metaPath);
MatrixStats stats(const Matrix& m);

std::filesystem::path tracePath(const std::filesystem::path& dumpDir, const std::string& logicalName, const std::string& suffix = ".tsv");
std::filesystem::path traceMetaPath(const std::filesystem::path& dumpDir, const std::string& logicalName);

void requireFile(const std::filesystem::path& path, const std::string& label);

} // namespace cslu
