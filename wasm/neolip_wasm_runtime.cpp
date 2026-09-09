#define NEOLIP_WASM_RUNTIME_NO_REMAP
#include "neolip_wasm_runtime.h"
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <wx/filefn.h>
namespace {
std::string Utf8(const wxString& value) {
    const wxScopedCharBuffer bytes = value.utf8_str();
    return bytes ? std::string(bytes.data()) : std::string{};
}
EM_JS(int, neo_lip_open_file, (const char* pathPtr, int audioOnly), {
    const path = UTF8ToString(pathPtr || 0);
    if (!path) return 0;
    let bytes;
    try { bytes = FS.readFile(path); } catch (error) {
        console.error('NeoLIP: unable to read browser file', path, error);
        return 0;
    }
    const extension = path.split('.').pop().toLowerCase();
    const mime = ({
        wav: 'audio/wav', mp3: 'audio/mpeg', ogg: 'audio/ogg', flac: 'audio/flac',
        txt: 'text/plain', json: 'application/json', xml: 'application/xml'
    })[extension] || 'application/octet-stream';
    const blob = new Blob([bytes.slice ? bytes.slice() : new Uint8Array(bytes)], { type: mime });
    const url = URL.createObjectURL(blob);

    if (audioOnly) {
        const previous = globalThis.NeoLipAudio;
        if (previous) {
            try { previous.audio.pause(); } catch (_) {}
            if (previous.url) URL.revokeObjectURL(previous.url);
        }
        const audio = new Audio(url);
        globalThis.NeoLipAudio = { audio, url };
        audio.addEventListener('ended', () => {
            if (globalThis.NeoLipAudio && globalThis.NeoLipAudio.audio === audio) {
                URL.revokeObjectURL(url);
                globalThis.NeoLipAudio = null;
            }
        }, { once: true });
        audio.play().catch(error => console.error('NeoLIP: browser audio playback failed', error));
        return 1;
    }

    const opened = window.open(url, '_blank', 'noopener');
    if (!opened) {
        const anchor = document.createElement('a');
        anchor.href = url;
        anchor.download = path.split('/').pop() || 'NeoLIP-file';
        anchor.style.display = 'none';
        document.body.appendChild(anchor);
        anchor.click();
        anchor.remove();
    }
    setTimeout(() => URL.revokeObjectURL(url), 60000);
    return 1;
});

EM_JS(void, neo_lip_stop_audio, (), {
    const current = globalThis.NeoLipAudio;
    if (!current) return;
    try {
        current.audio.pause();
        current.audio.currentTime = 0;
    } catch (_) {}
    if (current.url) URL.revokeObjectURL(current.url);
    globalThis.NeoLipAudio = null;
});

}
namespace neolip::wasm {
bool OpenFile(const wxString& path, int flags) {
    (void)flags;
    if (path.empty() || !wxFileExists(path)) return false;
    const std::string utf8 = Utf8(path);
    return neo_lip_open_file(utf8.c_str(), 0) != 0;
}

Sound::Sound(const wxString& fileName, bool isResource) { Create(fileName, isResource); }
bool Sound::Create(const wxString& fileName, bool isResource) {
    (void)isResource;
    path_ = fileName;
    return IsOk();
}
bool Sound::IsOk() const noexcept { return !path_.empty() && wxFileExists(path_); }
bool Sound::Play(unsigned flags) const {
    (void)flags;
    if (!IsOk()) return false;
    const std::string utf8 = Utf8(path_);
    return neo_lip_open_file(utf8.c_str(), 1) != 0;
}
void Sound::Stop() { neo_lip_stop_audio(); }

}
#endif
