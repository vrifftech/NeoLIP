#include "lip.hpp"
#include "phn.hpp"
#include "wav.hpp"
#include "NeoSettings.hpp"
#include "NeoViewState.hpp"
#include "NeoWxUi.hpp"
#include "NeoGameDirectoryMenu.hpp"

#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/collpane.h>
#include <wx/splitter.h>
#include <wx/filepicker.h>
#include <wx/stdpaths.h>
#ifndef __EMSCRIPTEN__
#include <wx/process.h>
#endif
#include <wx/timer.h>
#include <wx/filename.h>
#include <wx/fileconf.h>
#include <wx/sizer.h>
#include <wx/scrolwin.h>
#include <wx/textctrl.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/ffile.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/arrstr.h>
#include <wx/grid.h>
#ifndef __EMSCRIPTEN__
#include <wx/sound.h>
#endif
#include <wx/dcbuffer.h>

#include "neolip_icon.xpm"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <neoshared/wasm_dialog_compat.h>
#include <neolip_wasm_runtime.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr int ID_RunAlign = wxID_HIGHEST + 100;
constexpr int ID_RunLookup = wxID_HIGHEST + 102;
constexpr int ID_StopProcess = wxID_HIGHEST + 104;
constexpr int ID_ClearLog = wxID_HIGHEST + 105;
constexpr int ID_OpenWorkDir = wxID_HIGHEST + 107;
constexpr int ID_ProcessTimer = wxID_HIGHEST + 108;
constexpr int ID_RunBatch = wxID_HIGHEST + 109;
constexpr int ID_ScanUnknowns = wxID_HIGHEST + 110;
constexpr int ID_AppendDictStubs = wxID_HIGHEST + 111;
constexpr int ID_OpenUnknownReport = wxID_HIGHEST + 112;
constexpr int ID_OpenOverrideDict = wxID_HIGHEST + 113;
constexpr int ID_CopyLog = wxID_HIGHEST + 116;
constexpr int ID_SaveLog = wxID_HIGHEST + 117;
constexpr int ID_OpenOutputFolder = wxID_HIGHEST + 122;
constexpr int ID_LoadLipEditor = wxID_HIGHEST + 124;
constexpr int ID_SaveLipEditor = wxID_HIGHEST + 125;
constexpr int ID_InsertLipKey = wxID_HIGHEST + 126;
constexpr int ID_DeleteLipKeys = wxID_HIGHEST + 127;
constexpr int ID_ShiftLipKeys = wxID_HIGHEST + 128;
constexpr int ID_NormalizeLipKeys = wxID_HIGHEST + 129;
constexpr int ID_TimelinePlayCursor = wxID_HIGHEST + 134;
constexpr int ID_TimelineShapeChanged = wxID_HIGHEST + 135;
constexpr int ID_TimelineZoomChanged = wxID_HIGHEST + 136;
constexpr int ID_ToggleDarkMode = wxID_HIGHEST + 137;
constexpr int ID_FontIncrease = wxID_HIGHEST + 138;
constexpr int ID_FontDecrease = wxID_HIGHEST + 139;
constexpr int ID_FontReset = wxID_HIGHEST + 140;
constexpr int ID_ModelsSettings = wxID_HIGHEST + 141;
constexpr int ID_PronunciationDictionary = wxID_HIGHEST + 142;
constexpr int ID_SaveLipEditorAs = wxID_HIGHEST + 143;

void AppendChars(wxString& s, size_t count, wxUniChar ch) {
    for (size_t i = 0; i < count; ++i) s += ch;
}

wxString QuoteArg(const wxString& arg) {
    if (arg.empty()) return "\"\"";
    bool needsQuotes = false;
    for (wxUniChar ch : arg) {
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '"' || ch == '&' || ch == '(' || ch == ')' || ch == '^') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) return arg;

    wxString out = "\"";
    unsigned backslashes = 0;
    for (wxUniChar ch : arg) {
        if (ch == '\\') {
            ++backslashes;
        } else if (ch == '"') {
            AppendChars(out, backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
        } else {
            AppendChars(out, backslashes, '\\');
            backslashes = 0;
            out += ch;
        }
    }
    AppendChars(out, backslashes * 2, '\\');
    out += "\"";
    return out;
}

wxString JoinCommand(const wxArrayString& args) {
    wxString command;
    for (const auto& arg : args) {
        if (!command.empty()) command += ' ';
        command += QuoteArg(arg);
    }
    return command;
}

#ifndef __EMSCRIPTEN__
// NEOLIP_WXEXECUTE_ARGV_COMPAT_V1
// wxWidgets 3.2 exposes wxExecute() overloads for null-terminated argv
// arrays, but not the wxArrayString overload added by newer releases. Keep
// process launches shell-free and Unicode-safe by adapting wxArrayString to
// the wide-character argv overload available on both 3.2 and 3.3.
class ExecuteArguments final {
public:
    explicit ExecuteArguments(const wxArrayString& args) {
        storage_.reserve(args.size());
        for (const wxString& arg : args) storage_.push_back(arg.ToStdWstring());

        argv_.reserve(storage_.size() + 1);
        for (const std::wstring& arg : storage_) argv_.push_back(arg.c_str());
        argv_.push_back(nullptr);
    }

    const wchar_t* const* data() const noexcept { return argv_.data(); }

private:
    std::vector<std::wstring> storage_;
    std::vector<const wchar_t*> argv_;
};

long ExecuteCommand(const wxArrayString& args,
                    int flags,
                    wxProcess* process = nullptr) {
    if (args.empty()) return 0;
    const ExecuteArguments argv(args);
    return wxExecute(argv.data(), flags, process);
}
#else
long ExecuteCommand(const wxArrayString& args,
                    int flags,
                    wxProcess* process = nullptr) {
    if (args.empty()) return 0;
    return wxExecute(args, flags, process);
}
#endif

fs::path ToPath(const wxString& path) {
    return fs::u8path(wxui::toStd(path));
}

bool IsRegularFile(const wxString& path) {
    if (path.empty()) return false;
    std::error_code error;
    return fs::is_regular_file(ToPath(path), error);
}

wxString CreateTimelineTempFile(const wxString& prefix) {
    wxFileName tempPrefix(wxStandardPaths::Get().GetTempDir(), prefix);
    const wxString path = wxFileName::CreateTempFileName(tempPrefix.GetFullPath());
    if (path.empty()) throw std::runtime_error("could not create a secure temporary audio file");
    return path;
}

void RemoveFileBestEffort(const wxString& path) {
    if (IsRegularFile(path)) (void)wxRemoveFile(path);
}

void OpenExternalPath(const wxString& path, wxWindow* parent = nullptr) {
    (void)parent;
    if (path.empty()) return;
// Browser build uses the native route through neolip_wasm_runtime.
    wxLaunchDefaultApplication(path);
}

wxString DefaultToolPath() {
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
#ifdef __WXMSW__
    const wxString toolName = "neolip_backend.exe";
#else
    const wxString toolName = "neolip_backend";
#endif
    wxFileName candidate(exe.GetPath(), toolName);
    if (IsRegularFile(candidate.GetFullPath())) return candidate.GetFullPath();
    return toolName;
}


wxString ExistingFileFromCandidates(const wxArrayString& candidates) {
    for (const auto& c : candidates) {
        if (!c.empty() && IsRegularFile(c)) return c;
    }
    return wxEmptyString;
}

wxString DefaultKotorOverridePath(const wxString& toolPath, const wxString& modelRoot) {
    const wxString sep(wxFileName::GetPathSeparator());
    wxArrayString candidates;

    if (!modelRoot.empty()) {
        candidates.Add(modelRoot + sep + "dictionaries" + sep + "kotor_override.dict");
        wxFileName modelRootName(modelRoot, "");
        const wxString parent = modelRootName.GetPath();
        if (!parent.empty()) {
            candidates.Add(parent + sep + "dictionaries" + sep + "kotor_override.dict");
        }
    }

    if (!toolPath.empty()) {
        wxFileName tool(toolPath);
        const wxString binDir = tool.GetPath();
        if (!binDir.empty()) {
            candidates.Add(binDir + sep + "dictionaries" + sep + "kotor_override.dict");
            wxFileName fromAppResources(binDir + sep + ".." + sep + "Resources" + sep +
                                        "dictionaries" + sep + "kotor_override.dict");
            fromAppResources.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
            candidates.Add(fromAppResources.GetFullPath());
            wxFileName fromPackage(binDir + sep + ".." + sep + "dictionaries" + sep + "kotor_override.dict");
            fromPackage.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
            candidates.Add(fromPackage.GetFullPath());
        }
    }

    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
    wxFileName fromExePackage(exe.GetPath() + sep + ".." + sep + "dictionaries" + sep + "kotor_override.dict");
    fromExePackage.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
    candidates.Add(fromExePackage.GetFullPath());

    return ExistingFileFromCandidates(candidates);
}


wxString DefaultWorkDirForOutput(const wxString& outputPath) {
    wxFileName out(outputPath);
    if (!out.GetName().empty()) return wxFileName(out.GetPath(), out.GetName() + "_work").GetFullPath();
    return wxString();
}

void AddPickerRow(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, wxWindow* picker) {
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    grid->Add(picker, 1, wxEXPAND);
}

void AddTextRow(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, wxWindow* ctrl) {
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    grid->Add(ctrl, 1, wxEXPAND);
}

using GuiLipEntry = cslu::KotorVisibleKeyframe;
using GuiWaveform = cslu::DecodedWaveform;
using GuiPhnInterval = cslu::PhnInterval;

wxString KotorShapeName(int shape) {
    if (shape < 0 || shape > 15) return "out of range";
    return wxString::Format("%X: ", shape) +
           wxui::toWx(cslu::kotorLipShapeName(static_cast<std::uint8_t>(shape)));
}

std::vector<GuiLipEntry> ReadVisibleLipEntries(const wxString& path, double* durationOut) {
    return cslu::readKotorVisibleKeyframes(ToPath(path), durationOut);
}

void NormalizeVisibleLipEntries(std::vector<GuiLipEntry>& entries) {
    cslu::normalizeKotorVisibleKeyframes(entries);
}

void WriteVisibleLipEntries(const wxString& path, std::vector<GuiLipEntry> entries) {
    cslu::writeKotorVisibleKeyframes(ToPath(path), std::move(entries));
}

GuiWaveform ReadSimpleWaveform(const wxString& path) {
    return cslu::readMonoWaveform(ToPath(path));
}

void WritePcm16WavGui(const wxString& path, const GuiWaveform& waveform,
                      double startSeconds, double maxSeconds = 8.0) {
    cslu::writePcm16WaveformSegment(ToPath(path), waveform, startSeconds, maxSeconds);
}

std::vector<GuiPhnInterval> ReadGuiPhnIntervals(const wxString& path) {
    if (path.empty() || !IsRegularFile(path)) return {};
    return cslu::readPhn(ToPath(path)).intervals;
}

class LipTimelinePanel final : public wxScrolledWindow {
public:
    explicit LipTimelinePanel(wxWindow* parent)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 360), wxBORDER_SIMPLE | wxFULL_REPAINT_ON_RESIZE) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetScrollRate(20, 0);
        Bind(wxEVT_PAINT, &LipTimelinePanel::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &LipTimelinePanel::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP, &LipTimelinePanel::OnLeftUp, this);
        Bind(wxEVT_MOTION, &LipTimelinePanel::OnMotion, this);
        Bind(wxEVT_LEAVE_WINDOW, &LipTimelinePanel::OnLeftUp, this);
        Bind(wxEVT_LEFT_DCLICK, &LipTimelinePanel::OnDoubleClick, this);
        UpdateVirtualSize();
    }
    void SetData(GuiWaveform waveform, std::vector<GuiLipEntry> entries, std::vector<GuiPhnInterval> phn, double lipDuration) {
        waveform_ = std::move(waveform); entries_ = std::move(entries); phn_ = std::move(phn); lipDuration_ = lipDuration;
        duration_ = std::max({0.5, lipDuration_, waveform_.durationSeconds(), MaxPhnTime()});
        if (selected_ >= static_cast<int>(entries_.size())) selected_ = static_cast<int>(entries_.size()) - 1;
        if (selected_ < 0 && !entries_.empty()) selected_ = 0;
        UpdateVirtualSize(); Refresh(); NotifySelection();
    }
    const std::vector<GuiLipEntry>& Entries() const { return entries_; }
    const GuiWaveform& Waveform() const { return waveform_; }
    double CursorSeconds() const { return cursorSeconds_; }
    void SetZoom(int z) { zoom_ = std::clamp(z, 1, 20); UpdateVirtualSize(); Refresh(); }
    void SetSelectedShape(int shape) { if (selected_ >= 0 && selected_ < static_cast<int>(entries_.size())) { entries_[static_cast<std::size_t>(selected_)].shape = std::clamp(shape, 0, 15); Refresh(); NotifySelection(); } }
    void SetSelectionCallback(std::function<void(int,const GuiLipEntry*)> cb) { selectionCallback_ = std::move(cb); }
private:
    GuiWaveform waveform_;
    std::vector<GuiLipEntry> entries_;
    std::vector<GuiPhnInterval> phn_;
    double lipDuration_ = 0.0, duration_ = 1.0, cursorSeconds_ = 0.0;
    int zoom_ = 1, selected_ = -1;
    bool dragging_ = false;
    std::function<void(int,const GuiLipEntry*)> selectionCallback_;
    double MaxPhnTime() const { double t = 0.0; for (const auto& p : phn_) t = std::max(t, p.endMs / 1000.0); return t; }
    double PixelsPerSecond() const { return 160.0 * zoom_; }
    int Margin() const { return 55; }
    int XFromTime(double t) const { return Margin() + static_cast<int>(std::lround(t * PixelsPerSecond())); }
    double TimeFromX(int x) const { return std::max(0.0, static_cast<double>(x - Margin()) / PixelsPerSecond()); }
    void UpdateVirtualSize() { SetVirtualSize(std::max(900, XFromTime(duration_) + 80), 360); }
    int HitTestKey(int x) const { int best=-1, dist=10; for(int i=0;i<static_cast<int>(entries_.size());++i){ int d=std::abs(XFromTime(entries_[static_cast<std::size_t>(i)].timeSeconds)-x); if(d<dist){dist=d;best=i;}} return best; }
    void NotifySelection() { if (selectionCallback_) selectionCallback_(selected_, selected_>=0 && selected_<static_cast<int>(entries_.size()) ? &entries_[static_cast<std::size_t>(selected_)] : nullptr); }
    void OnPaint(wxPaintEvent&) {
        const auto palette = wxui::themePalette(wxui::currentDarkMode());
        wxAutoBufferedPaintDC dc(this); PrepareDC(dc); dc.SetBackground(wxBrush(palette.field)); dc.Clear();
        const int w = GetVirtualSize().GetWidth();
        const int waveMid=85, waveH=90, keyY=195, phnY=270, phnH=45;
        dc.SetTextForeground(palette.text);
        dc.SetPen(wxPen(palette.gridLine));
        for (double t=0; t<=duration_+0.001; t+=0.5) { int x=XFromTime(t); dc.DrawLine(x, 18, x, 330); dc.DrawText(wxString::Format("%.1fs", t), x+2, 4); }
        dc.DrawText("waveform", 6, 28); dc.DrawLine(0, waveMid, w, waveMid);
        if (!waveform_.mono.empty() && waveform_.sampleRate > 0) {
            dc.SetPen(wxPen(wxColour(70,110,180)));
            for (int x=Margin(); x<w; ++x) {
                double t0=TimeFromX(x), t1=TimeFromX(x+1);
                std::size_t a=std::min(waveform_.mono.size(), static_cast<std::size_t>(std::floor(t0*waveform_.sampleRate)));
                std::size_t b=std::min(waveform_.mono.size(), static_cast<std::size_t>(std::ceil(t1*waveform_.sampleRate)));
                float mn=0.0f, mx=0.0f; for(std::size_t i=a;i<b;++i){ mn=std::min(mn,waveform_.mono[i]); mx=std::max(mx,waveform_.mono[i]); }
                dc.DrawLine(x, waveMid-static_cast<int>(mx*waveH/2), x, waveMid-static_cast<int>(mn*waveH/2));
            }
        } else { dc.DrawText("load audio to display waveform", Margin()+15, waveMid-10); }
        dc.DrawText("mouth keys", 6, keyY-18); dc.SetPen(wxPen(palette.text)); dc.DrawLine(0, keyY, w, keyY);
        for (int i=0;i<static_cast<int>(entries_.size());++i) { const auto& e=entries_[static_cast<std::size_t>(i)]; int x=XFromTime(e.timeSeconds); wxPoint pts[4]={wxPoint(x,keyY-8),wxPoint(x+8,keyY),wxPoint(x,keyY+8),wxPoint(x-8,keyY)}; dc.SetBrush(wxBrush(i==selected_?wxColour(255,220,120):wxColour(120,180,120))); dc.SetPen(wxPen(palette.text)); dc.DrawPolygon(4,pts); if(i==selected_ || zoom_>=2) dc.DrawText(wxString::Format("%d",e.shape), x+10, keyY-20); }
        dc.DrawText("PHN", 6, phnY+12); dc.SetPen(wxPen(palette.gridLine)); dc.DrawRectangle(Margin(), phnY, w-Margin(), phnH);
        for (std::size_t i=0;i<phn_.size();++i) { const auto& p=phn_[i]; int x1=XFromTime(p.startMs/1000.0), x2=std::max(x1+1,XFromTime(p.endMs/1000.0)); dc.SetBrush(wxBrush(i%2?palette.fieldAlt:palette.field)); dc.DrawRectangle(x1,phnY,x2-x1,phnH); if(x2-x1>24) dc.DrawText(wxui::toWx(p.label),x1+2,phnY+12); }
        int cx=XFromTime(cursorSeconds_); dc.SetPen(wxPen(wxColour(200,40,40),2)); dc.DrawLine(cx, 18, cx, 330);
    }
    void OnLeftDown(wxMouseEvent& e) { int x=0,y=0; CalcUnscrolledPosition(e.GetX(), e.GetY(), &x, &y); cursorSeconds_=std::min(duration_,TimeFromX(x)); selected_=HitTestKey(x); dragging_=selected_>=0; if(dragging_) CaptureMouse(); NotifySelection(); Refresh(); }
    void OnMotion(wxMouseEvent& e) { if(!dragging_||selected_<0||selected_>=static_cast<int>(entries_.size())||!e.Dragging()||!e.LeftIsDown()) return; int x=0,y=0; CalcUnscrolledPosition(e.GetX(),e.GetY(),&x,&y); double t=std::min(duration_,TimeFromX(x)); entries_[static_cast<std::size_t>(selected_)].timeSeconds=t; cursorSeconds_=t; NotifySelection(); Refresh(); }
    void OnLeftUp(wxMouseEvent&) { if(dragging_ && HasCapture()) ReleaseMouse(); dragging_=false; }
    void OnDoubleClick(wxMouseEvent&) { if(selected_>=0 && selected_<static_cast<int>(entries_.size())) { entries_[static_cast<std::size_t>(selected_)].shape=(entries_[static_cast<std::size_t>(selected_)].shape+1)%16; NotifySelection(); Refresh(); } }
};

class CsluProcess final : public wxProcess {
public:
    explicit CsluProcess(wxEvtHandler* parent) : wxProcess(parent) { Redirect(); }
};

class MainFrame final : public wxFrame {
public:
    MainFrame()
        : wxFrame(nullptr, wxID_ANY, "NeoLIP", wxDefaultPosition, wxDefaultSize),
          timer_(this, ID_ProcessTimer) {
        SetIcon(wxIcon(neolip_icon_xpm));
        BuildMenus();
        BuildUi();
        wxui::configureResponsiveWindow(*this, wxSize(1140, 860), wxSize(720, 480));
        LoadConfig();
        wxui::constrainWindowToDisplay(*this);
        BindEvents();
        fontScaleWheelFilter_.attach(this, [this](int steps) { ChangeFontScaleSteps(steps); });
        neoview::bindFontScaleDpiRefresh(this, [this]() { ApplyFontScale(); });
        wxui::setStatusText(*this, "Ready");
    }

    ~MainFrame() override {
        wxSound::Stop();
        timelineSound_.reset();
        RemoveFileBestEffort(timelinePreviewPath_);
        SaveConfig();
#ifndef __EMSCRIPTEN__
        StopRunningProcess(false);
#endif
    }

private:
    wxNotebook* notebook_ = nullptr;
    wxTextCtrl* log_ = nullptr;
    wxGauge* progressGauge_ = nullptr;
    wxStaticText* progressLabel_ = nullptr;
    wxButton* stopButton_ = nullptr;
    wxButton* copyLogButton_ = nullptr;
    wxButton* saveLogButton_ = nullptr;
    wxButton* openWorkButton_ = nullptr;

    wxString modelsRoot_;
    wxMenuItem* darkModeMenuItem_ = nullptr;
    wxDialog* dictionaryDialog_ = nullptr;

    wxFilePickerCtrl* wavPicker_ = nullptr;
    wxFilePickerCtrl* textPicker_ = nullptr;
    wxTextCtrl* literalText_ = nullptr;
    wxFilePickerCtrl* lipOutputPicker_ = nullptr;
    wxDirPickerCtrl* workDirPicker_ = nullptr;
    wxChoice* alignMode_ = nullptr;
    wxFilePickerCtrl* userDictPicker_ = nullptr;
    wxCheckBox* useOgi_ = nullptr;
    wxCheckBox* writeReports_ = nullptr;
    wxComboBox* alignTailRest_ = nullptr;
    wxComboBox* alignClosePause_ = nullptr;
    wxChoice* alignSmoothing_ = nullptr;
    wxCheckBox* alignPreserveClosures_ = nullptr;
    wxComboBox* alignAnticipateClosures_ = nullptr;
    wxComboBox* alignAnticipateFricatives_ = nullptr;
    wxComboBox* alignAnticipateStops_ = nullptr;

    wxDirPickerCtrl* batchInputDirPicker_ = nullptr;
    wxDirPickerCtrl* batchOutputDirPicker_ = nullptr;
    wxChoice* batchMode_ = nullptr;
    wxFilePickerCtrl* batchUserDictPicker_ = nullptr;
    wxCheckBox* batchUseOgi_ = nullptr;
    wxCheckBox* batchRecursive_ = nullptr;
    wxCheckBox* batchWorkdirs_ = nullptr;
    wxCheckBox* batchFailFast_ = nullptr;
    wxCheckBox* batchResume_ = nullptr;
    wxTextCtrl* batchJobs_ = nullptr;
    wxDirPickerCtrl* batchDecodeCachePicker_ = nullptr;
    wxComboBox* batchTailRest_ = nullptr;
    wxComboBox* batchClosePause_ = nullptr;
    wxChoice* batchSmoothing_ = nullptr;
    wxCheckBox* batchPreserveClosures_ = nullptr;
    wxComboBox* batchAnticipateClosures_ = nullptr;
    wxComboBox* batchAnticipateFricatives_ = nullptr;
    wxComboBox* batchAnticipateStops_ = nullptr;

    wxDirPickerCtrl* dictInputDirPicker_ = nullptr;
    wxDirPickerCtrl* dictOutputDirPicker_ = nullptr;
    wxFilePickerCtrl* dictUnknownReportPicker_ = nullptr;
    wxFilePickerCtrl* dictSourceReportPicker_ = nullptr;
    wxFilePickerCtrl* dictOverrideDictPicker_ = nullptr;
    wxChoice* dictMode_ = nullptr;
    wxCheckBox* dictUseOgi_ = nullptr;
    wxCheckBox* dictRecursive_ = nullptr;
    wxTextCtrl* lookupWord_ = nullptr;
    wxFilePickerCtrl* lookupUserDictPicker_ = nullptr;
    wxCheckBox* lookupUseOgi_ = nullptr;

    wxFilePickerCtrl* timelineAudioPicker_ = nullptr;
    wxFilePickerCtrl* timelineLipPicker_ = nullptr;
    wxFilePickerCtrl* timelinePhnPicker_ = nullptr;
    wxFilePickerCtrl* timelineSavePicker_ = nullptr;
    LipTimelinePanel* timelinePanel_ = nullptr;
    wxStaticText* timelineStatus_ = nullptr;
    wxChoice* timelineShapeChoice_ = nullptr;
    wxSlider* timelineZoom_ = nullptr;
    wxStaticText* timelineSelection_ = nullptr;
    wxGrid* lipGrid_ = nullptr;
    wxStaticText* lipEditorStatus_ = nullptr;
    wxTextCtrl* lipShiftAmount_ = nullptr;
    std::vector<GuiLipEntry> lipEditorEntries_;
    std::vector<GuiLipEntry> timelineEntries_;
    GuiWaveform timelineWaveform_;
    double timelineLipDuration_ = 0.0;
    std::unique_ptr<wxSound> timelineSound_;
    wxString timelinePreviewPath_;
    bool synchronizingEditViews_ = false;
    bool lipEditorDirty_ = false;

    std::unique_ptr<CsluProcess> process_;
    long processPid_ = 0;
    wxString processCommand_;
    wxString lastOutputPath_;
    wxString lastWorkDir_;
    neosettings::AppSettings settings_{"neolip_gui"};
    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    double fontScale_ = neoview::kDefaultFontScale;
    wxTimer timer_;
    bool runningBatch_ = false;
    int progressTotal_ = 0;
    int progressValue_ = 0;

    wxComboBox* CreateTailRestCombo(wxWindow* parent) {
        wxArrayString choices;
        choices.Add("0");
        choices.Add("250");
        choices.Add("400");
        choices.Add("auto");
        auto* combo = new wxComboBox(parent, wxID_ANY, "0", wxDefaultPosition, wxSize(120, -1), choices, wxCB_DROPDOWN);
        combo->SetToolTip("Extra milliseconds added after the aligned dialogue before the final rest key. Use 0 for conservative output, 250/400 for stock-like tail padding, or auto.");
        return combo;
    }

    void AppendTailRestFlag(wxArrayString& args, wxComboBox* combo) const {
        if (!combo) return;
        wxString value = combo->GetValue();
        value.Trim(true).Trim(false);
        if (value.empty() || value == "0" || value == "0ms") return;
        wxString lower = value.Lower();
        if (lower == "auto") {
            args.Add("--tail-rest");
            args.Add("auto");
        } else {
            args.Add("--tail-rest-ms");
            args.Add(value);
        }
    }

    wxComboBox* CreateClosePauseCombo(wxWindow* parent) {
        wxArrayString choices;
        choices.Add("off");
        choices.Add("120");
        choices.Add("160");
        choices.Add("200");
        auto* combo = new wxComboBox(parent, wxID_ANY, "160", wxDefaultPosition, wxSize(120, -1), choices, wxCB_DROPDOWN);
        combo->SetToolTip("Close the mouth on silence/pause intervals at least this many milliseconds. Use off to disable, or type a custom value such as 120 or 200.");
        return combo;
    }

    wxChoice* CreateSmoothingChoice(wxWindow* parent) {
        auto* choice = new wxChoice(parent, wxID_ANY);
        choice->Append("none");
        choice->Append("light");
        choice->Append("medium");
        choice->Append("strong");
        choice->SetStringSelection("light");
        choice->SetToolTip("Minimum keyframe-spacing preset. Light uses about 40 ms and preserves visible closures; Medium/Strong smooth more aggressively.");
        return choice;
    }

    wxComboBox* CreateAnticipationCombo(wxWindow* parent, const wxString& defaultValue, const wxString& tooltip) {
        wxArrayString choices;
        choices.Add("0");
        choices.Add("10");
        choices.Add("20");
        choices.Add("25");
        choices.Add("30");
        choices.Add("40");
        choices.Add("50");
        auto* combo = new wxComboBox(parent, wxID_ANY, defaultValue, wxDefaultPosition, wxSize(90, -1), choices, wxCB_DROPDOWN);
        combo->SetToolTip(tooltip);
        return combo;
    }

    void AppendAnticipationFlag(wxArrayString& args, const wxString& flag, wxComboBox* combo, const wxString& defaultValue) const {
        if (!combo) return;
        wxString value = combo->GetValue();
        value.Trim(true).Trim(false);
        if (value.empty()) value = defaultValue;
        wxString lower = value.Lower();
        if (lower == "off" || lower == "none") value = "0";
        if (value != defaultValue) {
            args.Add(flag);
            args.Add(value);
        }
    }

    void AppendLipQualityFlags(wxArrayString& args, wxComboBox* closePause, wxChoice* smoothing, wxCheckBox* preserveClosures,
                               wxComboBox* anticipateClosures, wxComboBox* anticipateFricatives, wxComboBox* anticipateStops) const {
        if (closePause) {
            wxString value = closePause->GetValue();
            value.Trim(true).Trim(false);
            wxString lower = value.Lower();
            if (lower == "off" || lower == "none" || lower == "0" || lower == "0ms") {
                args.Add("--no-close-on-pause");
            } else if (!value.empty()) {
                args.Add("--close-on-pause-ms");
                args.Add(value);
            }
        }
        if (smoothing) {
            wxString value = smoothing->GetStringSelection();
            if (value.empty()) value = "light";
            args.Add("--smoothing");
            args.Add(value);
        }
        if (preserveClosures) {
            args.Add(preserveClosures->GetValue() ? "--preserve-closures" : "--no-preserve-closures");
        }
        AppendAnticipationFlag(args, "--anticipate-closures-ms", anticipateClosures, "40");
        AppendAnticipationFlag(args, "--anticipate-fricatives-ms", anticipateFricatives, "25");
        AppendAnticipationFlag(args, "--anticipate-stops-ms", anticipateStops, "20");
    }


    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;

    void BuildMenus() {
        auto* menuBar = new wxMenuBar();

        auto* file = new wxMenu();
        gameDirectoryMenu_ = neogames::appendOpenGameDirectoryMenu(
            *this, *file, [this](const std::filesystem::path& directory) {
                OpenLipFromGameDirectory(directory);
            });
        file->AppendSeparator();
        file->Append(wxID_EXIT, "E&xit");
        menuBar->Append(file, "&File");

        auto* settingsMenu = new wxMenu();
        settingsMenu->Append(ID_ModelsSettings, "&Models...");
        menuBar->Append(settingsMenu, "&Settings");

        auto* tools = new wxMenu();
        tools->Append(ID_PronunciationDictionary, "&Pronunciation Dictionary...");
        menuBar->Append(tools, "&Tools");

        auto* view = new wxMenu();
        darkModeMenuItem_ = view->AppendCheckItem(ID_ToggleDarkMode, "Dark mode");
        view->AppendSeparator();
        view->Append(ID_FontIncrease, "Increase Font Size\tCtrl++");
        view->Append(ID_FontDecrease, "Decrease Font Size\tCtrl+-");
        view->Append(ID_FontReset, "Reset Font Size\tCtrl+0");
        menuBar->Append(view, "&View");

        SetMenuBar(menuBar);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
    }

    void ApplyFontScale() {
        neoview::applyFontScale(this, fontScale_);
        if (dictionaryDialog_ != nullptr) neoview::applyFontScale(dictionaryDialog_, fontScale_);
    }

    void ChangeFontScaleSteps(int steps) {
        const double next = neoview::steppedFontScale(fontScale_, steps);
        if (neoview::fontScalePercent(next) == neoview::fontScalePercent(fontScale_)) return;
        fontScale_ = next;
        settings_.setFontScale(fontScale_);
        ApplyFontScale();
        wxui::setStatusText(*this, wxString::Format(
            "Font size: %d%%", neoview::fontScalePercent(fontScale_)));
    }

    void ApplyDarkMode(bool enabled) {
        if (darkModeMenuItem_ != nullptr) darkModeMenuItem_->Check(enabled);
        settings_.setDarkMode(enabled);
        wxui::applyTheme(this, enabled);
        if (dictionaryDialog_ != nullptr) wxui::applyTheme(dictionaryDialog_, enabled);
        ApplyFontScale();
        if (timelinePanel_ != nullptr) timelinePanel_->Refresh(false);
    }

    void BuildUi() {
        auto* root = new wxBoxSizer(wxVERTICAL);
        SetSizer(root);

        notebook_ = new wxNotebook(this, wxID_ANY);
        notebook_->AddPage(BuildCreatePage(notebook_), "Create", true);
        notebook_->AddPage(BuildEditPage(notebook_), "Edit", false);
        notebook_->AddPage(BuildBatchPage(notebook_), "Batch", false);
        root->Add(notebook_, 1, wxEXPAND | wxALL, 8);

        auto* logBox = new wxStaticBoxSizer(wxVERTICAL, this, "Activity log");
        log_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, 150),
                              wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxHSCROLL);
        logBox->Add(log_, 1, wxEXPAND | wxALL, 6);

        auto* progressRow = new wxBoxSizer(wxHORIZONTAL);
        progressLabel_ = new wxStaticText(this, wxID_ANY, "Idle");
        progressGauge_ = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, 18));
        progressGauge_->SetValue(0);
        progressRow->Add(progressLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        progressRow->Add(progressGauge_, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
        logBox->Add(progressRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

        auto* logButtons = new wxBoxSizer(wxHORIZONTAL);
        stopButton_ = new wxButton(this, ID_StopProcess, "Cancel");
        stopButton_->Enable(false);
        copyLogButton_ = new wxButton(this, ID_CopyLog, "Copy log");
        saveLogButton_ = new wxButton(this, ID_SaveLog, "Save log");
        openWorkButton_ = new wxButton(this, ID_OpenWorkDir, "Open output/work folder");
        auto* clearButton = new wxButton(this, ID_ClearLog, "Clear log");
#ifndef __EMSCRIPTEN__
        logButtons->Add(stopButton_, 0, wxRIGHT, 6);
#endif
        logButtons->Add(copyLogButton_, 0, wxRIGHT, 6);
        logButtons->Add(saveLogButton_, 0, wxRIGHT, 6);
#ifndef __EMSCRIPTEN__
        logButtons->Add(openWorkButton_, 0, wxRIGHT, 6);
#endif
        logButtons->AddStretchSpacer(1);
        logButtons->Add(clearButton, 0);
        logBox->Add(logButtons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
        root->Add(logBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        BuildDictionaryDialog();
        wxui::createStatusBar(*this);
    }

    wxPanel* BuildCreatePage(wxWindow* parent) {
        auto* panel = new wxScrolledWindow(
            parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        panel->SetScrollRate(0, FromDIP(10));
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);

        outer->Add(new wxStaticText(panel, wxID_ANY,
            "Create a KotOR LIP from one dialogue WAV and its spoken transcript."),
            0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        auto* grid = new wxFlexGridSizer(2, 8, 8);
        grid->AddGrowableCol(1, 1);
        wavPicker_ = new wxFilePickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Select dialogue WAV",
            "WAV files (*.wav)|*.wav|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize,
            wxFLP_USE_TEXTCTRL | wxFLP_OPEN | wxFLP_FILE_MUST_EXIST);
        textPicker_ = new wxFilePickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Select transcript",
            "Transcript files (*.txt;*.lab)|*.txt;*.lab|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize,
            wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        literalText_ = new wxTextCtrl(
            panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, 72),
            wxTE_MULTILINE | wxTE_WORDWRAP);
        lipOutputPicker_ = new wxFilePickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Choose output LIP",
            "KotOR LIP files (*.lip)|*.lip|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize,
            wxFLP_USE_TEXTCTRL | wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT);

        AddPickerRow(panel, grid, "Audio WAV", wavPicker_);
        AddPickerRow(panel, grid, "Transcript file", textPicker_);
        AddTextRow(panel, grid, "Transcript text", literalText_);
        AddPickerRow(panel, grid, "Output LIP", lipOutputPicker_);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);

        auto* advanced = new wxCollapsiblePane(panel, wxID_ANY, "Advanced options");
        auto* advancedPane = advanced->GetPane();
        auto* advancedGrid = new wxFlexGridSizer(2, 6, 6);
        advancedGrid->AddGrowableCol(1, 1);
        advancedPane->SetSizer(advancedGrid);

        workDirPicker_ = new wxDirPickerCtrl(
            advancedPane, wxID_ANY, wxEmptyString, "Choose optional work directory",
            wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
        alignMode_ = new wxChoice(advancedPane, wxID_ANY);
        alignMode_->Append("simple");
        alignMode_->Append("cslu");
        alignMode_->SetSelection(0);
        userDictPicker_ = new wxFilePickerCtrl(
            advancedPane, wxID_ANY, wxEmptyString, "Select optional pronunciation dictionary",
            "Dictionary files (*.dict;*.txt)|*.dict;*.txt|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize,
            wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        useOgi_ = new wxCheckBox(advancedPane, wxID_ANY, "Use OGI/Festival lexicon fallback");
        useOgi_->SetValue(true);
        writeReports_ = new wxCheckBox(advancedPane, wxID_ANY, "Write pronunciation and unknown-word reports");
        writeReports_->SetValue(true);
        alignTailRest_ = CreateTailRestCombo(advancedPane);
        alignClosePause_ = CreateClosePauseCombo(advancedPane);
        alignSmoothing_ = CreateSmoothingChoice(advancedPane);
        alignPreserveClosures_ = new wxCheckBox(advancedPane, wxID_ANY, "Preserve closures");
        alignPreserveClosures_->SetValue(true);
        alignAnticipateClosures_ = CreateAnticipationCombo(advancedPane, "40", "Start M/P/B closure keys this many milliseconds earlier.");
        alignAnticipateFricatives_ = CreateAnticipationCombo(advancedPane, "25", "Start F/V and TH fricative keys this many milliseconds earlier.");
        alignAnticipateStops_ = CreateAnticipationCombo(advancedPane, "20", "Start T/D and K/G stop keys this many milliseconds earlier.");

        AddPickerRow(advancedPane, advancedGrid, "Work folder", workDirPicker_);
        advancedGrid->Add(new wxStaticText(advancedPane, wxID_ANY, "Pronunciation mode"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        advancedGrid->Add(alignMode_, 0, wxALIGN_LEFT);
        AddPickerRow(advancedPane, advancedGrid, "Dictionary", userDictPicker_);
        advancedGrid->Add(new wxStaticText(advancedPane, wxID_ANY, "Tail rest ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        advancedGrid->Add(alignTailRest_, 0, wxALIGN_LEFT);
        advancedGrid->Add(new wxStaticText(advancedPane, wxID_ANY, "Close on pause ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        advancedGrid->Add(alignClosePause_, 0, wxALIGN_LEFT);
        advancedGrid->Add(new wxStaticText(advancedPane, wxID_ANY, "Mouth smoothing"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        auto* smoothingRow = new wxBoxSizer(wxHORIZONTAL);
        smoothingRow->Add(alignSmoothing_, 0, wxRIGHT, 8);
        smoothingRow->Add(alignPreserveClosures_, 0, wxALIGN_CENTER_VERTICAL);
        advancedGrid->Add(smoothingRow, 0, wxEXPAND);
        advancedGrid->Add(new wxStaticText(advancedPane, wxID_ANY, "Consonant anticipation ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        auto* anticipationRow = new wxBoxSizer(wxHORIZONTAL);
        anticipationRow->Add(new wxStaticText(advancedPane, wxID_ANY, "M/P/B"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        anticipationRow->Add(alignAnticipateClosures_, 0, wxRIGHT, 10);
        anticipationRow->Add(new wxStaticText(advancedPane, wxID_ANY, "F/V/TH"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        anticipationRow->Add(alignAnticipateFricatives_, 0, wxRIGHT, 10);
        anticipationRow->Add(new wxStaticText(advancedPane, wxID_ANY, "T/D/K/G"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        anticipationRow->Add(alignAnticipateStops_, 0);
        advancedGrid->Add(anticipationRow, 0, wxEXPAND);
        advancedGrid->AddSpacer(1);
        auto* checks = new wxBoxSizer(wxVERTICAL);
        checks->Add(useOgi_, 0, wxBOTTOM, 4);
        checks->Add(writeReports_, 0);
        advancedGrid->Add(checks, 0, wxEXPAND);
        outer->Add(advanced, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        advanced->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED,
                       [panel](wxCollapsiblePaneEvent& event) {
                           panel->Layout();
                           panel->FitInside();
                           event.Skip();
                       });

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(panel, ID_RunAlign, "Create LIP"), 0, wxRIGHT, 8);
#ifndef __EMSCRIPTEN__
        buttons->Add(new wxButton(panel, ID_OpenOutputFolder, "Open output folder"), 0, wxRIGHT, 8);
#endif
        buttons->Add(new wxStaticText(panel, wxID_ANY,
            "When Output LIP is blank, NeoLIP uses the WAV name in the same folder."),
            0, wxALIGN_CENTER_VERTICAL);
        buttons->AddStretchSpacer(1);
        outer->Add(buttons, 0, wxEXPAND | wxALL, 10);
        panel->FitInside();
        return panel;
    }

    wxPanel* BuildEditPage(wxWindow* parent) {
        auto* panel = new wxPanel(parent);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);

        auto* grid = new wxFlexGridSizer(2, 6, 6);
        grid->AddGrowableCol(1, 1);
        timelineAudioPicker_ = new wxFilePickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Select source audio",
            "Audio files (*.wav;*.mp3)|*.wav;*.mp3|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        timelineLipPicker_ = new wxFilePickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Open KotOR LIP file",
            "KotOR LIP files (*.lip)|*.lip|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize,
            wxFLP_USE_TEXTCTRL | wxFLP_OPEN | wxFLP_FILE_MUST_EXIST);
        timelinePhnPicker_ = new wxFilePickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Optional PHN alignment",
            "PHN files (*.phn)|*.phn|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        timelineSavePicker_ = new wxFilePickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Save edited KotOR LIP",
            "KotOR LIP files (*.lip)|*.lip|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize,
            wxFLP_USE_TEXTCTRL | wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT);
        AddPickerRow(panel, grid, "Audio", timelineAudioPicker_);
        AddPickerRow(panel, grid, "LIP", timelineLipPicker_);
        AddPickerRow(panel, grid, "PHN overlay", timelinePhnPicker_);
        AddPickerRow(panel, grid, "Save as", timelineSavePicker_);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);

        auto* controls = new wxBoxSizer(wxHORIZONTAL);
        controls->Add(new wxButton(panel, ID_LoadLipEditor, "Open LIP"), 0, wxRIGHT, 8);
        controls->Add(new wxButton(panel, ID_SaveLipEditor, "Save LIP"), 0, wxRIGHT, 8);
        controls->Add(new wxButton(panel, ID_SaveLipEditorAs, "Save As..."), 0, wxRIGHT, 8);
        controls->Add(new wxButton(panel, ID_TimelinePlayCursor, "Play from cursor"), 0, wxRIGHT, 8);
        controls->Add(new wxButton(panel, ID_NormalizeLipKeys, "Normalize final rest"), 0, wxRIGHT, 12);
        controls->Add(new wxStaticText(panel, wxID_ANY, "Shape"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        timelineShapeChoice_ = new wxChoice(panel, ID_TimelineShapeChanged);
        for (int i = 0; i < 16; ++i) timelineShapeChoice_->Append(KotorShapeName(i));
        timelineShapeChoice_->SetSelection(0);
        controls->Add(timelineShapeChoice_, 0, wxRIGHT, 12);
        controls->Add(new wxStaticText(panel, wxID_ANY, "Zoom"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        timelineZoom_ = new wxSlider(panel, ID_TimelineZoomChanged, 1, 1, 20,
                                     wxDefaultPosition, wxSize(150, -1), wxSL_HORIZONTAL);
        controls->Add(timelineZoom_, 0, wxRIGHT, 12);
        controls->AddStretchSpacer(1);
        outer->Add(controls, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        timelineSelection_ = new wxStaticText(panel, wxID_ANY, "No key selected.");
        timelineStatus_ = new wxStaticText(panel, wxID_ANY,
            "Open a LIP. Audio and PHN are optional synchronized overlays.");
        outer->Add(timelineSelection_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
        outer->Add(timelineStatus_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        auto* splitter = new wxSplitterWindow(
            panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxSP_LIVE_UPDATE | wxSP_3D);
        splitter->SetMinimumPaneSize(160);
        splitter->SetSashGravity(0.56);

        auto* timelinePane = new wxPanel(splitter);
        auto* timelineSizer = new wxBoxSizer(wxVERTICAL);
        timelinePane->SetSizer(timelineSizer);
        timelinePanel_ = new LipTimelinePanel(timelinePane);
        timelinePanel_->SetSelectionCallback([this](int idx, const GuiLipEntry* entry) {
            if (entry != nullptr) {
                timelineSelection_->SetLabel(wxString::Format(
                    "selected key %d   time %.3fs   shape %s",
                    idx, entry->timeSeconds, KotorShapeName(entry->shape)));
                if (timelineShapeChoice_ != nullptr) {
                    timelineShapeChoice_->SetSelection(entry->shape);
                }
                if (!synchronizingEditViews_ && idx >= 0 &&
                    idx < static_cast<int>(timelineEntries_.size()) &&
                    idx < static_cast<int>(lipEditorEntries_.size())) {
                    const GuiLipEntry previous =
                        lipEditorEntries_[static_cast<std::size_t>(idx)];
                    timelineEntries_[static_cast<std::size_t>(idx)] = *entry;
                    lipEditorEntries_[static_cast<std::size_t>(idx)] = *entry;
                    if (previous.timeSeconds != entry->timeSeconds ||
                        previous.shape != entry->shape) {
                        lipEditorDirty_ = true;
                    }
                    synchronizingEditViews_ = true;
                    UpdateLipGridRow(idx);
                    synchronizingEditViews_ = false;
                }
            } else {
                timelineSelection_->SetLabel("No key selected.");
            }
            UpdateTimelineStatus();
            UpdateLipEditorStatus();
        });
        timelineSizer->Add(timelinePanel_, 1, wxEXPAND);

        auto* tablePane = new wxPanel(splitter);
        auto* tableSizer = new wxBoxSizer(wxVERTICAL);
        tablePane->SetSizer(tableSizer);
        auto* tableControls = new wxBoxSizer(wxHORIZONTAL);
        tableControls->Add(new wxButton(tablePane, ID_InsertLipKey, "Insert key"), 0, wxRIGHT, 8);
        tableControls->Add(new wxButton(tablePane, ID_DeleteLipKeys, "Delete selected"), 0, wxRIGHT, 8);
        lipShiftAmount_ = new wxTextCtrl(tablePane, wxID_ANY, "0.050", wxDefaultPosition, wxSize(80, -1));
        tableControls->Add(new wxStaticText(tablePane, wxID_ANY, "Shift seconds"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        tableControls->Add(lipShiftAmount_, 0, wxRIGHT, 8);
        tableControls->Add(new wxButton(tablePane, ID_ShiftLipKeys, "Shift selected"), 0, wxRIGHT, 8);
        tableControls->AddStretchSpacer(1);
        tableSizer->Add(tableControls, 0, wxEXPAND | wxALL, 6);
        lipEditorStatus_ = new wxStaticText(tablePane, wxID_ANY, "No visible mouth keys loaded.");
        tableSizer->Add(lipEditorStatus_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
        lipGrid_ = new wxGrid(tablePane, wxID_ANY);
        lipGrid_->CreateGrid(0, 4);
        wxui::configureStableGridRendering(*lipGrid_);
        lipGrid_->SetColLabelValue(0, "Entry");
        lipGrid_->SetColLabelValue(1, "Time (s)");
        lipGrid_->SetColLabelValue(2, "Shape");
        lipGrid_->SetColLabelValue(3, "Shape name");
        lipGrid_->SetColSize(0, 70);
        lipGrid_->SetColSize(1, 120);
        lipGrid_->SetColSize(2, 80);
        lipGrid_->SetColSize(3, 220);
        lipGrid_->EnableEditing(true);
        lipGrid_->SetSelectionMode(wxGrid::wxGridSelectRows);
        tableSizer->Add(lipGrid_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

        splitter->SplitHorizontally(timelinePane, tablePane, 330);
        outer->Add(splitter, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        return panel;
    }

    wxPanel* BuildBatchPage(wxWindow* parent) {
        auto* panel = new wxScrolledWindow(
            parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        panel->SetScrollRate(0, FromDIP(10));
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);

        outer->Add(new wxStaticText(panel, wxID_ANY,
            "Process a folder of same-name WAV and TXT/LAB pairs into KotOR LIP files."),
            0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        auto* grid = new wxFlexGridSizer(2, 8, 8);
        grid->AddGrowableCol(1, 1);
        batchInputDirPicker_ = new wxDirPickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Select folder containing WAV/TXT pairs",
            wxDefaultPosition, wxDefaultSize,
            wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
        batchOutputDirPicker_ = new wxDirPickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Select output folder",
            wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
        AddPickerRow(panel, grid, "Input folder", batchInputDirPicker_);
        AddPickerRow(panel, grid, "Output folder", batchOutputDirPicker_);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);

        auto* advanced = new wxCollapsiblePane(panel, wxID_ANY, "Advanced options");
        auto* advancedPane = advanced->GetPane();
        auto* advancedGrid = new wxFlexGridSizer(2, 6, 6);
        advancedGrid->AddGrowableCol(1, 1);
        advancedPane->SetSizer(advancedGrid);

        batchMode_ = new wxChoice(advancedPane, wxID_ANY);
        batchMode_->Append("simple");
        batchMode_->Append("cslu");
        batchMode_->SetSelection(0);
        batchUserDictPicker_ = new wxFilePickerCtrl(
            advancedPane, wxID_ANY, wxEmptyString, "Select optional pronunciation dictionary",
            "Dictionary files (*.dict;*.txt)|*.dict;*.txt|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize,
            wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        batchUseOgi_ = new wxCheckBox(advancedPane, wxID_ANY, "Use OGI/Festival lexicon fallback");
        batchUseOgi_->SetValue(true);
        batchRecursive_ = new wxCheckBox(advancedPane, wxID_ANY, "Scan subfolders recursively");
        batchWorkdirs_ = new wxCheckBox(advancedPane, wxID_ANY, "Keep per-line work folders");
        batchWorkdirs_->SetValue(true);
        batchFailFast_ = new wxCheckBox(advancedPane, wxID_ANY, "Stop at first failed file");
        batchResume_ = new wxCheckBox(advancedPane, wxID_ANY, "Skip valid existing outputs");
        batchJobs_ = new wxTextCtrl(advancedPane, wxID_ANY, "auto");
        batchDecodeCachePicker_ = new wxDirPickerCtrl(
            advancedPane, wxID_ANY, wxEmptyString, "Optional decoded-audio cache",
            wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
        batchTailRest_ = CreateTailRestCombo(advancedPane);
        batchClosePause_ = CreateClosePauseCombo(advancedPane);
        batchSmoothing_ = CreateSmoothingChoice(advancedPane);
        batchPreserveClosures_ = new wxCheckBox(advancedPane, wxID_ANY, "Preserve closures");
        batchPreserveClosures_->SetValue(true);
        batchAnticipateClosures_ = CreateAnticipationCombo(advancedPane, "40", "Start M/P/B closure keys this many milliseconds earlier.");
        batchAnticipateFricatives_ = CreateAnticipationCombo(advancedPane, "25", "Start F/V and TH fricative keys this many milliseconds earlier.");
        batchAnticipateStops_ = CreateAnticipationCombo(advancedPane, "20", "Start T/D and K/G stop keys this many milliseconds earlier.");

        advancedGrid->Add(new wxStaticText(advancedPane, wxID_ANY, "Pronunciation mode"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        advancedGrid->Add(batchMode_, 0, wxALIGN_LEFT);
        AddPickerRow(advancedPane, advancedGrid, "Dictionary", batchUserDictPicker_);
        advancedGrid->Add(new wxStaticText(advancedPane, wxID_ANY, "Parallel jobs"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        advancedGrid->Add(batchJobs_, 0, wxEXPAND);
        AddPickerRow(advancedPane, advancedGrid, "Decoded audio cache", batchDecodeCachePicker_);
        advancedGrid->Add(new wxStaticText(advancedPane, wxID_ANY, "Tail rest ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        advancedGrid->Add(batchTailRest_, 0, wxALIGN_LEFT);
        advancedGrid->Add(new wxStaticText(advancedPane, wxID_ANY, "Close on pause ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        advancedGrid->Add(batchClosePause_, 0, wxALIGN_LEFT);
        advancedGrid->Add(new wxStaticText(advancedPane, wxID_ANY, "Mouth smoothing"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        auto* smoothingRow = new wxBoxSizer(wxHORIZONTAL);
        smoothingRow->Add(batchSmoothing_, 0, wxRIGHT, 8);
        smoothingRow->Add(batchPreserveClosures_, 0, wxALIGN_CENTER_VERTICAL);
        advancedGrid->Add(smoothingRow, 0, wxEXPAND);
        advancedGrid->Add(new wxStaticText(advancedPane, wxID_ANY, "Consonant anticipation ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        auto* anticipationRow = new wxBoxSizer(wxHORIZONTAL);
        anticipationRow->Add(new wxStaticText(advancedPane, wxID_ANY, "M/P/B"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        anticipationRow->Add(batchAnticipateClosures_, 0, wxRIGHT, 10);
        anticipationRow->Add(new wxStaticText(advancedPane, wxID_ANY, "F/V/TH"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        anticipationRow->Add(batchAnticipateFricatives_, 0, wxRIGHT, 10);
        anticipationRow->Add(new wxStaticText(advancedPane, wxID_ANY, "T/D/K/G"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        anticipationRow->Add(batchAnticipateStops_, 0);
        advancedGrid->Add(anticipationRow, 0, wxEXPAND);
        advancedGrid->AddSpacer(1);
        auto* checks = new wxBoxSizer(wxVERTICAL);
        checks->Add(batchUseOgi_, 0, wxBOTTOM, 4);
        checks->Add(batchRecursive_, 0, wxBOTTOM, 4);
        checks->Add(batchWorkdirs_, 0, wxBOTTOM, 4);
        checks->Add(batchFailFast_, 0, wxBOTTOM, 4);
        checks->Add(batchResume_, 0);
        advancedGrid->Add(checks, 0, wxEXPAND);
        outer->Add(advanced, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        advanced->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED,
                       [panel](wxCollapsiblePaneEvent& event) {
                           panel->Layout();
                           panel->FitInside();
                           event.Skip();
                       });

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(panel, ID_RunBatch, "Create LIP files"), 0, wxRIGHT, 8);
        buttons->Add(new wxStaticText(panel, wxID_ANY,
            "Writes batch_report.tsv and unknown_words.tsv in the output folder."),
            0, wxALIGN_CENTER_VERTICAL);
        buttons->AddStretchSpacer(1);
        outer->Add(buttons, 0, wxEXPAND | wxALL, 10);
        panel->FitInside();
        return panel;
    }

    wxPanel* BuildDictionaryWorkflowPage(wxWindow* parent) {
        auto* panel = new wxPanel(parent);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);
        auto* grid = new wxFlexGridSizer(2, 6, 6);
        grid->AddGrowableCol(1, 1);
        dictInputDirPicker_ = new wxDirPickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Select folder containing transcript files",
            wxDefaultPosition, wxDefaultSize,
            wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
        dictOutputDirPicker_ = new wxDirPickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Select report folder",
            wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
        dictUnknownReportPicker_ = new wxFilePickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Choose unknown_words.tsv",
            "TSV files (*.tsv)|*.tsv|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize,
            wxFLP_USE_TEXTCTRL | wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT);
        dictSourceReportPicker_ = new wxFilePickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Choose pronunciation-source report",
            "TSV files (*.tsv)|*.tsv|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize,
            wxFLP_USE_TEXTCTRL | wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT);
        dictOverrideDictPicker_ = new wxFilePickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Choose pronunciation dictionary",
            "Dictionary files (*.dict;*.txt)|*.dict;*.txt|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize,
            wxFLP_USE_TEXTCTRL | wxFLP_SAVE);
        dictMode_ = new wxChoice(panel, wxID_ANY);
        dictMode_->Append("simple");
        dictMode_->Append("cslu");
        dictMode_->SetSelection(0);
        dictUseOgi_ = new wxCheckBox(panel, wxID_ANY, "Use OGI/Festival lexicon fallback");
        dictUseOgi_->SetValue(true);
        dictRecursive_ = new wxCheckBox(panel, wxID_ANY, "Scan subfolders recursively");

        AddPickerRow(panel, grid, "Transcript folder", dictInputDirPicker_);
        AddPickerRow(panel, grid, "Report folder", dictOutputDirPicker_);
        AddPickerRow(panel, grid, "Unknown report", dictUnknownReportPicker_);
        AddPickerRow(panel, grid, "Source report", dictSourceReportPicker_);
        AddPickerRow(panel, grid, "Dictionary", dictOverrideDictPicker_);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Pronunciation mode"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(dictMode_, 0, wxALIGN_LEFT);
        grid->AddSpacer(1);
        auto* checks = new wxBoxSizer(wxVERTICAL);
        checks->Add(dictUseOgi_, 0, wxBOTTOM, 4);
        checks->Add(dictRecursive_, 0);
        grid->Add(checks, 0, wxEXPAND);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(panel, ID_ScanUnknowns, "Scan unknown words"), 0, wxRIGHT, 8);
        buttons->Add(new wxButton(panel, ID_AppendDictStubs, "Append dictionary stubs"), 0, wxRIGHT, 8);
        buttons->Add(new wxButton(panel, ID_OpenUnknownReport, "Open report"), 0, wxRIGHT, 8);
        buttons->Add(new wxButton(panel, ID_OpenOverrideDict, "Open dictionary"), 0, wxRIGHT, 8);
        buttons->AddStretchSpacer(1);
        outer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        return panel;
    }

    wxPanel* BuildLookupPage(wxWindow* parent) {
        auto* panel = new wxPanel(parent);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);
        auto* grid = new wxFlexGridSizer(2, 6, 6);
        grid->AddGrowableCol(1, 1);
        lookupWord_ = new wxTextCtrl(panel, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        lookupUserDictPicker_ = new wxFilePickerCtrl(
            panel, wxID_ANY, wxEmptyString, "Select optional pronunciation dictionary",
            "Dictionary files (*.dict;*.txt)|*.dict;*.txt|All files (*.*)|*.*",
            wxDefaultPosition, wxDefaultSize,
            wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        lookupUseOgi_ = new wxCheckBox(panel, wxID_ANY, "Use OGI/Festival lexicon fallback");
        lookupUseOgi_->SetValue(true);
        AddTextRow(panel, grid, "Word", lookupWord_);
        AddPickerRow(panel, grid, "Dictionary", lookupUserDictPicker_);
        grid->AddSpacer(1);
        grid->Add(lookupUseOgi_, 0, wxEXPAND);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(panel, ID_RunLookup, "Lookup word"), 0, wxRIGHT, 8);
        buttons->AddStretchSpacer(1);
        outer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        return panel;
    }

    void BuildDictionaryDialog() {
        dictionaryDialog_ = new wxDialog(
            this, wxID_ANY, "Pronunciation Dictionary",
            wxDefaultPosition, wxDefaultSize,
            wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* root = new wxBoxSizer(wxVERTICAL);
        dictionaryDialog_->SetSizer(root);
        auto* book = new wxNotebook(dictionaryDialog_, wxID_ANY);
        book->AddPage(BuildDictionaryWorkflowPage(book), "Unknown words", true);
        book->AddPage(BuildLookupPage(book), "Lookup", false);
        root->Add(book, 1, wxEXPAND | wxALL, 8);
        auto* close = new wxButton(dictionaryDialog_, wxID_CLOSE, "Close");
        root->Add(close, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        wxui::configureResponsiveWindow(*dictionaryDialog_, wxSize(820, 560), wxSize(560, 380));
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { dictionaryDialog_->Hide(); });
        dictionaryDialog_->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
            dictionaryDialog_->Hide();
            event.Veto();
        });
    }

    void BindEvents() {
        Bind(wxEVT_BUTTON, &MainFrame::OnRunAlign, this, ID_RunAlign);
        Bind(wxEVT_BUTTON, &MainFrame::OnRunBatch, this, ID_RunBatch);
        Bind(wxEVT_BUTTON, &MainFrame::OnScanUnknowns, this, ID_ScanUnknowns);
        Bind(wxEVT_BUTTON, &MainFrame::OnAppendDictStubs, this, ID_AppendDictStubs);
        Bind(wxEVT_BUTTON, &MainFrame::OnOpenUnknownReport, this, ID_OpenUnknownReport);
        Bind(wxEVT_BUTTON, &MainFrame::OnOpenOverrideDict, this, ID_OpenOverrideDict);
        Bind(wxEVT_BUTTON, &MainFrame::OnRunLookup, this, ID_RunLookup);
        Bind(wxEVT_BUTTON, &MainFrame::OnStop, this, ID_StopProcess);
        Bind(wxEVT_BUTTON, &MainFrame::OnClearLog, this, ID_ClearLog);
        Bind(wxEVT_BUTTON, &MainFrame::OnCopyLog, this, ID_CopyLog);
        Bind(wxEVT_BUTTON, &MainFrame::OnSaveLog, this, ID_SaveLog);
        Bind(wxEVT_BUTTON, &MainFrame::OnOpenWorkDir, this, ID_OpenWorkDir);
        Bind(wxEVT_BUTTON, &MainFrame::OnLoadLipEditor, this, ID_LoadLipEditor);
        Bind(wxEVT_BUTTON, &MainFrame::OnSaveLipEditor, this, ID_SaveLipEditor);
        Bind(wxEVT_BUTTON, &MainFrame::OnSaveLipEditorAs, this, ID_SaveLipEditorAs);
        Bind(wxEVT_BUTTON, &MainFrame::OnInsertLipKey, this, ID_InsertLipKey);
        Bind(wxEVT_BUTTON, &MainFrame::OnDeleteLipKeys, this, ID_DeleteLipKeys);
        Bind(wxEVT_BUTTON, &MainFrame::OnShiftLipKeys, this, ID_ShiftLipKeys);
        Bind(wxEVT_BUTTON, &MainFrame::OnNormalizeLipKeys, this, ID_NormalizeLipKeys);
        Bind(wxEVT_BUTTON, &MainFrame::OnTimelinePlayCursor, this, ID_TimelinePlayCursor);
        Bind(wxEVT_CHOICE, &MainFrame::OnTimelineShapeChanged, this, ID_TimelineShapeChanged);
        Bind(wxEVT_SLIDER, &MainFrame::OnTimelineZoomChanged, this, ID_TimelineZoomChanged);
        Bind(wxEVT_BUTTON, &MainFrame::OnOpenOutputFolder, this, ID_OpenOutputFolder);
        Bind(wxEVT_MENU, &MainFrame::OnShowModelsSettings, this, ID_ModelsSettings);
        Bind(wxEVT_MENU, &MainFrame::OnShowPronunciationDictionary, this, ID_PronunciationDictionary);
        Bind(wxEVT_MENU, &MainFrame::OnToggleDarkMode, this, ID_ToggleDarkMode);
        Bind(wxEVT_MENU, &MainFrame::OnIncreaseFontScale, this, ID_FontIncrease);
        Bind(wxEVT_MENU, &MainFrame::OnDecreaseFontScale, this, ID_FontDecrease);
        Bind(wxEVT_MENU, &MainFrame::OnResetFontScale, this, ID_FontReset);
        Bind(wxEVT_TIMER, &MainFrame::OnTimer, this, ID_ProcessTimer);
        Bind(wxEVT_END_PROCESS, &MainFrame::OnProcessTerminated, this);
        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

        wavPicker_->Bind(wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent&) {
            MaybeDeriveAlignOutput();
        });
        lipOutputPicker_->Bind(wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent&) {
            if (workDirPicker_->GetPath().empty()) {
                workDirPicker_->SetPath(DefaultWorkDirForOutput(lipOutputPicker_->GetPath()));
            }
        });
        dictOutputDirPicker_->Bind(wxEVT_DIRPICKER_CHANGED, [this](wxFileDirPickerEvent&) {
            UpdateDictionaryDefaults();
        });
        timelineAudioPicker_->Bind(wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent&) {
            ReloadTimelineAudio();
        });
        timelinePhnPicker_->Bind(wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent&) {
            RefreshTimelinePanel();
        });
        lookupWord_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& event) {
            OnRunLookup(event);
        });
        lipGrid_->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent& event) {
            if (!synchronizingEditViews_ && SyncLipGridToEntries(false)) {
                lipEditorDirty_ = true;
                synchronizingEditViews_ = true;
                UpdateLipGridRow(event.GetRow());
                synchronizingEditViews_ = false;
                SyncEditorToTimeline();
            }
            event.Skip();
        });
    }

    void OnToggleDarkMode(wxCommandEvent& event) {
        ApplyDarkMode(event.IsChecked());
    }

    void OnIncreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        ChangeFontScaleSteps(1);
    }

    void OnDecreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        ChangeFontScaleSteps(-1);
    }

    void OnResetFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        fontScale_ = neoview::kDefaultFontScale;
        settings_.setFontScale(fontScale_);
        ApplyFontScale();
        wxui::setStatusText(*this, "Font size: 100%");
    }

    void LoadConfig() {
        wxFileConfig config("neolip_gui");
        if (!settings_.restoreWindowPlacement(*this)) {
            int x = 0;
            int y = 0;
            int w = 1140;
            int h = 860;
            if (config.Read("window_x", &x) && config.Read("window_y", &y) &&
                config.Read("window_w", &w) && config.Read("window_h", &h)) {
                SetSize(x, y, w, h);
                settings_.saveWindowPlacement(*this);
            }
        }

        modelsRoot_ = config.Read("models_root", wxEmptyString);
        if (modelsRoot_.empty()) modelsRoot_ = DefaultModelRoot();

        const wxString savedDict = config.Read("user_dict", wxEmptyString);
        userDictPicker_->SetPath(savedDict);
        batchUserDictPicker_->SetPath(savedDict);
        dictOverrideDictPicker_->SetPath(savedDict);
        lookupUserDictPicker_->SetPath(savedDict);
        ApplyDefaultKotorOverrideIfAvailable();

        const bool useOgi = config.ReadBool("use_ogi", true);
        useOgi_->SetValue(useOgi);
        batchUseOgi_->SetValue(useOgi);
        dictUseOgi_->SetValue(useOgi);
        lookupUseOgi_->SetValue(useOgi);

        const wxString mode = config.Read("mode", "simple");
        alignMode_->SetStringSelection(mode);
        batchMode_->SetStringSelection(mode);
        dictMode_->SetStringSelection(mode);

        alignTailRest_->SetValue(config.Read("tail_rest_ms", "0"));
        alignClosePause_->SetValue(config.Read("close_on_pause_ms", "160"));
        alignSmoothing_->SetStringSelection(config.Read("lip_smoothing", "light"));
        alignPreserveClosures_->SetValue(config.ReadBool("preserve_closures", true));
        alignAnticipateClosures_->SetValue(config.Read("anticipate_closures_ms", "40"));
        alignAnticipateFricatives_->SetValue(config.Read("anticipate_fricatives_ms", "25"));
        alignAnticipateStops_->SetValue(config.Read("anticipate_stops_ms", "20"));
        writeReports_->SetValue(config.ReadBool("write_reports", true));

        batchTailRest_->SetValue(config.Read("batch_tail_rest_ms", config.Read("tail_rest_ms", "0")));
        batchClosePause_->SetValue(config.Read("batch_close_on_pause_ms", config.Read("close_on_pause_ms", "160")));
        batchSmoothing_->SetStringSelection(config.Read("batch_lip_smoothing", config.Read("lip_smoothing", "light")));
        batchPreserveClosures_->SetValue(config.ReadBool("batch_preserve_closures", config.ReadBool("preserve_closures", true)));
        batchAnticipateClosures_->SetValue(config.Read("batch_anticipate_closures_ms", config.Read("anticipate_closures_ms", "40")));
        batchAnticipateFricatives_->SetValue(config.Read("batch_anticipate_fricatives_ms", config.Read("anticipate_fricatives_ms", "25")));
        batchAnticipateStops_->SetValue(config.Read("batch_anticipate_stops_ms", config.Read("anticipate_stops_ms", "20")));
        batchJobs_->SetValue(config.Read("batch_jobs", "auto"));
        batchUseOgi_->SetValue(config.ReadBool("batch_use_ogi", useOgi));
        batchRecursive_->SetValue(config.ReadBool("batch_recursive", false));
        batchWorkdirs_->SetValue(config.ReadBool("batch_workdirs", true));
        batchFailFast_->SetValue(config.ReadBool("batch_fail_fast", false));
        batchResume_->SetValue(config.ReadBool("batch_resume", false));
        batchDecodeCachePicker_->SetPath(config.Read("batch_decode_cache", wxEmptyString));

        wavPicker_->SetPath(config.Read("create_wav", wxEmptyString));
        textPicker_->SetPath(config.Read("create_transcript_file", wxEmptyString));
        literalText_->SetValue(config.Read("create_transcript_text", wxEmptyString));
        lipOutputPicker_->SetPath(config.Read("create_output_lip", wxEmptyString));
        workDirPicker_->SetPath(config.Read("create_work_dir", wxEmptyString));

        batchInputDirPicker_->SetPath(config.Read("batch_input_dir", wxEmptyString));
        batchOutputDirPicker_->SetPath(config.Read("batch_output_dir", wxEmptyString));

        dictInputDirPicker_->SetPath(config.Read("dict_input_dir", wxEmptyString));
        dictOutputDirPicker_->SetPath(config.Read("dict_output_dir", wxEmptyString));
        dictUnknownReportPicker_->SetPath(config.Read("dict_unknown_report", wxEmptyString));
        dictSourceReportPicker_->SetPath(config.Read("dict_source_report", wxEmptyString));
        dictRecursive_->SetValue(config.ReadBool("dict_recursive", false));

        timelineAudioPicker_->SetPath(config.Read("timeline_audio", wxEmptyString));
        timelineLipPicker_->SetPath(config.Read(
            "timeline_lip", config.Read("lip_editor_open", wxEmptyString)));
        timelinePhnPicker_->SetPath(config.Read("timeline_phn", wxEmptyString));
        timelineSavePicker_->SetPath(config.Read(
            "timeline_save", config.Read("lip_editor_save", wxEmptyString)));

        fontScale_ = settings_.fontScale();
        bool darkMode = settings_.darkMode();
        if (!settings_.hasEntry("UI/DarkMode")) {
            darkMode = config.ReadBool("dark_mode", darkMode);
        }
        ApplyDarkMode(darkMode);
        UpdateDictionaryDefaults();
        MaybeDeriveAlignOutput();
    }

    void SaveConfig() {
        wxFileConfig config("neolip_gui");
        settings_.saveWindowPlacement(*this);
        settings_.setDarkMode(darkModeMenuItem_ != nullptr && darkModeMenuItem_->IsChecked());
        settings_.setFontScale(fontScale_);

        config.Write("models_root", modelsRoot_);
        wxString savedDict = userDictPicker_->GetPath();
        if (savedDict.empty()) savedDict = batchUserDictPicker_->GetPath();
        if (savedDict.empty()) savedDict = dictOverrideDictPicker_->GetPath();
        if (savedDict.empty()) savedDict = lookupUserDictPicker_->GetPath();
        config.Write("user_dict", savedDict);
        config.Write("use_ogi", useOgi_->GetValue());
        config.Write("mode", alignMode_->GetStringSelection());
        config.Write("write_reports", writeReports_->GetValue());

        config.Write("tail_rest_ms", alignTailRest_->GetValue());
        config.Write("close_on_pause_ms", alignClosePause_->GetValue());
        config.Write("lip_smoothing", alignSmoothing_->GetStringSelection());
        config.Write("preserve_closures", alignPreserveClosures_->GetValue());
        config.Write("anticipate_closures_ms", alignAnticipateClosures_->GetValue());
        config.Write("anticipate_fricatives_ms", alignAnticipateFricatives_->GetValue());
        config.Write("anticipate_stops_ms", alignAnticipateStops_->GetValue());

        config.Write("create_wav", wavPicker_->GetPath());
        config.Write("create_transcript_file", textPicker_->GetPath());
        config.Write("create_transcript_text", literalText_->GetValue());
        config.Write("create_output_lip", lipOutputPicker_->GetPath());
        config.Write("create_work_dir", workDirPicker_->GetPath());

        config.Write("batch_input_dir", batchInputDirPicker_->GetPath());
        config.Write("batch_output_dir", batchOutputDirPicker_->GetPath());
        config.Write("batch_tail_rest_ms", batchTailRest_->GetValue());
        config.Write("batch_close_on_pause_ms", batchClosePause_->GetValue());
        config.Write("batch_lip_smoothing", batchSmoothing_->GetStringSelection());
        config.Write("batch_preserve_closures", batchPreserveClosures_->GetValue());
        config.Write("batch_anticipate_closures_ms", batchAnticipateClosures_->GetValue());
        config.Write("batch_anticipate_fricatives_ms", batchAnticipateFricatives_->GetValue());
        config.Write("batch_anticipate_stops_ms", batchAnticipateStops_->GetValue());
        config.Write("batch_jobs", batchJobs_->GetValue());
        config.Write("batch_use_ogi", batchUseOgi_->GetValue());
        config.Write("batch_recursive", batchRecursive_->GetValue());
        config.Write("batch_workdirs", batchWorkdirs_->GetValue());
        config.Write("batch_fail_fast", batchFailFast_->GetValue());
        config.Write("batch_resume", batchResume_->GetValue());
        config.Write("batch_decode_cache", batchDecodeCachePicker_->GetPath());

        config.Write("dict_input_dir", dictInputDirPicker_->GetPath());
        config.Write("dict_output_dir", dictOutputDirPicker_->GetPath());
        config.Write("dict_unknown_report", dictUnknownReportPicker_->GetPath());
        config.Write("dict_source_report", dictSourceReportPicker_->GetPath());
        config.Write("dict_recursive", dictRecursive_->GetValue());

        config.Write("timeline_audio", timelineAudioPicker_->GetPath());
        config.Write("timeline_lip", timelineLipPicker_->GetPath());
        config.Write("timeline_phn", timelinePhnPicker_->GetPath());
        config.Write("timeline_save", timelineSavePicker_->GetPath());
        config.Flush();
    }

    void RememberPaths() {
        SaveConfig();
    }

    void MaybeDeriveAlignOutput() {
        if (lipOutputPicker_->GetPath().empty() && !wavPicker_->GetPath().empty()) {
            wxFileName wav(wavPicker_->GetPath());
            lipOutputPicker_->SetPath(wxFileName(
                wav.GetPath(), wav.GetName() + ".lip").GetFullPath());
        }
        if (workDirPicker_->GetPath().empty() && !lipOutputPicker_->GetPath().empty()) {
            workDirPicker_->SetPath(DefaultWorkDirForOutput(lipOutputPicker_->GetPath()));
        }
    }

    wxString ToolPath() const {
        return DefaultToolPath();
    }

    wxString DefaultModelRoot() const {
#ifdef __EMSCRIPTEN__
        if (IsRegularFile("/models/phonemes_new_16k.train.spec")) return "/models";
#endif
        wxArrayString candidates;
        const wxString sep(wxFileName::GetPathSeparator());
        wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
        const wxString binDir = exe.GetPath();
        candidates.Add(binDir + sep + ".." + sep + "Resources" + sep + "models");
        candidates.Add(binDir + sep + "models");
        candidates.Add(binDir + sep + ".." + sep + "models");
        candidates.Add(binDir + sep + ".." + sep + ".." + sep + "models");
        candidates.Add(binDir);

        for (const wxString& raw : candidates) {
            wxFileName root(raw, "");
            root.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
            const wxString path = root.GetFullPath();
            if (IsRegularFile(wxFileName(path, "phonemes_new_16k.train.spec").GetFullPath())) {
                return path;
            }
        }
        return wxEmptyString;
    }

    wxString ModelRoot() const {
        return modelsRoot_;
    }

    void ApplyDefaultKotorOverrideIfAvailable() {
        const wxString dict = DefaultKotorOverridePath(ToolPath(), ModelRoot());
        if (dict.empty()) return;
        if (userDictPicker_ != nullptr && userDictPicker_->GetPath().empty()) {
            userDictPicker_->SetPath(dict);
        }
        if (batchUserDictPicker_ != nullptr && batchUserDictPicker_->GetPath().empty()) {
            batchUserDictPicker_->SetPath(dict);
        }
        if (dictOverrideDictPicker_ != nullptr && dictOverrideDictPicker_->GetPath().empty()) {
            dictOverrideDictPicker_->SetPath(dict);
        }
        if (lookupUserDictPicker_ != nullptr && lookupUserDictPicker_->GetPath().empty()) {
            lookupUserDictPicker_->SetPath(dict);
        }
    }

    std::vector<wxString> RequiredModelFiles(const wxString& root) const {
        const wxString sep(wxFileName::GetPathSeparator());
        return {
            root + sep + "phonemes_new_16k.train.spec",
            root + sep + "phonemes_new_16k.parts",
            root + sep + "bc_16k" + sep + "manner_nnet.45",
            root + sep + "bc_16k" + sep + "place_nnet.45",
            root + sep + "bc_16k" + sep + "height_nnet.45",
            root + sep + "trans_16k" + sep + "manner_nnet.45",
            root + sep + "trans_16k" + sep + "place_nnet.45",
            root + sep + "trans_16k" + sep + "height_nnet.45"
        };
    }

    bool ValidateModelRoot(const wxString& root, bool showSuccess) {
        if (root.empty()) {
            wxMessageBox(
                "Choose the NeoLIP models folder in Settings > Models.",
                "Models not configured", wxOK | wxICON_WARNING,
                this);
            return false;
        }

        wxString missing;
        for (const wxString& path : RequiredModelFiles(root)) {
            if (!IsRegularFile(path)) missing += "  " + path + "\n";
        }
        if (!missing.empty()) {
            wxMessageBox(
                "The selected models folder is missing required files:\n\n" + missing,
                "Invalid models folder", wxOK | wxICON_ERROR,
                this);
            return false;
        }
        if (showSuccess) {
            wxMessageBox("The models folder is valid.", "Models",
                         wxOK | wxICON_INFORMATION,
                         this);
        }
        return true;
    }

    bool ValidateModelRootDetailed(bool showSuccess) {
        return ValidateModelRoot(ModelRoot(), showSuccess);
    }

    void OnShowModelsSettings(wxCommandEvent&) {
        wxDialog dialog(this, wxID_ANY, "Models",
                        wxDefaultPosition, wxDefaultSize,
                        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* root = new wxBoxSizer(wxVERTICAL);
        dialog.SetSizer(root);

        root->Add(new wxStaticText(
                      &dialog, wxID_ANY,
                      "Select the folder containing NeoLIP's acoustic model files."),
                  0, wxEXPAND | wxALL, 10);
        auto* picker = new wxDirPickerCtrl(
            &dialog, wxID_ANY, modelsRoot_, "Select NeoLIP models folder",
            wxDefaultPosition, wxDefaultSize,
            wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
        root->Add(picker, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* validate = new wxButton(&dialog, wxID_ANY, "Validate");
#ifndef __EMSCRIPTEN__
        auto* open = new wxButton(&dialog, wxID_ANY, "Open folder");
#endif
        auto* cancel = new wxButton(&dialog, wxID_CANCEL, "Cancel");
        auto* save = new wxButton(&dialog, wxID_OK, "Save");
        buttons->Add(validate, 0, wxRIGHT, 6);
#ifndef __EMSCRIPTEN__
        buttons->Add(open, 0, wxRIGHT, 6);
#endif
        buttons->AddStretchSpacer(1);
        buttons->Add(cancel, 0, wxRIGHT, 6);
        buttons->Add(save, 0);
        root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        validate->Bind(wxEVT_BUTTON, [this, picker](wxCommandEvent&) {
            ValidateModelRoot(picker->GetPath(), true);
        });
#ifndef __EMSCRIPTEN__
        open->Bind(wxEVT_BUTTON, [picker](wxCommandEvent&) {
            if (!picker->GetPath().empty()) {
                OpenExternalPath(picker->GetPath(), picker);
            }
        });
#endif
        save->Bind(wxEVT_BUTTON, [this, picker, &dialog](wxCommandEvent&) {
            if (!ValidateModelRoot(picker->GetPath(), false)) return;
            modelsRoot_ = picker->GetPath();
            ApplyDefaultKotorOverrideIfAvailable();
            SaveConfig();
            dialog.EndModal(wxID_OK);
        });

        dialog.Fit();
        wxui::configureResponsiveWindow(dialog, wxSize(700, 260), wxSize(480, 220));
        dialog.CentreOnParent();
        wxui::constrainWindowToDisplay(dialog);
        dialog.ShowModal();
    }

    void OnShowPronunciationDictionary(wxCommandEvent&) {
        if (dictionaryDialog_ == nullptr) return;
        dictionaryDialog_->CentreOnParent();
        wxui::constrainWindowToDisplay(*dictionaryDialog_);
        dictionaryDialog_->Show(true);
        dictionaryDialog_->Raise();
        dictionaryDialog_->SetFocus();
    }

    bool ValidateDictionaryFile(const wxString& path, bool mustExist) {
        if (path.empty() || !mustExist) return true;
        if (!IsRegularFile(path)) {
            wxMessageBox("Dictionary file does not exist:\n\n" + path,
                         "Missing dictionary", wxOK | wxICON_ERROR, this);
            return false;
        }
        return true;
    }

    bool ValidateCommon() {
#ifndef __EMSCRIPTEN__
        wxFileName toolName(ToolPath());
        if (!toolName.IsAbsolute() || !IsRegularFile(ToolPath())) {
            wxMessageBox(
                "NeoLIP's internal backend helper is missing:\n\n" + ToolPath() +
                    "\n\nKeep neolip_backend beside NeoLIP, or reinstall NeoLIP.",
                "Incomplete NeoLIP installation", wxOK | wxICON_ERROR, this);
            return false;
        }
#endif
        return ValidateModelRootDetailed(false);
    }

    void AddModelsFlag(wxArrayString& args) const {
        args.Add("--models");
        args.Add(ModelRoot());
    }

    void AppendTextFlags(wxArrayString& args, const wxString& mode,
                         const wxString& userDict, bool useOgi,
                         const wxString& sourceReport = wxEmptyString,
                         const wxString& unknownReport = wxEmptyString) {
        args.Add("--mode");
        args.Add(mode.empty() ? wxString("simple") : mode);
        if (!userDict.empty()) {
            args.Add("--override-dict");
            args.Add(userDict);
        }
        args.Add(useOgi ? "--use-ogi" : "--no-ogi");
        if (!sourceReport.empty()) {
            args.Add("--source-report");
            args.Add(sourceReport);
        }
        if (!unknownReport.empty()) {
            args.Add("--unknown-report");
            args.Add(unknownReport);
        }
    }

    wxString TextSourceFromControls(wxFilePickerCtrl* filePicker,
                                    wxTextCtrl* literal) const {
        const wxString file = filePicker->GetPath();
        if (!file.empty()) return file;
        return literal->GetValue();
    }

    void OnRunAlign(wxCommandEvent&) {
        MaybeDeriveAlignOutput();
        if (!ValidateCommon()) return;
        if (!ValidateDictionaryFile(userDictPicker_->GetPath(),
                                    !userDictPicker_->GetPath().empty())) {
            return;
        }
        if (wavPicker_->GetPath().empty() || !IsRegularFile(wavPicker_->GetPath())) {
            wxMessageBox("Select an existing WAV file.", "Missing WAV",
                         wxOK | wxICON_WARNING, this);
            return;
        }

        const wxString text = TextSourceFromControls(textPicker_, literalText_);
        if (text.empty()) {
            wxMessageBox("Select a transcript file or enter the spoken text.",
                         "Missing transcript", wxOK | wxICON_WARNING, this);
            return;
        }
        if (!textPicker_->GetPath().empty() && !IsRegularFile(textPicker_->GetPath())) {
            wxMessageBox("The selected transcript file does not exist.",
                         "Missing transcript", wxOK | wxICON_WARNING, this);
            return;
        }
        if (lipOutputPicker_->GetPath().empty()) {
            wxMessageBox("Choose an output LIP file.", "Missing output",
                         wxOK | wxICON_WARNING, this);
            return;
        }

        wxArrayString args;
        args.Add(ToolPath());
        args.Add("align-wav-text-lip");
        args.Add(wavPicker_->GetPath());
        args.Add(text);
        args.Add(lipOutputPicker_->GetPath());
        AddModelsFlag(args);
        AppendTailRestFlag(args, alignTailRest_);
        AppendLipQualityFlags(args, alignClosePause_, alignSmoothing_,
                              alignPreserveClosures_, alignAnticipateClosures_,
                              alignAnticipateFricatives_, alignAnticipateStops_);

        const wxString work = workDirPicker_->GetPath();
        if (!work.empty()) {
            args.Add("--work-dir");
            args.Add(work);
        }

        wxString sourceReport;
        wxString unknownReport;
        if (writeReports_->GetValue()) {
            wxString reportDir = work;
            if (reportDir.empty()) {
                reportDir = wxFileName(lipOutputPicker_->GetPath()).GetPath();
            }
            const wxString stem = wxFileName(lipOutputPicker_->GetPath()).GetName();
            sourceReport = wxFileName(
                reportDir, stem + "_pronunciations.tsv").GetFullPath();
            unknownReport = wxFileName(
                reportDir, stem + "_unknown_words.tsv").GetFullPath();
        }
        AppendTextFlags(args, alignMode_->GetStringSelection(),
                        userDictPicker_->GetPath(), useOgi_->GetValue(),
                        sourceReport, unknownReport);

        lastOutputPath_ = lipOutputPicker_->GetPath();
        lastWorkDir_ = work.empty()
            ? wxFileName(lipOutputPicker_->GetPath()).GetPath()
            : work;
        StartCommand(args, false);
    }

    void UpdateDictionaryDefaults() {
        const wxString outDir = dictOutputDirPicker_->GetPath();
        if (!outDir.empty()) {
            if (dictUnknownReportPicker_->GetPath().empty()) dictUnknownReportPicker_->SetPath(wxFileName(outDir, "unknown_words.tsv").GetFullPath());
            if (dictSourceReportPicker_->GetPath().empty()) dictSourceReportPicker_->SetPath(wxFileName(outDir, "pronunciations.tsv").GetFullPath());
            if (dictOverrideDictPicker_->GetPath().empty()) dictOverrideDictPicker_->SetPath(wxFileName(outDir, "kotor_override.dict").GetFullPath());
        }
    }

    bool ValidateDictionaryWorkflow(bool needOutputDir, bool needUnknownReport, bool needOverrideDict) {
        if (!ValidateCommon()) return false;
        UpdateDictionaryDefaults();
        if (dictInputDirPicker_->GetPath().empty()) { wxMessageBox("Select a transcript folder first.", "Missing folder", wxOK | wxICON_WARNING, this); return false; }
        if (needOutputDir && dictOutputDirPicker_->GetPath().empty()) { wxMessageBox("Select an output folder.", "Missing output folder", wxOK | wxICON_WARNING, this); return false; }
        if (needUnknownReport && dictUnknownReportPicker_->GetPath().empty()) { wxMessageBox("Choose an unknown_words.tsv report path.", "Missing unknown report", wxOK | wxICON_WARNING, this); return false; }
        if (needOverrideDict && dictOverrideDictPicker_->GetPath().empty()) { wxMessageBox("Choose an override dictionary path.", "Missing override dictionary", wxOK | wxICON_WARNING, this); return false; }
        return true;
    }

    void OnScanUnknowns(wxCommandEvent&) {
        if (!ValidateDictionaryWorkflow(false, true, false)) return;
        wxArrayString args;
        args.Add(ToolPath());
        args.Add("scan-unknowns");
        args.Add(dictInputDirPicker_->GetPath());
        args.Add(dictUnknownReportPicker_->GetPath());
        AddModelsFlag(args);
        AppendTextFlags(args, dictMode_->GetStringSelection(), dictOverrideDictPicker_->GetPath(), dictUseOgi_->GetValue(), dictSourceReportPicker_->GetPath(), wxEmptyString);
        if (dictRecursive_->GetValue()) args.Add("--recursive");
        StartCommand(args, false);
    }

    void OnAppendDictStubs(wxCommandEvent&) {
        if (!ValidateDictionaryWorkflow(false, true, true)) return;
        wxArrayString args;
        args.Add(ToolPath());
        args.Add("append-dict-stubs");
        args.Add(dictUnknownReportPicker_->GetPath());
        args.Add(dictOverrideDictPicker_->GetPath());
        StartCommand(args, false);
    }

    void OpenOrCreateTextFile(const wxString& path) {
        if (path.empty()) return;
        wxFileName fn(path);
        if (!fn.GetPath().empty()) wxFileName::Mkdir(fn.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        if (!IsRegularFile(fn.GetFullPath())) {
            wxFFile f(path, "ab");
            if (f.IsOpened()) f.Close();
        }
        OpenExternalPath(path, this);
    }

    void OnOpenUnknownReport(wxCommandEvent&) { UpdateDictionaryDefaults(); OpenOrCreateTextFile(dictUnknownReportPicker_->GetPath()); }
    void OnOpenOverrideDict(wxCommandEvent&) { UpdateDictionaryDefaults(); OpenOrCreateTextFile(dictOverrideDictPicker_->GetPath()); }

    void OnRunBatch(wxCommandEvent&) {
        if (!ValidateCommon()) return;
        if (!ValidateDictionaryFile(batchUserDictPicker_->GetPath(),
                                    !batchUserDictPicker_->GetPath().empty())) {
            return;
        }
        if (batchInputDirPicker_->GetPath().empty() ||
            !wxDirExists(batchInputDirPicker_->GetPath())) {
            wxMessageBox("Select an existing folder containing WAV/TXT pairs.",
                         "Missing input folder", wxOK | wxICON_WARNING, this);
            return;
        }
        if (batchOutputDirPicker_->GetPath().empty()) {
            wxMessageBox("Select an output folder.", "Missing output folder",
                         wxOK | wxICON_WARNING, this);
            return;
        }

        wxArrayString args;
        args.Add(ToolPath());
        args.Add("batch-align");
        args.Add(batchInputDirPicker_->GetPath());
        args.Add(batchOutputDirPicker_->GetPath());
        AddModelsFlag(args);
        args.Add("--input");
        args.Add("text");
        args.Add("--output");
        args.Add("lip");
        AppendTextFlags(args, batchMode_->GetStringSelection(),
                        batchUserDictPicker_->GetPath(),
                        batchUseOgi_->GetValue());
        AppendTailRestFlag(args, batchTailRest_);
        AppendLipQualityFlags(args, batchClosePause_, batchSmoothing_,
                              batchPreserveClosures_,
                              batchAnticipateClosures_,
                              batchAnticipateFricatives_,
                              batchAnticipateStops_);
        if (batchRecursive_->GetValue()) args.Add("--recursive");
        args.Add(batchWorkdirs_->GetValue() ? "--workdirs" : "--no-workdirs");
        if (batchFailFast_->GetValue()) args.Add("--fail-fast");
        if (batchResume_->GetValue()) args.Add("--resume");

        wxString jobs = batchJobs_->GetValue();
        jobs.Trim(true).Trim(false);
        if (!jobs.empty()) {
            args.Add("--jobs");
            args.Add(jobs);
        }
        if (!batchDecodeCachePicker_->GetPath().empty()) {
            args.Add("--decode-cache-dir");
            args.Add(batchDecodeCachePicker_->GetPath());
        }
        args.Add("--batch-report");
        args.Add(wxFileName(
            batchOutputDirPicker_->GetPath(), "batch_report.tsv").GetFullPath());
        args.Add("--unknown-report");
        args.Add(wxFileName(
            batchOutputDirPicker_->GetPath(), "unknown_words.tsv").GetFullPath());

        lastOutputPath_ = batchOutputDirPicker_->GetPath();
        lastWorkDir_ = batchOutputDirPicker_->GetPath();
        StartCommand(args, true,
                     CountWavs(batchInputDirPicker_->GetPath(),
                               batchRecursive_->GetValue()));
    }

    void OnRunLookup(wxCommandEvent&) {
        if (!ValidateCommon()) return;
        if (!ValidateDictionaryFile(lookupUserDictPicker_->GetPath(),
                                    !lookupUserDictPicker_->GetPath().empty())) {
            return;
        }
        wxString word = lookupWord_->GetValue();
        word.Trim(true).Trim(false);
        if (word.empty()) {
            wxMessageBox("Enter a word to look up.", "Missing word",
                         wxOK | wxICON_WARNING, this);
            return;
        }

        wxArrayString args;
        args.Add(ToolPath());
        args.Add("lookup-word");
        args.Add(word);
        AddModelsFlag(args);
        if (!lookupUserDictPicker_->GetPath().empty()) {
            args.Add("--override-dict");
            args.Add(lookupUserDictPicker_->GetPath());
        }
        args.Add(lookupUseOgi_->GetValue() ? "--use-ogi" : "--no-ogi");
        StartCommand(args, false);
    }

    int CountWavs(const wxString& dir, bool recursive) const {
        if (dir.empty()) return 0;
        int count = 0;
        try {
            fs::path p(dir.ToStdWstring());
            if (!fs::exists(p)) return 0;
            if (recursive) {
                for (const auto& e : fs::recursive_directory_iterator(p)) if (IsWavPath(e.path())) ++count;
            } else {
                for (const auto& e : fs::directory_iterator(p)) if (IsWavPath(e.path())) ++count;
            }
        } catch (...) { return 0; }
        return count;
    }

    static bool IsWavPath(const fs::path& p) {
        if (!fs::is_regular_file(p)) return false;
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return ext == ".wav";
    }

    void StartCommand(const wxArrayString& args, bool batchCommand, int progressTotal = 0) {
// Browser build uses the native route through neolip_wasm_runtime.
        if (process_) { wxMessageBox("A job is already running.", "Busy", wxOK | wxICON_INFORMATION, this); return; }
        RememberPaths();
        processCommand_ = JoinCommand(args);
        log_->AppendText("\n[starting job]\n");
        process_ = std::make_unique<CsluProcess>(this);
        processPid_ = ExecuteCommand(args, wxEXEC_ASYNC, process_.get());
        if (processPid_ == 0) {
            log_->AppendText("ERROR: failed to start process.\n");
            process_.reset();
            wxui::setStatusText(*this, "Failed to start process");
            return;
        }
        runningBatch_ = batchCommand;
        progressTotal_ = progressTotal;
        progressValue_ = 0;
        progressGauge_->SetRange(progressTotal_ > 0 ? progressTotal_ : 100);
        progressGauge_->SetValue(0);
        progressLabel_->SetLabel(batchCommand && progressTotal_ > 0 ? wxString::Format("0 / %d", progressTotal_) : wxString("Running"));
#ifndef __EMSCRIPTEN__
        stopButton_->Enable(true);
#endif
        wxui::setStatusText(*this, "Running...");
        timer_.Start(100);
    }

    void DrainProcessOutput() {
        if (!process_) return;
        DrainStream(process_->GetInputStream());
        DrainStream(process_->GetErrorStream());
    }

    void DrainStream(wxInputStream* stream) {
        if (!stream) return;
        while (stream->CanRead()) {
            char buffer[4096];
            stream->Read(buffer, sizeof(buffer) - 1);
            const size_t count = static_cast<size_t>(stream->LastRead());
            if (count == 0) break;
            buffer[count] = '\0';
            wxString chunk = wxString::FromUTF8(buffer);
            log_->AppendText(chunk);
            if (runningBatch_) UpdateBatchProgressFromText(chunk);
        }
    }

    void UpdateBatchProgressFromText(const wxString& text) {
        size_t pos = 0;
        while (true) {
            const size_t found = text.find("batch_item\t", pos);
            if (found == wxString::npos) break;
            ++progressValue_;
            pos = found + 1;
        }
        if (progressTotal_ > 0) {
            if (progressValue_ > progressTotal_) progressValue_ = progressTotal_;
            progressGauge_->SetValue(progressValue_);
            progressLabel_->SetLabel(wxString::Format("%d / %d", progressValue_, progressTotal_));
        }
    }

    void OnTimer(wxTimerEvent&) {
        DrainProcessOutput();
        if (process_ && progressTotal_ == 0) progressGauge_->Pulse();
    }

    void OnProcessTerminated(wxProcessEvent& event) {
        DrainProcessOutput();
        const int code = event.GetExitCode();
        log_->AppendText(wxString::Format("\n[process exited with code %d]\n", code));
        timer_.Stop();
        process_.reset();
        processPid_ = 0;
        stopButton_->Enable(false);
        runningBatch_ = false;
        if (progressTotal_ > 0 && code == 0) progressGauge_->SetValue(progressTotal_);
        progressLabel_->SetLabel(code == 0 ? "Done" : "Failed or cancelled");
        wxui::setStatusText(*this, code == 0 ? "Done" : "Job failed");
    }

    void OnStop(wxCommandEvent&) { StopRunningProcess(true); }

    void StopRunningProcess(bool notify) {
#ifndef __EMSCRIPTEN__
        if (!process_ || processPid_ == 0) return;
        const wxKillError rc = wxProcess::Kill(processPid_, wxSIGTERM, wxKILL_CHILDREN);
        if (notify) log_->AppendText(wxString::Format("\n[cancel requested, wxKill result %d]\n", static_cast<int>(rc)));
#else
        (void)notify;
#endif
    }

    void OnClearLog(wxCommandEvent&) { log_->Clear(); }

    void OnCopyLog(wxCommandEvent&) {
        CopyToClipboard(log_->GetValue(), "Copied log to clipboard");
    }

    void CopyToClipboard(const wxString& text, const wxString& status) {
        if (text.empty()) return;
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(text));
            wxTheClipboard->Close();
            wxui::setStatusText(*this, status);
        }
    }

    void OnSaveLog(wxCommandEvent&) {
        wxFileDialog dlg(this, "Save activity log", wxEmptyString, "NeoLIP-log.txt", "Text files (*.txt)|*.txt|All files (*.*)|*.*", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK) return;
        wxFFile f(dlg.GetPath(), "wb");
        if (!f.IsOpened()) { wxMessageBox("Could not write log file.", "Save failed", wxOK | wxICON_ERROR, this); return; }
        f.Write(log_->GetValue(), wxConvUTF8);
        f.Close();
        wxui::setStatusText(*this, "Saved log");
    }

    void OnOpenWorkDir(wxCommandEvent&) {
        wxString path = lastWorkDir_;
        if (path.empty()) {
            switch (notebook_->GetSelection()) {
                case 0:
                    path = workDirPicker_->GetPath();
                    if (path.empty() && !lipOutputPicker_->GetPath().empty()) {
                        path = wxFileName(lipOutputPicker_->GetPath()).GetPath();
                    }
                    break;
                case 1: {
                    wxString file = timelineSavePicker_->GetPath();
                    if (file.empty()) file = timelineLipPicker_->GetPath();
                    if (!file.empty()) path = wxFileName(file).GetPath();
                    break;
                }
                case 2:
                    path = batchOutputDirPicker_->GetPath();
                    break;
                default:
                    break;
            }
        }
        if (!path.empty()) OpenExternalPath(path, this);
    }

    void OnOpenOutputFolder(wxCommandEvent&) {
        if (lipOutputPicker_->GetPath().empty()) return;
        const wxString path = wxFileName(lipOutputPicker_->GetPath()).GetPath();
        if (!path.empty()) OpenExternalPath(path, this);
    }

    void UpdateLipGridRow(int row) {
        if (lipGrid_ == nullptr || row < 0 ||
            row >= static_cast<int>(lipEditorEntries_.size()) ||
            row >= lipGrid_->GetNumberRows()) {
            return;
        }
        const GuiLipEntry& entry = lipEditorEntries_[static_cast<std::size_t>(row)];
        lipGrid_->SetCellValue(row, 0, wxString::Format("%d", row));
        lipGrid_->SetCellValue(row, 1, wxString::Format("%.6f", entry.timeSeconds));
        lipGrid_->SetCellValue(row, 2, wxString::Format("%d", entry.shape));
        lipGrid_->SetCellValue(row, 3, KotorShapeName(entry.shape));
        lipGrid_->SetReadOnly(row, 0, true);
        lipGrid_->SetReadOnly(row, 3, true);
    }

    void RefreshLipGrid() {
        if (lipGrid_ == nullptr) return;
        const bool previousSync = synchronizingEditViews_;
        synchronizingEditViews_ = true;
        wxGridUpdateLocker updateLocker(lipGrid_);
        const int existingRows = lipGrid_->GetNumberRows();
        if (existingRows > 0) lipGrid_->DeleteRows(0, existingRows);
        if (!lipEditorEntries_.empty()) {
            lipGrid_->AppendRows(static_cast<int>(lipEditorEntries_.size()));
        }
        for (int row = 0; row < static_cast<int>(lipEditorEntries_.size()); ++row) {
            UpdateLipGridRow(row);
        }
        synchronizingEditViews_ = previousSync;
        UpdateLipEditorStatus();
    }

    bool SyncLipGridToEntries(bool showErrors = true) {
        if (lipGrid_ == nullptr) return false;
        std::vector<GuiLipEntry> next;
        const int rows = lipGrid_->GetNumberRows();
        next.reserve(static_cast<std::size_t>(std::max(rows, 0)));
        for (int row = 0; row < rows; ++row) {
            double time = 0.0;
            long shape = 0;
            if (!lipGrid_->GetCellValue(row, 1).ToDouble(&time) ||
                !std::isfinite(time) || time < 0.0) {
                if (showErrors) {
                    wxMessageBox(wxString::Format(
                                     "Time must be a non-negative number at row %d.",
                                     row),
                                 "Invalid LIP table", wxOK | wxICON_WARNING, this);
                }
                return false;
            }
            if (!lipGrid_->GetCellValue(row, 2).ToLong(&shape) ||
                shape < 0 || shape > 15) {
                if (showErrors) {
                    wxMessageBox(wxString::Format(
                                     "Shape must be 0 through 15 at row %d.", row),
                                 "Invalid LIP table", wxOK | wxICON_WARNING, this);
                }
                return false;
            }
            next.push_back({time, static_cast<std::uint8_t>(shape)});
        }
        lipEditorEntries_ = std::move(next);
        UpdateLipEditorStatus();
        return true;
    }

    void UpdateLipEditorStatus() {
        if (lipEditorStatus_ == nullptr) return;
        if (lipEditorEntries_.empty()) {
            lipEditorStatus_->SetLabel("No visible mouth keys loaded.");
            return;
        }
        const auto maxIt = std::max_element(
            lipEditorEntries_.begin(), lipEditorEntries_.end(),
            [](const GuiLipEntry& lhs, const GuiLipEntry& rhs) {
                return lhs.timeSeconds < rhs.timeSeconds;
            });
        const bool finalRest = lipEditorEntries_.back().shape == 0;
        lipEditorStatus_->SetLabel(wxString::Format(
            "visible keys %zu   duration %.3fs   final rest %s",
            lipEditorEntries_.size(), maxIt->timeSeconds,
            finalRest ? "yes" : "will be normalized on save"));
    }

    std::vector<int> SelectedLipRows() const {
        std::vector<int> rows;
        if (lipGrid_ == nullptr) return rows;
        const wxArrayInt selected = lipGrid_->GetSelectedRows();
        for (unsigned i = 0; i < selected.GetCount(); ++i) {
            rows.push_back(selected[i]);
        }
        if (rows.empty()) {
            const int cursor = lipGrid_->GetGridCursorRow();
            if (cursor >= 0 && cursor < lipGrid_->GetNumberRows()) {
                rows.push_back(cursor);
            }
        }
        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        return rows;
    }

    void SyncEditorToTimeline() {
        if (synchronizingEditViews_) return;
        synchronizingEditViews_ = true;
        timelineEntries_ = lipEditorEntries_;
        timelineLipDuration_ = timelineEntries_.empty()
            ? 0.0
            : std::max_element(
                  timelineEntries_.begin(), timelineEntries_.end(),
                  [](const GuiLipEntry& lhs, const GuiLipEntry& rhs) {
                      return lhs.timeSeconds < rhs.timeSeconds;
                  })->timeSeconds;
        RefreshTimelinePanel();
        synchronizingEditViews_ = false;
    }

    void SyncTimelineToEditor() {
        if (timelinePanel_ == nullptr || synchronizingEditViews_) return;
        synchronizingEditViews_ = true;
        timelineEntries_ = timelinePanel_->Entries();
        lipEditorEntries_ = timelineEntries_;
        timelineLipDuration_ = timelineEntries_.empty()
            ? 0.0
            : std::max_element(
                  timelineEntries_.begin(), timelineEntries_.end(),
                  [](const GuiLipEntry& lhs, const GuiLipEntry& rhs) {
                      return lhs.timeSeconds < rhs.timeSeconds;
                  })->timeSeconds;
        RefreshLipGrid();
        synchronizingEditViews_ = false;
        UpdateTimelineStatus();
    }

    void LoadLipEditorPath(const wxString& path) {
        if (path.empty() || !IsRegularFile(path)) {
            wxMessageBox("Select an existing .lip file.", "Missing LIP",
                         wxOK | wxICON_WARNING, this);
            return;
        }
        try {
            double duration = 0.0;
            lipEditorEntries_ = ReadVisibleLipEntries(path, &duration);
            timelineEntries_ = lipEditorEntries_;
            timelineLipDuration_ = duration;
            timelineLipPicker_->SetPath(path);
            timelineSavePicker_->SetPath(path);
            lipEditorDirty_ = false;

            if (!timelineAudioPicker_->GetPath().empty() &&
                IsRegularFile(timelineAudioPicker_->GetPath())) {
                timelineWaveform_ =
                    LoadTimelineWaveformViaBackend(timelineAudioPicker_->GetPath());
            } else {
                timelineWaveform_ = GuiWaveform{};
            }

            RefreshLipGrid();
            RefreshTimelinePanel();
            notebook_->SetSelection(1);
            log_->AppendText(wxString::Format(
                "Loaded LIP: %s\nvisible_keys\t%zu\nduration_seconds\t%.6f\n",
                path, lipEditorEntries_.size(), duration));
            wxui::setStatusText(*this, "Loaded LIP");
        } catch (const std::exception& error) {
            wxMessageBox(wxString::Format("Could not load LIP: %s", error.what()),
                         "Open failed", wxOK | wxICON_ERROR, this);
        }
    }

    void OpenLipFromGameDirectory(const std::filesystem::path& directory) {
        if (!ConfirmLipEditorChanges()) return;
        wxui::requestOpenFile(
            this,
            "Open KotOR LIP",
            "KotOR LIP files (*.lip)|*.lip|All files (*.*)|*.*",
            directory,
            [this](std::optional<std::filesystem::path> selected) {
                if (selected) LoadLipEditorPath(wxui::toWx(selected->u8string()));
            });
    }

    void OnLoadLipEditor(wxCommandEvent&) {
        if (!ConfirmLipEditorChanges()) return;
        const wxString current = timelineLipPicker_->GetPath();
        if (IsRegularFile(current)) {
            LoadLipEditorPath(current);
            return;
        }
        wxui::requestOpenFile(
            this,
            "Open KotOR LIP",
            "KotOR LIP files (*.lip)|*.lip|All files (*.*)|*.*",
            [this](std::optional<std::filesystem::path> selected) {
                if (selected) LoadLipEditorPath(wxui::toWx(selected->u8string()));
            });
    }

    bool SaveLipEditorToPath(const wxString& path) {
        if (!SyncLipGridToEntries(true)) return false;
        timelineEntries_ = lipEditorEntries_;
        try {
            NormalizeVisibleLipEntries(lipEditorEntries_);
            timelineEntries_ = lipEditorEntries_;
            WriteVisibleLipEntries(path, lipEditorEntries_);
#if defined(__EMSCRIPTEN__)
            wxui::publishBrowserFile(
                ToPath(path), wxui::toStd(wxFileName(path).GetFullName()));
#endif
            timelineLipDuration_ = lipEditorEntries_.empty()
                ? 0.0
                : lipEditorEntries_.back().timeSeconds;
            timelineLipPicker_->SetPath(path);
            timelineSavePicker_->SetPath(path);
            RefreshLipGrid();
            RefreshTimelinePanel();
            lastOutputPath_ = path;
            lastWorkDir_ = wxFileName(path).GetPath();
            lipEditorDirty_ = false;
            log_->AppendText(wxString::Format(
                "Saved KotOR LIP: %s\nvisible_keys\t%zu\n",
                path, lipEditorEntries_.size()));
            wxui::setStatusText(*this, "Saved LIP");
            return true;
        } catch (const std::exception& error) {
            wxMessageBox(wxString::Format("Could not save LIP: %s", error.what()),
                         "Save failed", wxOK | wxICON_ERROR, this);
            return false;
        }
    }

    bool SaveLipEditor(bool saveAs) {
        wxString path;
        if (!saveAs) {
            path = timelineSavePicker_->GetPath();
            if (path.empty()) path = timelineLipPicker_->GetPath();
        }
        if (saveAs || path.empty()) {
            wxString current = timelineSavePicker_->GetPath();
            if (current.empty()) current = timelineLipPicker_->GetPath();
            std::string defaultName = current.empty()
                ? std::string("edited.lip")
                : wxui::toStd(wxFileName(current).GetFullName());
            const auto selected = wxui::chooseSaveFile(
                this,
                "Save KotOR LIP As",
                "KotOR LIP files (*.lip)|*.lip|All files (*.*)|*.*",
                defaultName);
            if (!selected) return false;
            path = wxui::toWx(selected->u8string());
        }
        return SaveLipEditorToPath(path);
    }

    bool ConfirmLipEditorChanges() {
        if (!lipEditorDirty_) return true;
        const int answer = wxMessageBox(
            "The current LIP has unsaved changes. Save them before continuing?",
            "Unsaved LIP",
            wxYES_NO | wxCANCEL | wxICON_WARNING,
            this);
        if (answer == wxCANCEL) return false;
        if (answer == wxNO) return true;
        return SaveLipEditor(false);
    }

    void OnSaveLipEditor(wxCommandEvent&) { (void)SaveLipEditor(false); }
    void OnSaveLipEditorAs(wxCommandEvent&) { (void)SaveLipEditor(true); }

    void OnClose(wxCloseEvent& event) {
        if (!ConfirmLipEditorChanges()) {
            event.Veto();
            return;
        }
        event.Skip();
    }

    void OnInsertLipKey(wxCommandEvent&) {
        if (!SyncLipGridToEntries(false)) return;
        const std::vector<int> rows = SelectedLipRows();
        int insertAfter = lipEditorEntries_.empty()
            ? -1
            : static_cast<int>(lipEditorEntries_.size() - 1);
        if (!rows.empty()) insertAfter = rows.back();

        GuiLipEntry entry;
        if (insertAfter >= 0 &&
            insertAfter < static_cast<int>(lipEditorEntries_.size())) {
            entry = lipEditorEntries_[static_cast<std::size_t>(insertAfter)];
            entry.timeSeconds += 0.050;
        } else {
            entry = {0.125, 0};
        }
        const auto position = lipEditorEntries_.begin() +
            std::min<std::size_t>(static_cast<std::size_t>(insertAfter + 1),
                                  lipEditorEntries_.size());
        lipEditorEntries_.insert(position, entry);
        lipEditorDirty_ = true;
        RefreshLipGrid();
        SyncEditorToTimeline();
    }

    void OnDeleteLipKeys(wxCommandEvent&) {
        if (!SyncLipGridToEntries(false)) return;
        const std::vector<int> rows = SelectedLipRows();
        if (rows.empty()) return;
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            if (*it >= 0 && *it < static_cast<int>(lipEditorEntries_.size())) {
                lipEditorEntries_.erase(lipEditorEntries_.begin() + *it);
            }
        }
        lipEditorDirty_ = true;
        RefreshLipGrid();
        SyncEditorToTimeline();
    }

    void OnShiftLipKeys(wxCommandEvent&) {
        if (!SyncLipGridToEntries(true)) return;
        double delta = 0.0;
        if (!lipShiftAmount_->GetValue().ToDouble(&delta)) {
            wxMessageBox("Enter a numeric shift amount in seconds.",
                         "Invalid shift", wxOK | wxICON_WARNING, this);
            return;
        }
        const std::vector<int> rows = SelectedLipRows();
        if (rows.empty()) return;
        for (const int row : rows) {
            if (row >= 0 && row < static_cast<int>(lipEditorEntries_.size())) {
                GuiLipEntry& entry = lipEditorEntries_[static_cast<std::size_t>(row)];
                entry.timeSeconds = std::max(0.0, entry.timeSeconds + delta);
            }
        }
        lipEditorDirty_ = true;
        RefreshLipGrid();
        SyncEditorToTimeline();
    }

    void OnNormalizeLipKeys(wxCommandEvent&) {
        if (!SyncLipGridToEntries(true)) return;
        NormalizeVisibleLipEntries(lipEditorEntries_);
        lipEditorDirty_ = true;
        RefreshLipGrid();
        SyncEditorToTimeline();
    }

    GuiWaveform LoadTimelineWaveformViaBackend(const wxString& audioPath) {
        try {
            return ReadSimpleWaveform(audioPath);
        } catch (...) {
#ifdef __EMSCRIPTEN__
            throw std::runtime_error(
                "the browser build currently supports directly readable PCM WAV audio only");
#else
            const wxString temporary =
                CreateTimelineTempFile("neolip_timeline_decoded_");
            wxArrayString args;
            args.Add(ToolPath());
            args.Add("decode-audio-wav");
            args.Add(audioPath);
            args.Add(temporary);
            try {
                const long result = ExecuteCommand(args, wxEXEC_SYNC);
                if (result != 0 || !IsRegularFile(temporary)) {
                    throw std::runtime_error(
                        "backend audio decoding failed; check the NeoLIP installation");
                }
                GuiWaveform waveform = ReadSimpleWaveform(temporary);
                RemoveFileBestEffort(temporary);
                return waveform;
            } catch (...) {
                RemoveFileBestEffort(temporary);
                throw;
            }
#endif
        }
    }

    void ReloadTimelineAudio() {
        const wxString path = timelineAudioPicker_->GetPath();
        if (path.empty()) {
            timelineWaveform_ = GuiWaveform{};
            RefreshTimelinePanel();
            return;
        }
        if (!IsRegularFile(path)) {
            wxMessageBox("The selected audio file does not exist.", "Missing audio",
                         wxOK | wxICON_WARNING, this);
            return;
        }
        try {
            timelineWaveform_ = LoadTimelineWaveformViaBackend(path);
            RefreshTimelinePanel();
        } catch (const std::exception& error) {
            wxMessageBox(wxString::Format("Could not load audio: %s", error.what()),
                         "Audio load failed", wxOK | wxICON_ERROR, this);
        }
    }

    void UpdateTimelineStatus() {
        if (timelineStatus_ == nullptr) return;
        const double audioDuration = timelineWaveform_.durationSeconds();
        const double lastKey = timelineEntries_.empty()
            ? 0.0
            : std::max_element(
                  timelineEntries_.begin(), timelineEntries_.end(),
                  [](const GuiLipEntry& lhs, const GuiLipEntry& rhs) {
                      return lhs.timeSeconds < rhs.timeSeconds;
                  })->timeSeconds;
        const bool finalRest = !timelineEntries_.empty() &&
                               timelineEntries_.back().shape == 0;
        timelineStatus_->SetLabel(wxString::Format(
            "audio %.3fs   lip %.3fs   keys %zu   last key %.3fs   final rest %s",
            audioDuration, timelineLipDuration_, timelineEntries_.size(), lastKey,
            finalRest ? "yes" : "will be normalized on save"));
    }

    void RefreshTimelinePanel() {
        if (timelinePanel_ == nullptr) return;
        std::vector<GuiPhnInterval> intervals;
        try {
            intervals = ReadGuiPhnIntervals(
                timelinePhnPicker_ != nullptr
                    ? timelinePhnPicker_->GetPath()
                    : wxString());
        } catch (const std::exception& error) {
            log_->AppendText(wxString::Format(
                "PHN overlay warning: %s\n", error.what()));
        }

        const bool previousSync = synchronizingEditViews_;
        synchronizingEditViews_ = true;
        timelinePanel_->SetData(timelineWaveform_, timelineEntries_,
                                std::move(intervals), timelineLipDuration_);
        if (timelineZoom_ != nullptr) {
            timelinePanel_->SetZoom(timelineZoom_->GetValue());
        }
        synchronizingEditViews_ = previousSync;
        UpdateTimelineStatus();
    }

    void OnTimelineShapeChanged(wxCommandEvent&) {
        if (timelinePanel_ == nullptr || timelineShapeChoice_ == nullptr) return;
        timelinePanel_->SetSelectedShape(timelineShapeChoice_->GetSelection());
        SyncTimelineToEditor();
        lipEditorDirty_ = true;
    }

    void OnTimelineZoomChanged(wxCommandEvent&) {
        if (timelinePanel_ != nullptr && timelineZoom_ != nullptr) {
            timelinePanel_->SetZoom(timelineZoom_->GetValue());
        }
    }

    void OnTimelinePlayCursor(wxCommandEvent&) {
// Browser build uses the native route through neolip_wasm_runtime.
        if (!timelinePanel_) return;
        const auto& wf = timelinePanel_->Waveform();
        if (wf.mono.empty() || wf.sampleRate <= 0) { wxMessageBox("Load an audio waveform before playback.", "No audio", wxOK | wxICON_INFORMATION, this); return; }
        wxSound::Stop();
        timelineSound_.reset();
        RemoveFileBestEffort(timelinePreviewPath_);
        timelinePreviewPath_.clear();
        wxString tmp;
        try {
            tmp = CreateTimelineTempFile("neolip_timeline_preview_");
            WritePcm16WavGui(tmp, wf, timelinePanel_->CursorSeconds(), 8.0);
            timelineSound_ = std::make_unique<wxSound>(tmp);
            timelinePreviewPath_ = tmp;
            if (!timelineSound_->IsOk()) { OpenExternalPath(tmp, this); return; }
            timelineSound_->Play(wxSOUND_ASYNC);
            log_->AppendText(wxString::Format("Playback preview from %.3fs\n", timelinePanel_->CursorSeconds()));
        } catch (const std::exception& e) {
            timelineSound_.reset();
            RemoveFileBestEffort(tmp);
            timelinePreviewPath_.clear();
            wxMessageBox(wxString::Format("Could not play timeline audio: %s", e.what()), "Playback failed", wxOK | wxICON_ERROR, this);
        }
    }

};

class NeoLipGuiApp final : public wxApp {
public:
    bool OnInit() override {
        if (!wxApp::OnInit()) return false;
        SetAppName("NeoLIP");

        auto* frame = new MainFrame();
        frame->Show(true);
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(NeoLipGuiApp);
