#pragma once

#include <functional>
#include <thread>
#include <utility>

namespace neolip::wasm {

// Keeps the native backend's worker abstraction intact while avoiding pthreads
// on GitHub Pages, where cross-origin isolation cannot be assumed.
#if defined(__EMSCRIPTEN__)
class Thread final {
public:
    Thread() noexcept = default;

    template <class Function, class... Args>
    explicit Thread(Function&& function, Args&&... args) {
        std::invoke(std::forward<Function>(function), std::forward<Args>(args)...);
    }

    Thread(Thread&&) noexcept = default;
    Thread& operator=(Thread&&) noexcept = default;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    bool joinable() const noexcept { return false; }
    void join() noexcept {}
    void detach() noexcept {}
    static unsigned hardware_concurrency() noexcept { return 1U; }
};
#else
using Thread = std::thread;
#endif

}  // namespace neolip::wasm

#if defined(__EMSCRIPTEN__)

#include <wx/arrstr.h>
#include <wx/mstream.h>
#include <wx/process.h>
#include <wx/sound.h>
#include <wx/string.h>

#include <memory>
#include <string>

namespace neolip::wasm {

class Process : public wxEvtHandler {
public:
    explicit Process(wxEvtHandler* parent = nullptr, int id = wxID_ANY);
    ~Process() override = default;

    void Redirect();
    bool IsRedirected() const noexcept;
    bool IsInputAvailable() const;
    bool IsErrorAvailable() const;
    bool IsInputOpened() const;
    wxInputStream* GetInputStream() const;
    wxInputStream* GetErrorStream() const;
    wxOutputStream* GetOutputStream() const;
    void CloseOutput();
    void Detach();
    int GetPid() const noexcept;
    void OnTerminate(int pid, int status);

    void SetPid(int pid) noexcept;
    void SetCapturedOutput(std::string standardOutput, std::string standardError);
    std::weak_ptr<int> LifetimeToken() const noexcept;

    static bool Exists(int pid);
    static wxKillError Kill(int pid,
                            wxSignal signal = wxSIGTERM,
                            int flags = wxKILL_NOCHILDREN);

private:
    wxEvtHandler* parent_{nullptr};
    int eventId_{wxID_ANY};
    int pid_{0};
    bool redirected_{false};
    bool detached_{false};
    std::string standardOutput_;
    std::string standardError_;
    mutable std::unique_ptr<wxMemoryInputStream> input_;
    mutable std::unique_ptr<wxMemoryInputStream> error_;
    mutable std::unique_ptr<wxMemoryOutputStream> output_;
    std::shared_ptr<int> lifetime_{std::make_shared<int>(0)};
};

long Execute(const wxString& command,
             int flags = wxEXEC_ASYNC,
             Process* callback = nullptr);
long Execute(const wxArrayString& arguments,
             int flags = wxEXEC_ASYNC,
             Process* callback = nullptr);

// Preserve calls whose flags are enums or project typedefs.
template <class Flags>
long Execute(const wxString& command, Flags flags, Process* callback) {
    return Execute(command, static_cast<int>(flags), callback);
}

template <class Flags>
long Execute(const wxArrayString& arguments, Flags flags, Process* callback) {
    return Execute(arguments, static_cast<int>(flags), callback);
}

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

}  // namespace neolip::wasm

#ifndef NEOLIP_WASM_RUNTIME_NO_REMAP
#define wxProcess ::neolip::wasm::Process
#define wxExecute ::neolip::wasm::Execute
#define wxLaunchDefaultApplication ::neolip::wasm::OpenFile
#define wxSound ::neolip::wasm::Sound
#endif

#endif  // __EMSCRIPTEN__
