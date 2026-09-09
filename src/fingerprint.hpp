#pragma once
#include <string>
#include <filesystem>
namespace neolip {
std::string sha256(const std::string& bytes);
std::string fileFingerprint(const std::filesystem::path& file);
std::string treeFingerprint(const std::filesystem::path& root);
}
