#if defined(__EMSCRIPTEN__)

#define NEOLIP_WASM_RUNTIME_NO_REMAP
#include <neolip_wasm_runtime.h>

#include <emscripten.h>

#include <wx/app.h>
#include <wx/filename.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <vector>

// The backend CLI entry point is compiled into the GUI through a generated
// wrapper translation unit in CMake.
extern int neolip_backend_main(int argc, char* argv[]);

namespace {

std::string Utf8(const wxString& value) {
    const wxScopedCharBuffer bytes = value.utf8_str();
    return bytes ? std::string(bytes.data()) : std::string();
}

std::vector<std::string> SplitCommandLine(const std::string& command) {
    std::vector<std::string> result;
    std::string current;
    char quote = '\0';
    bool escaped = false;

    for (char character : command) {
        if (escaped) {
            current.push_back(character);
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (quote != '\0') {
            if (character == quote) quote = '\0';
            else current.push_back(character);
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = character;
        } else if (std::isspace(static_cast<unsigned char>(character))) {
            if (!current.empty()) {
                result.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }
    if (escaped) current.push_back('\\');
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

std::vector<std::string> ToArguments(const wxArrayString& arguments) {
    std::vector<std::string> result;
    result.reserve(arguments.GetCount());
    for (const wxString& argument : arguments) result.push_back(Utf8(argument));
    return result;
}

bool LooksLikeBackend(const std::vector<std::string>& arguments) {
    if (arguments.empty()) return false;
    std::string executable = wxFileName(wxString::FromUTF8(arguments.front().c_str())).GetFullName().Lower().ToStdString();
    return executable.find("neolip_backend") != std::string::npos ||
           executable.find("neolip-backend") != std::string::npos;
}

int RunBackend(std::vector<std::string> arguments,
               std::string& standardOutput,
               std::string& standardError) {
    if (arguments.empty()) arguments.emplace_back("neolip_backend");

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);

    std::ostringstream output;
    std::ostringstream error;
    std::streambuf* oldOutput = std::cout.rdbuf(output.rdbuf());
    std::streambuf* oldError = std::cerr.rdbuf(error.rdbuf());
    int status = 1;
    try {
        status = neolip_backend_main(static_cast<int>(arguments.size()), argv.data());
    } catch (const std::exception& exception) {
        error << exception.what() << '\n';
        status = 1;
    } catch (...) {
        error << "Unknown NeoLIP backend error.\n";
        status = 1;
    }
    std::cout.rdbuf(oldOutput);
    std::cerr.rdbuf(oldError);
    standardOutput = std::move(output).str();
    standardError = std::move(error).str();
    return status;
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

std::atomic<int> NextPid{1000};

long ExecuteArguments(std::vector<std::string> arguments,
                      int flags,
                      neolip::wasm::Process* callback) {
    const bool backend = LooksLikeBackend(arguments) || callback != nullptr;
    if (!backend) {
        for (auto iterator = arguments.rbegin(); iterator != arguments.rend(); ++iterator) {
            const wxString candidate = wxString::FromUTF8(*iterator);
            if (wxFileExists(candidate)) return neolip::wasm::OpenFile(candidate) ? 1L : 0L;
        }
        return 0;
    }

    const int pid = ++NextPid;
    if (callback) callback->SetPid(pid);
    const std::weak_ptr<int> lifetime = callback
        ? callback->LifetimeToken()
        : std::weak_ptr<int>{};

    auto run = [arguments = std::move(arguments), callback, lifetime, pid]() mutable {
        if (callback && lifetime.expired()) return 1;
        std::string output;
        std::string error;
        const int status = RunBackend(std::move(arguments), output, error);
        if (callback && !lifetime.expired()) {
            callback->SetCapturedOutput(std::move(output), std::move(error));
            callback->OnTerminate(pid, status);
        } else if (!callback) {
            if (!output.empty()) std::cout << output;
            if (!error.empty()) std::cerr << error;
        }
        return status;
    };

    // A callback denotes the asynchronous wxExecute form. Defer until after the
    // originating DOM event returns so wxWidgets can finish its handler safely.
    if (callback && wxTheApp) {
        wxTheApp->CallAfter([run = std::move(run)]() mutable { run(); });
        return pid;
    }
    return run();
}

}  // namespace

namespace neolip::wasm {

Process::Process(wxEvtHandler* parent, int id) : parent_(parent), eventId_(id) {}
void Process::Redirect() { redirected_ = true; }
bool Process::IsRedirected() const noexcept { return redirected_; }
bool Process::IsInputAvailable() const { return input_ && input_->CanRead(); }
bool Process::IsErrorAvailable() const { return error_ && error_->CanRead(); }
bool Process::IsInputOpened() const { return input_ != nullptr; }
wxInputStream* Process::GetInputStream() const { return input_.get(); }
wxInputStream* Process::GetErrorStream() const { return error_.get(); }
wxOutputStream* Process::GetOutputStream() const {
    if (!output_) output_ = std::make_unique<wxMemoryOutputStream>();
    return output_.get();
}
void Process::CloseOutput() { output_.reset(); }
void Process::Detach() { detached_ = true; parent_ = nullptr; }
int Process::GetPid() const noexcept { return pid_; }
void Process::SetPid(int pid) noexcept { pid_ = pid; }
void Process::SetCapturedOutput(std::string standardOutput, std::string standardError) {
    standardOutput_ = std::move(standardOutput);
    standardError_ = std::move(standardError);
    input_ = std::make_unique<wxMemoryInputStream>(standardOutput_.data(), standardOutput_.size());
    error_ = std::make_unique<wxMemoryInputStream>(standardError_.data(), standardError_.size());
}
std::weak_ptr<int> Process::LifetimeToken() const noexcept { return lifetime_; }
void Process::OnTerminate(int pid, int status) {
    pid_ = pid;
    if (!detached_ && parent_) {
        wxQueueEvent(parent_, new wxProcessEvent(eventId_, pid, status));
    }
}
bool Process::Exists(int pid) { return pid > 0 && pid == NextPid.load(); }
wxKillError Process::Kill(int, wxSignal, int) { return wxKILL_NO_PROCESS; }

long Execute(const wxString& command, int flags, Process* callback) {
    return ExecuteArguments(SplitCommandLine(Utf8(command)), flags, callback);
}
long Execute(const wxArrayString& arguments, int flags, Process* callback) {
    return ExecuteArguments(ToArguments(arguments), flags, callback);
}

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

}  // namespace neolip::wasm

#endif  // __EMSCRIPTEN__
