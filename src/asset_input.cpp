#include "asset_input.hpp"
#ifdef NEOLIP_EMBEDDED_ASSETS
#include "embedded_asset_hash.hpp"
#include <windows.h>
#endif

namespace cslu {
#ifdef NEOLIP_EMBEDDED_ASSETS
bool isBundledAsset(const std::filesystem::path& path) {
    return !path.empty() && *path.begin() == "@neolip";
}
std::string_view bundledAssetFingerprint() { return NEOLIP_BUNDLED_ASSET_HASH; }
#endif

AssetInput::AssetInput(const std::filesystem::path& path) : std::istream(nullptr) {
#ifdef NEOLIP_EMBEDDED_ASSETS
    if (isBundledAsset(path)) {
        const auto module = GetModuleHandleW(nullptr);
        auto name = path.generic_wstring();
        for (auto& c : name) {
            if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
            else if (!((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'))) c = L'_';
        }
        const auto resource = FindResourceW(module, name.c_str(), MAKEINTRESOURCEW(10));
        auto* data = static_cast<char*>(LockResource(LoadResource(module, resource)));
        memory_.reset(data, SizeofResource(module, resource));
        rdbuf(&memory_);
        return;
    }
#endif
    rdbuf(file_.open(path, std::ios::in | std::ios::binary));
}
}
