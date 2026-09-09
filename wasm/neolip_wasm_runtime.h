#pragma once
#if defined(__EMSCRIPTEN__)
#include <wx/string.h>
#include <wx/sound.h>
#include <string>
namespace neolip::wasm {
bool OpenFile(const wxString& path, int flags = 0);
class Sound final {
public:
    Sound() = default;
    explicit Sound(const wxString& fileName, bool isResource = false);
    bool Create(const wxString& fileName, bool isResource = false);
    bool IsOk() const noexcept;
    bool Play(unsigned flags = wxSOUND_ASYNC) const;
    static void Stop();
private:
    wxString path_;
};
}
#ifndef NEOLIP_WASM_RUNTIME_NO_REMAP
#define wxLaunchDefaultApplication ::neolip::wasm::OpenFile
#define wxSound ::neolip::wasm::Sound
#endif
#endif
