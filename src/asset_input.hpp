#pragma once
#include <filesystem>
#include <fstream>
#include <istream>
#include <string_view>

namespace cslu {
class AssetInput final : public std::istream {
    class MemoryBuffer final : public std::streambuf {
    public:
        void reset(char* data, std::size_t size) { setg(data, data, data + size); }
    } memory_;
    std::filebuf file_;
public:
    explicit AssetInput(const std::filesystem::path& path);
};

#ifdef NEOLIP_EMBEDDED_ASSETS
bool isBundledAsset(const std::filesystem::path& path);
std::string_view bundledAssetFingerprint();
#endif
}
