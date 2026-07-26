#include "lip.hpp"
#include "phn.hpp"
#include "wav.hpp"
#include "NeoSettings.hpp"
#include "NeoViewState.hpp"
#include "NeoWxUi.hpp"
#include "NeoGameDirectoryMenu.hpp"

#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/filepicker.h>
#include <wx/stdpaths.h>
#include <wx/process.h>
#include <wx/timer.h>
#include <wx/filename.h>
#include <wx/fileconf.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/textctrl.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/radiobox.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/ffile.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/dir.h>
#include <wx/arrstr.h>
#include <wx/grid.h>
#include <wx/sound.h>
#include <wx/dcbuffer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int ID_RunAlign = wxID_HIGHEST + 100;
constexpr int ID_RunTextToNtp = wxID_HIGHEST + 101;
constexpr int ID_RunLookup = wxID_HIGHEST + 102;
constexpr int ID_RunCompareTrace = wxID_HIGHEST + 103;
constexpr int ID_StopProcess = wxID_HIGHEST + 104;
constexpr int ID_ClearLog = wxID_HIGHEST + 105;
constexpr int ID_OpenWorkDir = wxID_HIGHEST + 107;
constexpr int ID_ProcessTimer = wxID_HIGHEST + 108;
constexpr int ID_RunBatch = wxID_HIGHEST + 109;
constexpr int ID_ScanUnknowns = wxID_HIGHEST + 110;
constexpr int ID_AppendDictStubs = wxID_HIGHEST + 111;
constexpr int ID_OpenUnknownReport = wxID_HIGHEST + 112;
constexpr int ID_OpenOverrideDict = wxID_HIGHEST + 113;
constexpr int ID_RunDictionaryBatch = wxID_HIGHEST + 114;
constexpr int ID_ValidateModels = wxID_HIGHEST + 115;
constexpr int ID_CopyLog = wxID_HIGHEST + 116;
constexpr int ID_SaveLog = wxID_HIGHEST + 117;
constexpr int ID_RecentModelChoice = wxID_HIGHEST + 118;
constexpr int ID_RecentFolderChoice = wxID_HIGHEST + 119;
constexpr int ID_LoadPhnPreview = wxID_HIGHEST + 120;
constexpr int ID_LoadLastPhnPreview = wxID_HIGHEST + 121;
constexpr int ID_OpenOutputFolder = wxID_HIGHEST + 122;
constexpr int ID_OpenModelFolder = wxID_HIGHEST + 123;
constexpr int ID_LoadLipEditor = wxID_HIGHEST + 124;
constexpr int ID_SaveLipEditor = wxID_HIGHEST + 125;
constexpr int ID_InsertLipKey = wxID_HIGHEST + 126;
constexpr int ID_DeleteLipKeys = wxID_HIGHEST + 127;
constexpr int ID_ShiftLipKeys = wxID_HIGHEST + 128;
constexpr int ID_NormalizeLipKeys = wxID_HIGHEST + 129;
constexpr int ID_LoadLipTimeline = wxID_HIGHEST + 130;
constexpr int ID_SaveLipTimeline = wxID_HIGHEST + 131;
constexpr int ID_TimelineLoadFromEditor = wxID_HIGHEST + 132;
constexpr int ID_TimelineNormalize = wxID_HIGHEST + 133;
constexpr int ID_TimelinePlayCursor = wxID_HIGHEST + 134;
constexpr int ID_TimelineShapeChanged = wxID_HIGHEST + 135;
constexpr int ID_TimelineZoomChanged = wxID_HIGHEST + 136;
constexpr int ID_ToggleDarkMode = wxID_HIGHEST + 137;
constexpr int ID_FontIncrease = wxID_HIGHEST + 138;
constexpr int ID_FontDecrease = wxID_HIGHEST + 139;
constexpr int ID_FontReset = wxID_HIGHEST + 140;

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

wxString CreateTimelineTempFile(const wxString& prefix) {
    wxFileName tempPrefix(wxStandardPaths::Get().GetTempDir(), prefix);
    const wxString path = wxFileName::CreateTempFileName(tempPrefix.GetFullPath());
    if (path.empty()) throw std::runtime_error("could not create a secure temporary audio file");
    return path;
}

void RemoveFileBestEffort(const wxString& path) {
    if (!path.empty() && wxFileExists(path)) (void)wxRemoveFile(path);
}

wxString DefaultToolPath() {
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
#ifdef __WXMSW__
    const wxString toolName = "neolip_backend.exe";
#else
    const wxString toolName = "neolip_backend";
#endif
    wxFileName candidate(exe.GetPath(), toolName);
    if (candidate.FileExists()) return candidate.GetFullPath();
    return toolName;
}


wxString ExistingFileFromCandidates(const wxArrayString& candidates) {
    for (const auto& c : candidates) {
        if (!c.empty() && wxFileExists(c)) return c;
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

wxString DeriveOutputFile(const wxString& wavPath, const wxString& outDir, const wxString& extensionWithDot) {
    if (wavPath.empty() || outDir.empty()) return wxString();
    wxFileName wav(wavPath);
    return wxFileName(outDir, wav.GetName() + extensionWithDot).GetFullPath();
}

wxString DeriveOutputPhn(const wxString& wavPath, const wxString& outDir) {
    return DeriveOutputFile(wavPath, outDir, ".phn");
}

wxString DeriveOutputLip(const wxString& wavPath, const wxString& outDir) {
    return DeriveOutputFile(wavPath, outDir, ".lip");
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

fs::path ToPath(const wxString& path) {
    return fs::u8path(wxui::toStd(path));
}

wxString KotorShapeName(int shape) {
    if (shape < 0 || shape > 15) return "out of range";
    return wxString::Format("%X: ", shape) +
           wxui::toWx(cslu::kotorLipShapeName(static_cast<std::uint8_t>(shape)));
}

std::vector<GuiLipEntry> ReadStockLipVisibleEntries(const wxString& path, double* durationOut) {
    return cslu::readKotorVisibleKeyframes(ToPath(path), durationOut);
}

void NormalizeVisibleLipEntries(std::vector<GuiLipEntry>& entries) {
    cslu::normalizeKotorVisibleKeyframes(entries);
}

void WriteStockLipVisibleEntries(const wxString& path, std::vector<GuiLipEntry> entries) {
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
    if (path.empty() || !wxFileExists(path)) return {};
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
        : wxFrame(nullptr, wxID_ANY, "NeoLIP", wxDefaultPosition, wxSize(1140, 860)),
          timer_(this, ID_ProcessTimer) {
        SetMinSize(wxSize(980, 700));
        BuildMenus();
        BuildUi();
        LoadConfig();
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
        StopRunningProcess(false);
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

    wxDirPickerCtrl* modelsPicker_ = nullptr;
    wxChoice* recentModels_ = nullptr;
    wxChoice* recentFolders_ = nullptr;
    wxCheckBox* darkMode_ = nullptr;
    wxMenuItem* darkModeMenuItem_ = nullptr;

    wxFilePickerCtrl* wavPicker_ = nullptr;
    wxRadioBox* inputKind_ = nullptr;
    wxFilePickerCtrl* textPicker_ = nullptr;
    wxFilePickerCtrl* ntpPicker_ = nullptr;
    wxTextCtrl* literalText_ = nullptr;
    wxFilePickerCtrl* phnOutputPicker_ = nullptr;
    wxChoice* alignOutputFormat_ = nullptr;
    wxDirPickerCtrl* alignOutputDirPicker_ = nullptr;
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
    wxChoice* batchInputKind_ = nullptr;
    wxChoice* batchOutputKind_ = nullptr;
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
    wxCheckBox* dictWorkdirs_ = nullptr;
    wxComboBox* dictTailRest_ = nullptr;
    wxComboBox* dictClosePause_ = nullptr;
    wxChoice* dictSmoothing_ = nullptr;
    wxCheckBox* dictPreserveClosures_ = nullptr;
    wxComboBox* dictAnticipateClosures_ = nullptr;
    wxComboBox* dictAnticipateFricatives_ = nullptr;
    wxComboBox* dictAnticipateStops_ = nullptr;

    wxFilePickerCtrl* phnPreviewPicker_ = nullptr;
    wxTextCtrl* phnSummary_ = nullptr;
    wxTextCtrl* phnPreviewText_ = nullptr;

    wxFilePickerCtrl* lipEditorOpenPicker_ = nullptr;
    wxFilePickerCtrl* lipEditorSavePicker_ = nullptr;
    wxGrid* lipGrid_ = nullptr;
    wxStaticText* lipEditorStatus_ = nullptr;
    wxTextCtrl* lipShiftAmount_ = nullptr;
    std::vector<GuiLipEntry> lipEditorEntries_;

    wxFilePickerCtrl* timelineAudioPicker_ = nullptr;
    wxFilePickerCtrl* timelineLipPicker_ = nullptr;
    wxFilePickerCtrl* timelinePhnPicker_ = nullptr;
    wxFilePickerCtrl* timelineSavePicker_ = nullptr;
    LipTimelinePanel* timelinePanel_ = nullptr;
    wxStaticText* timelineStatus_ = nullptr;
    wxChoice* timelineShapeChoice_ = nullptr;
    wxSlider* timelineZoom_ = nullptr;
    wxStaticText* timelineSelection_ = nullptr;
    std::vector<GuiLipEntry> timelineEntries_;
    GuiWaveform timelineWaveform_;
    double timelineLipDuration_ = 0.0;
    std::unique_ptr<wxSound> timelineSound_;
    wxString timelinePreviewPath_;

    wxFilePickerCtrl* ntpTextFilePicker_ = nullptr;
    wxTextCtrl* ntpLiteralText_ = nullptr;
    wxFilePickerCtrl* ntpOutputPicker_ = nullptr;
    wxChoice* ntpMode_ = nullptr;
    wxFilePickerCtrl* ntpUserDictPicker_ = nullptr;
    wxCheckBox* ntpUseOgi_ = nullptr;
    wxCheckBox* ntpWriteReports_ = nullptr;

    wxTextCtrl* lookupWord_ = nullptr;
    wxFilePickerCtrl* lookupUserDictPicker_ = nullptr;
    wxCheckBox* lookupUseOgi_ = nullptr;

    wxDirPickerCtrl* traceDumpPicker_ = nullptr;
    wxFilePickerCtrl* traceOutputPhnPicker_ = nullptr;
    wxDirPickerCtrl* traceWorkDirPicker_ = nullptr;

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
        if (darkMode_ != nullptr) darkMode_->SetValue(enabled);
        if (darkModeMenuItem_ != nullptr) darkModeMenuItem_->Check(enabled);
        settings_.setDarkMode(enabled);
        wxui::applyTheme(this, enabled);
        ApplyFontScale();
        if (timelinePanel_ != nullptr) timelinePanel_->Refresh(false);
    }

    void BuildUi() {
        auto* root = new wxBoxSizer(wxVERTICAL);
        SetSizer(root);

        auto* settingsBox = new wxStaticBoxSizer(wxVERTICAL, this, "Package settings");
        auto* settingsGrid = new wxFlexGridSizer(2, 6, 6);
        settingsGrid->AddGrowableCol(1, 1);
        modelsPicker_ = new wxDirPickerCtrl(this, wxID_ANY, wxEmptyString, "Select standalone package root or models folder", wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
        recentModels_ = new wxChoice(this, ID_RecentModelChoice);
        recentFolders_ = new wxChoice(this, ID_RecentFolderChoice);
        darkMode_ = new wxCheckBox(this, ID_ToggleDarkMode, "Dark mode");
        darkMode_->SetToolTip("Use a dark color theme for the GUI. This is a display-only setting; generated files are unchanged.");
        AddPickerRow(this, settingsGrid, "Package/models", modelsPicker_);
        AddTextRow(this, settingsGrid, "Recent model roots", recentModels_);
        AddTextRow(this, settingsGrid, "Recent folders", recentFolders_);
        auto* settingsButtons = new wxBoxSizer(wxHORIZONTAL);
        settingsButtons->Add(new wxButton(this, ID_ValidateModels, "Validate package/models"), 0, wxRIGHT, 6);
        settingsButtons->Add(new wxButton(this, ID_OpenModelFolder, "Open models folder"), 0, wxRIGHT, 6);
        settingsButtons->Add(darkMode_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 10);
        settingsButtons->Add(new wxStaticText(this, wxID_ANY, "Required files are checked before each run."), 0, wxALIGN_CENTER_VERTICAL);
        settingsGrid->AddSpacer(1);
        settingsGrid->Add(settingsButtons, 0, wxEXPAND);
        settingsBox->Add(settingsGrid, 0, wxEXPAND | wxALL, 8);
        root->Add(settingsBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        notebook_ = new wxNotebook(this, wxID_ANY);
        notebook_->AddPage(BuildAlignPage(notebook_), "Align WAV → LIP/PHN", true);
        notebook_->AddPage(BuildBatchPage(notebook_), "Batch", false);
        notebook_->AddPage(BuildDictionaryWorkflowPage(notebook_), "Unknown words", false);
        notebook_->AddPage(BuildPhnPreviewPage(notebook_), "PHN preview", false);
        notebook_->AddPage(BuildLipEditorPage(notebook_), "LIP editor", false);
        notebook_->AddPage(BuildLipTimelinePage(notebook_), "LIP timeline", false);
        notebook_->AddPage(BuildTextToNtpPage(notebook_), "Text → NTP", false);
        notebook_->AddPage(BuildLookupPage(notebook_), "Dictionary lookup", false);
        notebook_->AddPage(BuildTracePage(notebook_), "Trace validation", false);
        root->Add(notebook_, 1, wxEXPAND | wxALL, 8);

        auto* logBox = new wxStaticBoxSizer(wxVERTICAL, this, "Activity log");
        log_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, 220), wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxHSCROLL);
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
        logButtons->Add(stopButton_, 0, wxRIGHT, 6);
        logButtons->Add(copyLogButton_, 0, wxRIGHT, 6);
        logButtons->Add(saveLogButton_, 0, wxRIGHT, 6);
        logButtons->Add(openWorkButton_, 0, wxRIGHT, 6);
        logButtons->AddStretchSpacer(1);
        logButtons->Add(clearButton, 0);
        logBox->Add(logButtons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
        root->Add(logBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        wxui::createStatusBar(*this);
        Centre();
    }

    wxPanel* BuildAlignPage(wxWindow* parent) {
        auto* panel = new wxPanel(parent);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);
        auto* grid = new wxFlexGridSizer(2, 6, 6);
        grid->AddGrowableCol(1, 1);

        wavPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Select dialogue WAV", "WAV files (*.wav)|*.wav|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN | wxFLP_FILE_MUST_EXIST);
        const wxString choices[] = {"Text transcript", "NTP phone transcript"};
        inputKind_ = new wxRadioBox(panel, wxID_ANY, "Input type", wxDefaultPosition, wxDefaultSize, WXSIZEOF(choices), choices, 1, wxRA_SPECIFY_ROWS);
        textPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Select text transcript", "Text files (*.txt;*.lab)|*.txt;*.lab|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        ntpPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Select .ntp phone transcript", "NTP files (*.ntp)|*.ntp|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        literalText_ = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        phnOutputPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Choose output .lip or .phn", "KOTOR LIP files (*.lip)|*.lip|PHN files (*.phn)|*.phn|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT);
        alignOutputFormat_ = new wxChoice(panel, wxID_ANY);
        alignOutputFormat_->Append("LIP");
        alignOutputFormat_->Append("PHN");
        alignOutputFormat_->SetSelection(0);
        alignOutputDirPicker_ = new wxDirPickerCtrl(panel, wxID_ANY, wxEmptyString, "Optional output folder; output name is derived from WAV stem", wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
        workDirPicker_ = new wxDirPickerCtrl(panel, wxID_ANY, wxEmptyString, "Choose optional work directory", wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
        alignMode_ = new wxChoice(panel, wxID_ANY);
        alignMode_->Append("simple");
        alignMode_->Append("cslu");
        alignMode_->SetSelection(0);
        userDictPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Select optional user override dictionary", "Dictionary files (*.dict;*.txt)|*.dict;*.txt|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        useOgi_ = new wxCheckBox(panel, wxID_ANY, "Use OGI/Festival lexicon fallback");
        useOgi_->SetValue(true);
        writeReports_ = new wxCheckBox(panel, wxID_ANY, "Write source/unknown reports in work directory");
        writeReports_->SetValue(true);
        alignTailRest_ = CreateTailRestCombo(panel);
        alignClosePause_ = CreateClosePauseCombo(panel);
        alignSmoothing_ = CreateSmoothingChoice(panel);
        alignPreserveClosures_ = new wxCheckBox(panel, wxID_ANY, "Preserve closures");
        alignPreserveClosures_->SetValue(true);
        alignAnticipateClosures_ = CreateAnticipationCombo(panel, "40", "Start M/P/B closure keys this many milliseconds earlier.");
        alignAnticipateFricatives_ = CreateAnticipationCombo(panel, "25", "Start F/V and TH fricative keys this many milliseconds earlier.");
        alignAnticipateStops_ = CreateAnticipationCombo(panel, "20", "Start T/D and K/G stop keys this many milliseconds earlier.");

        AddPickerRow(panel, grid, "WAV", wavPicker_);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Input type"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(inputKind_, 0, wxEXPAND);
        AddPickerRow(panel, grid, "Text file", textPicker_);
        AddPickerRow(panel, grid, "NTP file", ntpPicker_);
        AddTextRow(panel, grid, "Literal text", literalText_);
        AddPickerRow(panel, grid, "Output file", phnOutputPicker_);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Output format"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(alignOutputFormat_, 0, wxALIGN_LEFT);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Tail rest ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(alignTailRest_, 0, wxALIGN_LEFT);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Close on pause ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(alignClosePause_, 0, wxALIGN_LEFT);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Mouth smoothing"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        auto* alignSmoothRow = new wxBoxSizer(wxHORIZONTAL);
        alignSmoothRow->Add(alignSmoothing_, 0, wxRIGHT, 8);
        alignSmoothRow->Add(alignPreserveClosures_, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(alignSmoothRow, 0, wxEXPAND);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Consonant anticipation ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        auto* alignAntRow = new wxBoxSizer(wxHORIZONTAL);
        alignAntRow->Add(new wxStaticText(panel, wxID_ANY, "M/P/B"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        alignAntRow->Add(alignAnticipateClosures_, 0, wxRIGHT, 10);
        alignAntRow->Add(new wxStaticText(panel, wxID_ANY, "F/V/TH"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        alignAntRow->Add(alignAnticipateFricatives_, 0, wxRIGHT, 10);
        alignAntRow->Add(new wxStaticText(panel, wxID_ANY, "T/D/K/G"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        alignAntRow->Add(alignAnticipateStops_, 0);
        grid->Add(alignAntRow, 0, wxEXPAND);
        AddPickerRow(panel, grid, "Output folder", alignOutputDirPicker_);
        AddPickerRow(panel, grid, "Work dir", workDirPicker_);
        grid->Add(new wxStaticText(panel, wxID_ANY, "NTP mode"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(alignMode_, 0, wxALIGN_LEFT);
        AddPickerRow(panel, grid, "User dict", userDictPicker_);
        grid->AddSpacer(1);
        auto* checkSizer = new wxBoxSizer(wxVERTICAL);
        checkSizer->Add(useOgi_, 0, wxBOTTOM, 4);
        checkSizer->Add(writeReports_, 0);
        grid->Add(checkSizer, 0, wxEXPAND);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);
        outer->Add(new wxStaticLine(panel), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(panel, ID_RunAlign, "Run alignment"), 0, wxRIGHT, 8);
        buttons->Add(new wxButton(panel, ID_OpenOutputFolder, "Open output folder"), 0, wxRIGHT, 8);
        buttons->Add(new wxStaticText(panel, wxID_ANY, "Default output is KOTOR .lip. If Output file is blank, the output folder is used with the WAV stem."), 0, wxALIGN_CENTER_VERTICAL);
        buttons->AddStretchSpacer(1);
        outer->Add(buttons, 0, wxEXPAND | wxALL, 10);
        UpdateAlignVisibility();
        return panel;
    }

    wxPanel* BuildBatchPage(wxWindow* parent) {
        auto* panel = new wxPanel(parent);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);
        auto* grid = new wxFlexGridSizer(2, 6, 6);
        grid->AddGrowableCol(1, 1);
        batchInputDirPicker_ = new wxDirPickerCtrl(panel, wxID_ANY, wxEmptyString, "Select folder containing WAV/TXT or WAV/NTP pairs", wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
        batchOutputDirPicker_ = new wxDirPickerCtrl(panel, wxID_ANY, wxEmptyString, "Select output folder", wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
        batchInputKind_ = new wxChoice(panel, wxID_ANY);
        batchInputKind_->Append("text");
        batchInputKind_->Append("ntp");
        batchInputKind_->Append("auto");
        batchInputKind_->SetSelection(0);
        batchOutputKind_ = new wxChoice(panel, wxID_ANY);
        batchOutputKind_->Append("lip");
        batchOutputKind_->Append("phn");
        batchOutputKind_->Append("both");
        batchOutputKind_->SetSelection(0);
        batchMode_ = new wxChoice(panel, wxID_ANY);
        batchMode_->Append("simple");
        batchMode_->Append("cslu");
        batchMode_->SetSelection(0);
        batchUserDictPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Select optional user override dictionary", "Dictionary files (*.dict;*.txt)|*.dict;*.txt|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        batchUseOgi_ = new wxCheckBox(panel, wxID_ANY, "Use OGI/Festival lexicon fallback");
        batchUseOgi_->SetValue(true);
        batchRecursive_ = new wxCheckBox(panel, wxID_ANY, "Scan subfolders recursively");
        batchWorkdirs_ = new wxCheckBox(panel, wxID_ANY, "Write per-line work folders");
        batchWorkdirs_->SetValue(true);
        batchFailFast_ = new wxCheckBox(panel, wxID_ANY, "Stop at first failed file");
        batchResume_ = new wxCheckBox(panel, wxID_ANY, "Resume/skip valid existing outputs");
        batchJobs_ = new wxTextCtrl(panel, wxID_ANY, "auto");
        batchJobs_->SetToolTip("Parallel jobs. Use auto, 1, 2, 4, etc. Auto caps at a safe default.");
        batchDecodeCachePicker_ = new wxDirPickerCtrl(panel, wxID_ANY, wxEmptyString, "Optional decoded-audio cache folder", wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
        batchTailRest_ = CreateTailRestCombo(panel);
        batchClosePause_ = CreateClosePauseCombo(panel);
        batchSmoothing_ = CreateSmoothingChoice(panel);
        batchPreserveClosures_ = new wxCheckBox(panel, wxID_ANY, "Preserve closures");
        batchPreserveClosures_->SetValue(true);
        batchAnticipateClosures_ = CreateAnticipationCombo(panel, "40", "Start M/P/B closure keys this many milliseconds earlier.");
        batchAnticipateFricatives_ = CreateAnticipationCombo(panel, "25", "Start F/V and TH fricative keys this many milliseconds earlier.");
        batchAnticipateStops_ = CreateAnticipationCombo(panel, "20", "Start T/D and K/G stop keys this many milliseconds earlier.");
        AddPickerRow(panel, grid, "Input folder", batchInputDirPicker_);
        AddPickerRow(panel, grid, "Output folder", batchOutputDirPicker_);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Input kind"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(batchInputKind_, 0, wxALIGN_LEFT);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Output kind"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(batchOutputKind_, 0, wxALIGN_LEFT);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Tail rest ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(batchTailRest_, 0, wxALIGN_LEFT);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Close on pause ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(batchClosePause_, 0, wxALIGN_LEFT);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Mouth smoothing"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        auto* batchSmoothRow = new wxBoxSizer(wxHORIZONTAL);
        batchSmoothRow->Add(batchSmoothing_, 0, wxRIGHT, 8);
        batchSmoothRow->Add(batchPreserveClosures_, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(batchSmoothRow, 0, wxEXPAND);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Consonant anticipation ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        auto* batchAntRow = new wxBoxSizer(wxHORIZONTAL);
        batchAntRow->Add(new wxStaticText(panel, wxID_ANY, "M/P/B"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        batchAntRow->Add(batchAnticipateClosures_, 0, wxRIGHT, 10);
        batchAntRow->Add(new wxStaticText(panel, wxID_ANY, "F/V/TH"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        batchAntRow->Add(batchAnticipateFricatives_, 0, wxRIGHT, 10);
        batchAntRow->Add(new wxStaticText(panel, wxID_ANY, "T/D/K/G"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        batchAntRow->Add(batchAnticipateStops_, 0);
        grid->Add(batchAntRow, 0, wxEXPAND);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Text/NTP mode"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(batchMode_, 0, wxALIGN_LEFT);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Parallel jobs"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(batchJobs_, 0, wxEXPAND);
        AddPickerRow(panel, grid, "Decoded audio cache", batchDecodeCachePicker_);
        AddPickerRow(panel, grid, "User dict", batchUserDictPicker_);
        grid->AddSpacer(1);
        auto* checks = new wxBoxSizer(wxVERTICAL);
        checks->Add(batchUseOgi_, 0, wxBOTTOM, 4);
        checks->Add(batchRecursive_, 0, wxBOTTOM, 4);
        checks->Add(batchWorkdirs_, 0, wxBOTTOM, 4);
        checks->Add(batchFailFast_, 0, wxBOTTOM, 4);
        checks->Add(batchResume_, 0);
        grid->Add(checks, 0, wxEXPAND);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);
        outer->Add(new wxStaticLine(panel), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(panel, ID_RunBatch, "Run batch"), 0, wxRIGHT, 8);
        buttons->Add(new wxStaticText(panel, wxID_ANY, "Writes batch_report.tsv and unknown_words.tsv. Progress advances per WAV."), 0, wxALIGN_CENTER_VERTICAL);
        buttons->AddStretchSpacer(1);
        outer->Add(buttons, 0, wxEXPAND | wxALL, 10);
        return panel;
    }

    wxPanel* BuildDictionaryWorkflowPage(wxWindow* parent) {
        auto* panel = new wxPanel(parent);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);
        auto* grid = new wxFlexGridSizer(2, 6, 6);
        grid->AddGrowableCol(1, 1);
        dictInputDirPicker_ = new wxDirPickerCtrl(panel, wxID_ANY, wxEmptyString, "Select folder containing transcript .txt/.lab files", wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
        dictOutputDirPicker_ = new wxDirPickerCtrl(panel, wxID_ANY, wxEmptyString, "Select folder for scan reports and batch output", wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
        dictUnknownReportPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Choose unknown_words.tsv", "TSV files (*.tsv)|*.tsv|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT);
        dictSourceReportPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Choose pronunciations/source report", "TSV files (*.tsv)|*.tsv|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT);
        dictOverrideDictPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Choose KOTOR override dictionary", "Dictionary files (*.dict;*.txt)|*.dict;*.txt|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_SAVE);
        dictMode_ = new wxChoice(panel, wxID_ANY);
        dictMode_->Append("simple");
        dictMode_->Append("cslu");
        dictMode_->SetSelection(0);
        dictUseOgi_ = new wxCheckBox(panel, wxID_ANY, "Use OGI/Festival lexicon fallback");
        dictUseOgi_->SetValue(true);
        dictRecursive_ = new wxCheckBox(panel, wxID_ANY, "Scan subfolders recursively");
        dictWorkdirs_ = new wxCheckBox(panel, wxID_ANY, "Write per-line work folders when rerunning batch");
        dictWorkdirs_->SetValue(true);
        dictTailRest_ = CreateTailRestCombo(panel);
        dictClosePause_ = CreateClosePauseCombo(panel);
        dictSmoothing_ = CreateSmoothingChoice(panel);
        dictPreserveClosures_ = new wxCheckBox(panel, wxID_ANY, "Preserve closures");
        dictPreserveClosures_->SetValue(true);
        dictAnticipateClosures_ = CreateAnticipationCombo(panel, "40", "Start M/P/B closure keys this many milliseconds earlier.");
        dictAnticipateFricatives_ = CreateAnticipationCombo(panel, "25", "Start F/V and TH fricative keys this many milliseconds earlier.");
        dictAnticipateStops_ = CreateAnticipationCombo(panel, "20", "Start T/D and K/G stop keys this many milliseconds earlier.");
        AddPickerRow(panel, grid, "Transcript folder", dictInputDirPicker_);
        AddPickerRow(panel, grid, "Output folder", dictOutputDirPicker_);
        AddPickerRow(panel, grid, "Unknown report", dictUnknownReportPicker_);
        AddPickerRow(panel, grid, "Source report", dictSourceReportPicker_);
        AddPickerRow(panel, grid, "Override dict", dictOverrideDictPicker_);
        grid->Add(new wxStaticText(panel, wxID_ANY, "NTP mode"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(dictMode_, 0, wxALIGN_LEFT);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Tail rest ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(dictTailRest_, 0, wxALIGN_LEFT);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Close on pause ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(dictClosePause_, 0, wxALIGN_LEFT);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Mouth smoothing"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        auto* dictSmoothRow = new wxBoxSizer(wxHORIZONTAL);
        dictSmoothRow->Add(dictSmoothing_, 0, wxRIGHT, 8);
        dictSmoothRow->Add(dictPreserveClosures_, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(dictSmoothRow, 0, wxEXPAND);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Consonant anticipation ms"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        auto* dictAntRow = new wxBoxSizer(wxHORIZONTAL);
        dictAntRow->Add(new wxStaticText(panel, wxID_ANY, "M/P/B"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        dictAntRow->Add(dictAnticipateClosures_, 0, wxRIGHT, 10);
        dictAntRow->Add(new wxStaticText(panel, wxID_ANY, "F/V/TH"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        dictAntRow->Add(dictAnticipateFricatives_, 0, wxRIGHT, 10);
        dictAntRow->Add(new wxStaticText(panel, wxID_ANY, "T/D/K/G"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        dictAntRow->Add(dictAnticipateStops_, 0);
        grid->Add(dictAntRow, 0, wxEXPAND);
        grid->AddSpacer(1);
        auto* checks = new wxBoxSizer(wxVERTICAL);
        checks->Add(dictUseOgi_, 0, wxBOTTOM, 4);
        checks->Add(dictRecursive_, 0, wxBOTTOM, 4);
        checks->Add(dictWorkdirs_, 0);
        grid->Add(checks, 0, wxEXPAND);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);
        outer->Add(new wxStaticLine(panel), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
        outer->Add(new wxStaticText(panel, wxID_ANY, "Scan transcripts, append empty stubs, edit pronunciations, then rerun the batch."), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
        auto* buttons1 = new wxBoxSizer(wxHORIZONTAL);
        buttons1->Add(new wxButton(panel, ID_ScanUnknowns, "Scan unknown words"), 0, wxRIGHT, 8);
        buttons1->Add(new wxButton(panel, ID_AppendDictStubs, "Generate/append dictionary stubs"), 0, wxRIGHT, 8);
        buttons1->Add(new wxButton(panel, ID_OpenUnknownReport, "Open unknown report"), 0, wxRIGHT, 8);
        buttons1->Add(new wxButton(panel, ID_OpenOverrideDict, "Open override dictionary"), 0, wxRIGHT, 8);
        buttons1->AddStretchSpacer(1);
        outer->Add(buttons1, 0, wxEXPAND | wxALL, 10);
        auto* buttons2 = new wxBoxSizer(wxHORIZONTAL);
        buttons2->Add(new wxButton(panel, ID_RunDictionaryBatch, "Rerun batch with override dictionary"), 0, wxRIGHT, 8);
        buttons2->Add(new wxStaticText(panel, wxID_ANY, "Uses edited override dictionary and selected transcript folder."), 0, wxALIGN_CENTER_VERTICAL);
        buttons2->AddStretchSpacer(1);
        outer->Add(buttons2, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        return panel;
    }

    wxPanel* BuildPhnPreviewPage(wxWindow* parent) {
        auto* panel = new wxPanel(parent);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);
        auto* grid = new wxFlexGridSizer(2, 6, 6);
        grid->AddGrowableCol(1, 1);
        phnPreviewPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Select PHN file", "PHN files (*.phn)|*.phn|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        AddPickerRow(panel, grid, "PHN file", phnPreviewPicker_);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(panel, ID_LoadPhnPreview, "Load PHN"), 0, wxRIGHT, 8);
        buttons->Add(new wxButton(panel, ID_LoadLastPhnPreview, "Load last output PHN"), 0, wxRIGHT, 8);
        buttons->AddStretchSpacer(1);
        outer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        phnSummary_ = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, 100), wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
        phnPreviewText_ = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
        outer->Add(phnSummary_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        outer->Add(phnPreviewText_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        return panel;
    }

    wxPanel* BuildLipEditorPage(wxWindow* parent) {
        auto* panel = new wxPanel(parent);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);

        auto* grid = new wxFlexGridSizer(2, 6, 6);
        grid->AddGrowableCol(1, 1);
        lipEditorOpenPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Open KOTOR LIP file", "KOTOR LIP files (*.lip)|*.lip|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        lipEditorSavePicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Save KOTOR LIP file", "KOTOR LIP files (*.lip)|*.lip|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT);
        AddPickerRow(panel, grid, "Open LIP", lipEditorOpenPicker_);
        AddPickerRow(panel, grid, "Save as", lipEditorSavePicker_);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(panel, ID_LoadLipEditor, "Load LIP"), 0, wxRIGHT, 8);
        buttons->Add(new wxButton(panel, ID_SaveLipEditor, "Save LIP"), 0, wxRIGHT, 8);
        buttons->Add(new wxButton(panel, ID_InsertLipKey, "Insert key"), 0, wxRIGHT, 8);
        buttons->Add(new wxButton(panel, ID_DeleteLipKeys, "Delete selected"), 0, wxRIGHT, 8);
        lipShiftAmount_ = new wxTextCtrl(panel, wxID_ANY, "0.050", wxDefaultPosition, wxSize(80, -1));
        buttons->Add(new wxStaticText(panel, wxID_ANY, "Shift seconds"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        buttons->Add(lipShiftAmount_, 0, wxRIGHT, 8);
        buttons->Add(new wxButton(panel, ID_ShiftLipKeys, "Shift selected"), 0, wxRIGHT, 8);
        buttons->Add(new wxButton(panel, ID_NormalizeLipKeys, "Normalize final rest"), 0, wxRIGHT, 8);
        buttons->AddStretchSpacer(1);
        outer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        lipEditorStatus_ = new wxStaticText(panel, wxID_ANY, "Load a .lip file to edit key times and mouth shapes.");
        outer->Add(lipEditorStatus_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        lipGrid_ = new wxGrid(panel, wxID_ANY);
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
        outer->Add(lipGrid_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        auto* note = new wxStaticText(panel, wxID_ANY,
            "The table edits visible KOTOR mouth keys. The hidden first rest key is preserved automatically. Saving normalizes the final key to shape 0.");
        outer->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        return panel;
    }

    wxPanel* BuildLipTimelinePage(wxWindow* parent) {
        auto* panel = new wxPanel(parent);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);
        auto* grid = new wxFlexGridSizer(2, 6, 6);
        grid->AddGrowableCol(1, 1);
        timelineAudioPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Select source audio", "Audio files (*.wav;*.mp3)|*.wav;*.mp3|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        timelineLipPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Open KOTOR LIP file", "KOTOR LIP files (*.lip)|*.lip|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        timelinePhnPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Optional PHN file", "PHN files (*.phn)|*.phn|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        timelineSavePicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Save edited KOTOR LIP", "KOTOR LIP files (*.lip)|*.lip|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT);
        AddPickerRow(panel, grid, "Audio", timelineAudioPicker_);
        AddPickerRow(panel, grid, "LIP", timelineLipPicker_);
        AddPickerRow(panel, grid, "PHN optional", timelinePhnPicker_);
        AddPickerRow(panel, grid, "Save as", timelineSavePicker_);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);

        auto* controls = new wxBoxSizer(wxHORIZONTAL);
        controls->Add(new wxButton(panel, ID_LoadLipTimeline, "Load timeline"), 0, wxRIGHT, 8);
        controls->Add(new wxButton(panel, ID_TimelineLoadFromEditor, "Use LIP editor data"), 0, wxRIGHT, 8);
        controls->Add(new wxButton(panel, ID_TimelinePlayCursor, "Play from cursor"), 0, wxRIGHT, 8);
        controls->Add(new wxButton(panel, ID_SaveLipTimeline, "Export edited .lip"), 0, wxRIGHT, 8);
        controls->Add(new wxButton(panel, ID_TimelineNormalize, "Normalize final rest"), 0, wxRIGHT, 8);
        controls->Add(new wxStaticText(panel, wxID_ANY, "Shape"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        timelineShapeChoice_ = new wxChoice(panel, ID_TimelineShapeChanged);
        for (int i = 0; i < 16; ++i) timelineShapeChoice_->Append(KotorShapeName(i));
        timelineShapeChoice_->SetSelection(0);
        controls->Add(timelineShapeChoice_, 0, wxRIGHT, 12);
        controls->Add(new wxStaticText(panel, wxID_ANY, "Zoom"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        timelineZoom_ = new wxSlider(panel, ID_TimelineZoomChanged, 1, 1, 20, wxDefaultPosition, wxSize(150, -1), wxSL_HORIZONTAL);
        controls->Add(timelineZoom_, 0, wxRIGHT, 12);
        controls->AddStretchSpacer(1);
        outer->Add(controls, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        timelineSelection_ = new wxStaticText(panel, wxID_ANY, "No key selected.");
        timelineStatus_ = new wxStaticText(panel, wxID_ANY, "Load audio plus a .lip file. Optional PHN intervals appear in the bottom lane.");
        outer->Add(timelineSelection_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
        outer->Add(timelineStatus_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        timelinePanel_ = new LipTimelinePanel(panel);
        timelinePanel_->SetSelectionCallback([this](int idx, const GuiLipEntry* entry) {
            if (entry) {
                timelineSelection_->SetLabel(wxString::Format("selected key %d   time %.3fs   shape %s", idx, entry->timeSeconds, KotorShapeName(entry->shape)));
                if (timelineShapeChoice_) timelineShapeChoice_->SetSelection(entry->shape);
                if (idx >= 0 && idx < static_cast<int>(timelineEntries_.size())) timelineEntries_[static_cast<std::size_t>(idx)] = *entry;
            } else {
                timelineSelection_->SetLabel("No key selected.");
            }
            UpdateTimelineStatus();
        });
        outer->Add(timelinePanel_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        return panel;
    }

    wxPanel* BuildTextToNtpPage(wxWindow* parent) {
        auto* panel = new wxPanel(parent);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);
        auto* grid = new wxFlexGridSizer(2, 6, 6);
        grid->AddGrowableCol(1, 1);
        ntpTextFilePicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Select text transcript", "Text files (*.txt;*.lab)|*.txt;*.lab|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        ntpLiteralText_ = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        ntpOutputPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Choose output .ntp", "NTP files (*.ntp)|*.ntp|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT);
        ntpMode_ = new wxChoice(panel, wxID_ANY);
        ntpMode_->Append("simple");
        ntpMode_->Append("cslu");
        ntpMode_->SetSelection(0);
        ntpUserDictPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Select optional user override dictionary", "Dictionary files (*.dict;*.txt)|*.dict;*.txt|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        ntpUseOgi_ = new wxCheckBox(panel, wxID_ANY, "Use OGI/Festival lexicon fallback");
        ntpUseOgi_->SetValue(true);
        ntpWriteReports_ = new wxCheckBox(panel, wxID_ANY, "Write source/unknown reports next to output NTP");
        ntpWriteReports_->SetValue(true);
        AddPickerRow(panel, grid, "Text file", ntpTextFilePicker_);
        AddTextRow(panel, grid, "Literal text", ntpLiteralText_);
        AddPickerRow(panel, grid, "Output NTP", ntpOutputPicker_);
        grid->Add(new wxStaticText(panel, wxID_ANY, "Mode"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        grid->Add(ntpMode_, 0, wxALIGN_LEFT);
        AddPickerRow(panel, grid, "User dict", ntpUserDictPicker_);
        grid->AddSpacer(1);
        auto* checks = new wxBoxSizer(wxVERTICAL);
        checks->Add(ntpUseOgi_, 0, wxBOTTOM, 4);
        checks->Add(ntpWriteReports_, 0);
        grid->Add(checks, 0, wxEXPAND);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);
        outer->Add(new wxStaticLine(panel), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(panel, ID_RunTextToNtp, "Generate NTP"), 0, wxRIGHT, 8);
        buttons->Add(new wxStaticText(panel, wxID_ANY, "Use this to inspect or edit phone transcripts before alignment."), 0, wxALIGN_CENTER_VERTICAL);
        buttons->AddStretchSpacer(1);
        outer->Add(buttons, 0, wxEXPAND | wxALL, 10);
        return panel;
    }

    wxPanel* BuildLookupPage(wxWindow* parent) {
        auto* panel = new wxPanel(parent);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);
        auto* grid = new wxFlexGridSizer(2, 6, 6);
        grid->AddGrowableCol(1, 1);
        lookupWord_ = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        lookupUserDictPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Select optional user override dictionary", "Dictionary files (*.dict;*.txt)|*.dict;*.txt|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);
        lookupUseOgi_ = new wxCheckBox(panel, wxID_ANY, "Use OGI/Festival lexicon fallback");
        lookupUseOgi_->SetValue(true);
        AddTextRow(panel, grid, "Word", lookupWord_);
        AddPickerRow(panel, grid, "User dict", lookupUserDictPicker_);
        grid->AddSpacer(1);
        grid->Add(lookupUseOgi_, 0, wxEXPAND);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);
        outer->Add(new wxStaticLine(panel), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(panel, ID_RunLookup, "Lookup word"), 0, wxRIGHT, 8);
        buttons->Add(new wxStaticText(panel, wxID_ANY, "Use this when building KOTOR override pronunciations."), 0, wxALIGN_CENTER_VERTICAL);
        buttons->AddStretchSpacer(1);
        outer->Add(buttons, 0, wxEXPAND | wxALL, 10);
        return panel;
    }

    wxPanel* BuildTracePage(wxWindow* parent) {
        auto* panel = new wxPanel(parent);
        auto* outer = new wxBoxSizer(wxVERTICAL);
        panel->SetSizer(outer);
        auto* grid = new wxFlexGridSizer(2, 6, 6);
        grid->AddGrowableCol(1, 1);
        traceDumpPicker_ = new wxDirPickerCtrl(panel, wxID_ANY, wxEmptyString, "Select Phase 2 trace dump folder", wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
        traceOutputPhnPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxEmptyString, "Choose output trace PHN", "PHN files (*.phn)|*.phn|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT);
        traceWorkDirPicker_ = new wxDirPickerCtrl(panel, wxID_ANY, wxEmptyString, "Choose work directory", wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL);
        AddPickerRow(panel, grid, "Trace dump", traceDumpPicker_);
        AddPickerRow(panel, grid, "Output PHN", traceOutputPhnPicker_);
        AddPickerRow(panel, grid, "Work dir", traceWorkDirPicker_);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);
        outer->Add(new wxStaticLine(panel), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(panel, ID_RunCompareTrace, "Run end-to-end trace validation"), 0, wxRIGHT, 8);
        buttons->Add(new wxStaticText(panel, wxID_ANY, "This should match the CSLU trace PHN exactly."), 0, wxALIGN_CENTER_VERTICAL);
        buttons->AddStretchSpacer(1);
        outer->Add(buttons, 0, wxEXPAND | wxALL, 10);
        return panel;
    }

    void BindEvents() {
        Bind(wxEVT_BUTTON, &MainFrame::OnRunAlign, this, ID_RunAlign);
        Bind(wxEVT_BUTTON, &MainFrame::OnRunTextToNtp, this, ID_RunTextToNtp);
        Bind(wxEVT_BUTTON, &MainFrame::OnRunBatch, this, ID_RunBatch);
        Bind(wxEVT_BUTTON, &MainFrame::OnScanUnknowns, this, ID_ScanUnknowns);
        Bind(wxEVT_BUTTON, &MainFrame::OnAppendDictStubs, this, ID_AppendDictStubs);
        Bind(wxEVT_BUTTON, &MainFrame::OnOpenUnknownReport, this, ID_OpenUnknownReport);
        Bind(wxEVT_BUTTON, &MainFrame::OnOpenOverrideDict, this, ID_OpenOverrideDict);
        Bind(wxEVT_BUTTON, &MainFrame::OnRunDictionaryBatch, this, ID_RunDictionaryBatch);
        Bind(wxEVT_BUTTON, &MainFrame::OnRunLookup, this, ID_RunLookup);
        Bind(wxEVT_BUTTON, &MainFrame::OnRunCompareTrace, this, ID_RunCompareTrace);
        Bind(wxEVT_BUTTON, &MainFrame::OnStop, this, ID_StopProcess);
        Bind(wxEVT_BUTTON, &MainFrame::OnClearLog, this, ID_ClearLog);
        Bind(wxEVT_BUTTON, &MainFrame::OnCopyLog, this, ID_CopyLog);
        Bind(wxEVT_BUTTON, &MainFrame::OnSaveLog, this, ID_SaveLog);
        Bind(wxEVT_BUTTON, &MainFrame::OnOpenWorkDir, this, ID_OpenWorkDir);
        Bind(wxEVT_BUTTON, &MainFrame::OnValidateModels, this, ID_ValidateModels);
        Bind(wxEVT_BUTTON, &MainFrame::OnLoadPhnPreview, this, ID_LoadPhnPreview);
        Bind(wxEVT_BUTTON, &MainFrame::OnLoadLastPhnPreview, this, ID_LoadLastPhnPreview);
        Bind(wxEVT_BUTTON, &MainFrame::OnLoadLipEditor, this, ID_LoadLipEditor);
        Bind(wxEVT_BUTTON, &MainFrame::OnSaveLipEditor, this, ID_SaveLipEditor);
        Bind(wxEVT_BUTTON, &MainFrame::OnInsertLipKey, this, ID_InsertLipKey);
        Bind(wxEVT_BUTTON, &MainFrame::OnDeleteLipKeys, this, ID_DeleteLipKeys);
        Bind(wxEVT_BUTTON, &MainFrame::OnShiftLipKeys, this, ID_ShiftLipKeys);
        Bind(wxEVT_BUTTON, &MainFrame::OnNormalizeLipKeys, this, ID_NormalizeLipKeys);
        Bind(wxEVT_BUTTON, &MainFrame::OnLoadLipTimeline, this, ID_LoadLipTimeline);
        Bind(wxEVT_BUTTON, &MainFrame::OnSaveLipTimeline, this, ID_SaveLipTimeline);
        Bind(wxEVT_BUTTON, &MainFrame::OnTimelineLoadFromEditor, this, ID_TimelineLoadFromEditor);
        Bind(wxEVT_BUTTON, &MainFrame::OnTimelineNormalize, this, ID_TimelineNormalize);
        Bind(wxEVT_BUTTON, &MainFrame::OnTimelinePlayCursor, this, ID_TimelinePlayCursor);
        Bind(wxEVT_CHOICE, &MainFrame::OnTimelineShapeChanged, this, ID_TimelineShapeChanged);
        Bind(wxEVT_SLIDER, &MainFrame::OnTimelineZoomChanged, this, ID_TimelineZoomChanged);
        Bind(wxEVT_BUTTON, &MainFrame::OnOpenOutputFolder, this, ID_OpenOutputFolder);
        Bind(wxEVT_BUTTON, &MainFrame::OnOpenModelFolder, this, ID_OpenModelFolder);
        Bind(wxEVT_CHECKBOX, &MainFrame::OnToggleDarkMode, this, ID_ToggleDarkMode);
        Bind(wxEVT_MENU, &MainFrame::OnToggleDarkMode, this, ID_ToggleDarkMode);
        Bind(wxEVT_MENU, &MainFrame::OnIncreaseFontScale, this, ID_FontIncrease);
        Bind(wxEVT_MENU, &MainFrame::OnDecreaseFontScale, this, ID_FontDecrease);
        Bind(wxEVT_MENU, &MainFrame::OnResetFontScale, this, ID_FontReset);
        Bind(wxEVT_TIMER, &MainFrame::OnTimer, this, ID_ProcessTimer);
        Bind(wxEVT_END_PROCESS, &MainFrame::OnProcessTerminated, this);
        recentModels_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            if (recentModels_->GetSelection() != wxNOT_FOUND) modelsPicker_->SetPath(recentModels_->GetStringSelection());
            ApplyDefaultKotorOverrideIfAvailable();
        });
        modelsPicker_->Bind(wxEVT_DIRPICKER_CHANGED, [this](wxFileDirPickerEvent&) { ApplyDefaultKotorOverrideIfAvailable(); });
        recentFolders_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            if (recentFolders_->GetSelection() != wxNOT_FOUND) {
                const wxString p = recentFolders_->GetStringSelection();
                if (notebook_->GetSelection() == 1) batchInputDirPicker_->SetPath(p);
                else if (notebook_->GetSelection() == 2) dictInputDirPicker_->SetPath(p);
                else alignOutputDirPicker_->SetPath(p);
            }
        });
        inputKind_->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent&) { UpdateAlignVisibility(); });
        alignOutputFormat_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { phnOutputPicker_->SetPath(wxEmptyString); MaybeDeriveAlignOutput(); });
        phnOutputPicker_->Bind(wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent&) {
            if (workDirPicker_->GetPath().empty()) workDirPicker_->SetPath(DefaultWorkDirForOutput(phnOutputPicker_->GetPath()));
        });
        wavPicker_->Bind(wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent&) { MaybeDeriveAlignOutput(); });
        alignOutputDirPicker_->Bind(wxEVT_DIRPICKER_CHANGED, [this](wxFileDirPickerEvent&) { MaybeDeriveAlignOutput(); });
        dictOutputDirPicker_->Bind(wxEVT_DIRPICKER_CHANGED, [this](wxFileDirPickerEvent&) { UpdateDictionaryDefaults(); });
    }

    void OnToggleDarkMode(wxCommandEvent& event) {
        const bool enabled = event.GetEventType() == wxEVT_MENU
            ? event.IsChecked()
            : (darkMode_ != nullptr && darkMode_->GetValue());
        ApplyDarkMode(enabled);
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
            int x = 0, y = 0, w = 1140, h = 860;
            if (config.Read("window_x", &x) && config.Read("window_y", &y) &&
                config.Read("window_w", &w) && config.Read("window_h", &h)) {
                SetSize(x, y, w, h);
                settings_.saveWindowPlacement(*this);
            }
        }
        modelsPicker_->SetPath(config.Read("models_root", wxEmptyString));
        auto recentModels = settings_.recentPaths("ModelRoots", 12, {"recent_models"});
        auto recentFolders = settings_.recentPaths("Folders", 12, {"recent_folders"});
        if (recentModels.empty()) recentModels = ParseLegacyPathList(config.Read("recent_models", wxEmptyString));
        if (recentFolders.empty()) recentFolders = ParseLegacyPathList(config.Read("recent_folders", wxEmptyString));
        settings_.setRecentPaths("ModelRoots", recentModels, 12);
        settings_.setRecentPaths("Folders", recentFolders, 12);
        LoadChoiceFromPaths(recentModels_, recentModels);
        LoadChoiceFromPaths(recentFolders_, recentFolders);
        userDictPicker_->SetPath(config.Read("user_dict", wxEmptyString));
        batchUserDictPicker_->SetPath(config.Read("user_dict", wxEmptyString));
        dictOverrideDictPicker_->SetPath(config.Read("user_dict", wxEmptyString));
        ntpUserDictPicker_->SetPath(config.Read("user_dict", wxEmptyString));
        lookupUserDictPicker_->SetPath(config.Read("user_dict", wxEmptyString));
        ApplyDefaultKotorOverrideIfAvailable();
        useOgi_->SetValue(config.ReadBool("use_ogi", true));
        batchUseOgi_->SetValue(config.ReadBool("use_ogi", true));
        dictUseOgi_->SetValue(config.ReadBool("use_ogi", true));
        ntpUseOgi_->SetValue(config.ReadBool("use_ogi", true));
        lookupUseOgi_->SetValue(config.ReadBool("use_ogi", true));
        alignMode_->SetStringSelection(config.Read("mode", "simple"));
        alignOutputFormat_->SetStringSelection(config.Read("align_output_format", "LIP"));
        batchOutputKind_->SetStringSelection(config.Read("batch_output_kind", "lip"));
        alignTailRest_->SetValue(config.Read("tail_rest_ms", "0"));
        alignClosePause_->SetValue(config.Read("close_on_pause_ms", "160"));
        alignSmoothing_->SetStringSelection(config.Read("lip_smoothing", "light"));
        alignPreserveClosures_->SetValue(config.ReadBool("preserve_closures", true));
        alignAnticipateClosures_->SetValue(config.Read("anticipate_closures_ms", "40"));
        alignAnticipateFricatives_->SetValue(config.Read("anticipate_fricatives_ms", "25"));
        alignAnticipateStops_->SetValue(config.Read("anticipate_stops_ms", "20"));
        batchTailRest_->SetValue(config.Read("batch_tail_rest_ms", config.Read("tail_rest_ms", "0")));
        batchClosePause_->SetValue(config.Read("batch_close_on_pause_ms", config.Read("close_on_pause_ms", "160")));
        batchSmoothing_->SetStringSelection(config.Read("batch_lip_smoothing", config.Read("lip_smoothing", "light")));
        batchPreserveClosures_->SetValue(config.ReadBool("batch_preserve_closures", config.ReadBool("preserve_closures", true)));
        batchAnticipateClosures_->SetValue(config.Read("batch_anticipate_closures_ms", config.Read("anticipate_closures_ms", "40")));
        batchAnticipateFricatives_->SetValue(config.Read("batch_anticipate_fricatives_ms", config.Read("anticipate_fricatives_ms", "25")));
        batchAnticipateStops_->SetValue(config.Read("batch_anticipate_stops_ms", config.Read("anticipate_stops_ms", "20")));
        batchJobs_->SetValue(config.Read("batch_jobs", "auto"));
        batchResume_->SetValue(config.ReadBool("batch_resume", false));
        batchDecodeCachePicker_->SetPath(config.Read("batch_decode_cache", wxEmptyString));
        dictTailRest_->SetValue(config.Read("dict_tail_rest_ms", config.Read("tail_rest_ms", "0")));
        dictClosePause_->SetValue(config.Read("dict_close_on_pause_ms", config.Read("close_on_pause_ms", "160")));
        dictSmoothing_->SetStringSelection(config.Read("dict_lip_smoothing", config.Read("lip_smoothing", "light")));
        dictPreserveClosures_->SetValue(config.ReadBool("dict_preserve_closures", config.ReadBool("preserve_closures", true)));
        dictAnticipateClosures_->SetValue(config.Read("dict_anticipate_closures_ms", config.Read("anticipate_closures_ms", "40")));
        dictAnticipateFricatives_->SetValue(config.Read("dict_anticipate_fricatives_ms", config.Read("anticipate_fricatives_ms", "25")));
        dictAnticipateStops_->SetValue(config.Read("dict_anticipate_stops_ms", config.Read("anticipate_stops_ms", "20")));
        batchMode_->SetStringSelection(config.Read("mode", "simple"));
        dictMode_->SetStringSelection(config.Read("mode", "simple"));
        ntpMode_->SetStringSelection(config.Read("mode", "simple"));
        batchInputDirPicker_->SetPath(config.Read("batch_input_dir", wxEmptyString));
        batchOutputDirPicker_->SetPath(config.Read("batch_output_dir", wxEmptyString));
        dictInputDirPicker_->SetPath(config.Read("dict_input_dir", wxEmptyString));
        dictOutputDirPicker_->SetPath(config.Read("dict_output_dir", wxEmptyString));
        alignOutputDirPicker_->SetPath(config.Read("align_output_dir", wxEmptyString));
        phnPreviewPicker_->SetPath(config.Read("phn_preview", wxEmptyString));
        lipEditorOpenPicker_->SetPath(config.Read("lip_editor_open", wxEmptyString));
        lipEditorSavePicker_->SetPath(config.Read("lip_editor_save", wxEmptyString));
        timelineAudioPicker_->SetPath(config.Read("timeline_audio", wxEmptyString));
        timelineLipPicker_->SetPath(config.Read("timeline_lip", wxEmptyString));
        timelinePhnPicker_->SetPath(config.Read("timeline_phn", wxEmptyString));
        timelineSavePicker_->SetPath(config.Read("timeline_save", wxEmptyString));
        fontScale_ = settings_.fontScale();
        bool darkMode = settings_.darkMode();
        if (!settings_.hasEntry("UI/DarkMode")) darkMode = config.ReadBool("dark_mode", darkMode);
        ApplyDarkMode(darkMode);
        UpdateDictionaryDefaults();
    }

    void SaveConfig() {
        wxFileConfig config("neolip_gui");
        settings_.saveWindowPlacement(*this);
        settings_.setDarkMode(darkMode_ != nullptr && darkMode_->GetValue());
        settings_.setFontScale(fontScale_);
        settings_.setRecentPaths("ModelRoots", ChoicePaths(recentModels_), 12);
        settings_.setRecentPaths("Folders", ChoicePaths(recentFolders_), 12);
        config.Write("models_root", modelsPicker_->GetPath());
        wxString savedDict = userDictPicker_->GetPath();
        if (savedDict.empty()) savedDict = batchUserDictPicker_->GetPath();
        if (savedDict.empty()) savedDict = dictOverrideDictPicker_->GetPath();
        config.Write("user_dict", savedDict);
        config.Write("batch_input_dir", batchInputDirPicker_->GetPath());
        config.Write("batch_output_dir", batchOutputDirPicker_->GetPath());
        config.Write("dict_input_dir", dictInputDirPicker_->GetPath());
        config.Write("dict_output_dir", dictOutputDirPicker_->GetPath());
        config.Write("align_output_dir", alignOutputDirPicker_->GetPath());
        config.Write("phn_preview", phnPreviewPicker_->GetPath());
        config.Write("lip_editor_open", lipEditorOpenPicker_->GetPath());
        config.Write("lip_editor_save", lipEditorSavePicker_->GetPath());
        config.Write("timeline_audio", timelineAudioPicker_->GetPath());
        config.Write("timeline_lip", timelineLipPicker_->GetPath());
        config.Write("timeline_phn", timelinePhnPicker_->GetPath());
        config.Write("timeline_save", timelineSavePicker_->GetPath());
        config.Write("use_ogi", useOgi_->GetValue());
        config.Write("mode", alignMode_->GetStringSelection());
        config.Write("align_output_format", alignOutputFormat_->GetStringSelection());
        config.Write("batch_output_kind", batchOutputKind_->GetStringSelection());
        config.Write("tail_rest_ms", alignTailRest_->GetValue());
        config.Write("close_on_pause_ms", alignClosePause_->GetValue());
        config.Write("lip_smoothing", alignSmoothing_->GetStringSelection());
        config.Write("preserve_closures", alignPreserveClosures_->GetValue());
        config.Write("anticipate_closures_ms", alignAnticipateClosures_->GetValue());
        config.Write("anticipate_fricatives_ms", alignAnticipateFricatives_->GetValue());
        config.Write("anticipate_stops_ms", alignAnticipateStops_->GetValue());
        config.Write("batch_tail_rest_ms", batchTailRest_->GetValue());
        config.Write("batch_close_on_pause_ms", batchClosePause_->GetValue());
        config.Write("batch_lip_smoothing", batchSmoothing_->GetStringSelection());
        config.Write("batch_preserve_closures", batchPreserveClosures_->GetValue());
        config.Write("batch_anticipate_closures_ms", batchAnticipateClosures_->GetValue());
        config.Write("batch_anticipate_fricatives_ms", batchAnticipateFricatives_->GetValue());
        config.Write("batch_anticipate_stops_ms", batchAnticipateStops_->GetValue());
        config.Write("batch_jobs", batchJobs_->GetValue());
        config.Write("batch_resume", batchResume_->GetValue());
        config.Write("batch_decode_cache", batchDecodeCachePicker_->GetPath());
        config.Write("dict_tail_rest_ms", dictTailRest_->GetValue());
        config.Write("dict_close_on_pause_ms", dictClosePause_->GetValue());
        config.Write("dict_lip_smoothing", dictSmoothing_->GetStringSelection());
        config.Write("dict_preserve_closures", dictPreserveClosures_->GetValue());
        config.Write("dict_anticipate_closures_ms", dictAnticipateClosures_->GetValue());
        config.Write("dict_anticipate_fricatives_ms", dictAnticipateFricatives_->GetValue());
        config.Write("dict_anticipate_stops_ms", dictAnticipateStops_->GetValue());
        config.Flush();
    }

    static std::vector<fs::path> ParseLegacyPathList(const wxString& value) {
        std::vector<fs::path> paths;
        const std::string delimited = wxui::toStd(value);
        std::size_t begin = 0;
        while (begin <= delimited.size()) {
            const std::size_t end = delimited.find('|', begin);
            const std::string item = delimited.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            if (!item.empty()) paths.push_back(fs::u8path(item));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        return paths;
    }

    static void LoadChoiceFromPaths(wxChoice* choice, const std::vector<fs::path>& paths) {
        choice->Clear();
        for (const auto& path : paths) choice->Append(wxui::toWx(path.u8string()));
        if (!paths.empty()) choice->SetSelection(0);
    }

    static std::vector<fs::path> ChoicePaths(const wxChoice* choice) {
        std::vector<fs::path> paths;
        if (choice == nullptr) return paths;
        paths.reserve(choice->GetCount());
        for (unsigned i = 0; i < choice->GetCount(); ++i) {
            paths.push_back(fs::u8path(wxui::toStd(choice->GetString(i))));
        }
        return paths;
    }

    void AddRecent(wxChoice* choice, const wxString& value) {
        if (value.empty()) return;
        int existing = choice->FindString(value);
        if (existing != wxNOT_FOUND) choice->Delete(static_cast<unsigned>(existing));
        choice->Insert(value, 0);
        while (choice->GetCount() > 12) choice->Delete(choice->GetCount() - 1);
        choice->SetSelection(0);
    }

    void RememberPaths() {
        AddRecent(recentModels_, modelsPicker_->GetPath());
        AddRecent(recentFolders_, batchInputDirPicker_->GetPath());
        AddRecent(recentFolders_, batchOutputDirPicker_->GetPath());
        AddRecent(recentFolders_, dictInputDirPicker_->GetPath());
        AddRecent(recentFolders_, dictOutputDirPicker_->GetPath());
        AddRecent(recentFolders_, alignOutputDirPicker_->GetPath());
        SaveConfig();
    }

    void UpdateAlignVisibility() {
        const bool textMode = inputKind_->GetSelection() == 0;
        textPicker_->Enable(textMode);
        literalText_->Enable(textMode);
        alignMode_->Enable(textMode);
        userDictPicker_->Enable(textMode);
        useOgi_->Enable(textMode);
        writeReports_->Enable(textMode);
        ntpPicker_->Enable(!textMode);
    }

    void MaybeDeriveAlignOutput() {
        if (phnOutputPicker_->GetPath().empty() && !wavPicker_->GetPath().empty() && !alignOutputDirPicker_->GetPath().empty()) {
            const bool lip = alignOutputFormat_ && alignOutputFormat_->GetStringSelection() == "LIP";
            phnOutputPicker_->SetPath(lip ? DeriveOutputLip(wavPicker_->GetPath(), alignOutputDirPicker_->GetPath())
                                         : DeriveOutputPhn(wavPicker_->GetPath(), alignOutputDirPicker_->GetPath()));
        }
        if (workDirPicker_->GetPath().empty() && !phnOutputPicker_->GetPath().empty()) {
            workDirPicker_->SetPath(DefaultWorkDirForOutput(phnOutputPicker_->GetPath()));
        }
    }

    wxString ToolPath() const { return DefaultToolPath(); }
    wxString ModelRoot() const { return modelsPicker_->GetPath(); }

    void ApplyDefaultKotorOverrideIfAvailable() {
        const wxString dict = DefaultKotorOverridePath(ToolPath(), ModelRoot());
        if (dict.empty()) return;
        if (userDictPicker_ && userDictPicker_->GetPath().empty()) userDictPicker_->SetPath(dict);
        if (batchUserDictPicker_ && batchUserDictPicker_->GetPath().empty()) batchUserDictPicker_->SetPath(dict);
        if (dictOverrideDictPicker_ && dictOverrideDictPicker_->GetPath().empty()) dictOverrideDictPicker_->SetPath(dict);
        if (ntpUserDictPicker_ && ntpUserDictPicker_->GetPath().empty()) ntpUserDictPicker_->SetPath(dict);
        if (lookupUserDictPicker_ && lookupUserDictPicker_->GetPath().empty()) lookupUserDictPicker_->SetPath(dict);
    }

    std::vector<wxString> RequiredModelFiles(const wxString& rootIn) const {
        wxString root = rootIn;
        wxFileName packageModels(root, "models");
        if (wxDirExists(packageModels.GetFullPath())) {
            wxFileName spec(packageModels.GetFullPath(), "phonemes_new_16k.train.spec");
            if (spec.FileExists()) root = packageModels.GetFullPath();
        }
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

    bool ValidateModelRootDetailed(bool showSuccess) {
        if (ModelRoot().empty()) {
            wxMessageBox("Select a standalone package root or models folder.", "Missing models", wxOK | wxICON_WARNING, this);
            return false;
        }
        wxString missing;
        for (const auto& path : RequiredModelFiles(ModelRoot())) {
            if (!wxFileExists(path)) missing += "  " + path + "\n";
        }
        if (!missing.empty()) {
            wxMessageBox("The selected package/models folder is missing required files:\n\n" + missing,
                         "Invalid model folder", wxOK | wxICON_ERROR, this);
            return false;
        }
        if (showSuccess) wxMessageBox("Model folder validation passed.", "Package/models OK", wxOK | wxICON_INFORMATION, this);
        return true;
    }

    bool ValidateDictionaryFile(const wxString& path, bool mustExist) {
        if (path.empty() || !mustExist) return true;
        if (!wxFileExists(path)) {
            wxMessageBox("Dictionary file does not exist:\n\n" + path,
                         "Missing dictionary", wxOK | wxICON_ERROR, this);
            return false;
        }
        return true;
    }

    bool ValidateCommon() {
        wxFileName toolName(ToolPath());
        if (!toolName.IsAbsolute() || !wxFileExists(ToolPath())) {
            wxMessageBox("NeoLIP's internal backend helper is missing:\n\n" + ToolPath() +
                             "\n\nKeep neolip_backend beside neolip_gui, or reinstall NeoLIP.",
                         "Incomplete NeoLIP installation", wxOK | wxICON_ERROR, this);
            return false;
        }
        return ValidateModelRootDetailed(false);
    }

    void AddModelsFlag(wxArrayString& args) const {
        args.Add("--models");
        args.Add(ModelRoot());
    }

    void AppendTextFlags(wxArrayString& args, const wxString& mode, const wxString& userDict, bool useOgi,
                         const wxString& sourceReport = wxEmptyString, const wxString& unknownReport = wxEmptyString) {
        args.Add("--mode");
        args.Add(mode.empty() ? wxString("simple") : mode);
        if (!userDict.empty()) {
            args.Add("--override-dict");
            args.Add(userDict);
        }
        args.Add(useOgi ? "--use-ogi" : "--no-ogi");
        if (!sourceReport.empty()) { args.Add("--source-report"); args.Add(sourceReport); }
        if (!unknownReport.empty()) { args.Add("--unknown-report"); args.Add(unknownReport); }
    }

    wxString TextSourceFromControls(wxFilePickerCtrl* filePicker, wxTextCtrl* literal) const {
        const wxString file = filePicker->GetPath();
        if (!file.empty()) return file;
        return literal->GetValue();
    }


    void OnRunAlign(wxCommandEvent&) {
        MaybeDeriveAlignOutput();
        if (!ValidateCommon()) return;
        if (!ValidateDictionaryFile(userDictPicker_->GetPath(), inputKind_->GetSelection() == 0 && !userDictPicker_->GetPath().empty())) return;
        if (wavPicker_->GetPath().empty()) { wxMessageBox("Select a WAV file.", "Missing WAV", wxOK | wxICON_WARNING, this); return; }
        if (phnOutputPicker_->GetPath().empty()) { wxMessageBox("Choose an output LIP/PHN file or output folder.", "Missing output", wxOK | wxICON_WARNING, this); return; }
        const bool outputLip = alignOutputFormat_->GetStringSelection() == "LIP";
        wxArrayString args;
        args.Add(ToolPath());
        const wxString work = workDirPicker_->GetPath();
        lastOutputPath_ = phnOutputPicker_->GetPath();
        lastWorkDir_ = work;
        if (inputKind_->GetSelection() == 0) {
            const wxString text = TextSourceFromControls(textPicker_, literalText_);
            if (text.empty()) { wxMessageBox("Select a text file or enter literal text.", "Missing text", wxOK | wxICON_WARNING, this); return; }
            args.Add(outputLip ? "align-wav-text-lip" : "align-wav-text");
            args.Add(wavPicker_->GetPath());
            args.Add(text);
            args.Add(phnOutputPicker_->GetPath());
            AddModelsFlag(args);
            if (outputLip) { AppendTailRestFlag(args, alignTailRest_); AppendLipQualityFlags(args, alignClosePause_, alignSmoothing_, alignPreserveClosures_, alignAnticipateClosures_, alignAnticipateFricatives_, alignAnticipateStops_); }
            if (!work.empty()) { args.Add("--work-dir"); args.Add(work); }
            wxString srcReport, unknownReport;
            if (writeReports_->GetValue() && !work.empty()) {
                srcReport = wxFileName(work, "pronunciations.tsv").GetFullPath();
                unknownReport = wxFileName(work, "unknown_words.tsv").GetFullPath();
            }
            AppendTextFlags(args, alignMode_->GetStringSelection(), userDictPicker_->GetPath(), useOgi_->GetValue(), srcReport, unknownReport);
        } else {
            if (ntpPicker_->GetPath().empty()) { wxMessageBox("Select an NTP file.", "Missing NTP", wxOK | wxICON_WARNING, this); return; }
            args.Add(outputLip ? "align-wav-ntp-lip" : "align-wav-ntp");
            args.Add(wavPicker_->GetPath());
            args.Add(ntpPicker_->GetPath());
            args.Add(phnOutputPicker_->GetPath());
            AddModelsFlag(args);
            if (outputLip) { AppendTailRestFlag(args, alignTailRest_); AppendLipQualityFlags(args, alignClosePause_, alignSmoothing_, alignPreserveClosures_, alignAnticipateClosures_, alignAnticipateFricatives_, alignAnticipateStops_); }
            if (!work.empty()) { args.Add("--work-dir"); args.Add(work); }
        }
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
        if (dictInputDirPicker_->GetPath().empty()) { wxMessageBox("Select a transcript/WAV folder first.", "Missing folder", wxOK | wxICON_WARNING, this); return false; }
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
        if (!fn.FileExists()) {
            wxFFile f(path, "ab");
            if (f.IsOpened()) f.Close();
        }
        wxLaunchDefaultApplication(path);
    }

    void OnOpenUnknownReport(wxCommandEvent&) { UpdateDictionaryDefaults(); OpenOrCreateTextFile(dictUnknownReportPicker_->GetPath()); }
    void OnOpenOverrideDict(wxCommandEvent&) { UpdateDictionaryDefaults(); OpenOrCreateTextFile(dictOverrideDictPicker_->GetPath()); }

    void OnRunDictionaryBatch(wxCommandEvent&) {
        if (!ValidateDictionaryWorkflow(true, true, true)) return;
        wxArrayString args;
        args.Add(ToolPath());
        args.Add("batch-align");
        args.Add(dictInputDirPicker_->GetPath());
        args.Add(dictOutputDirPicker_->GetPath());
        AddModelsFlag(args);
        args.Add("--input"); args.Add("text");
        args.Add("--output"); args.Add("lip");
        AppendTextFlags(args, dictMode_->GetStringSelection(), dictOverrideDictPicker_->GetPath(), dictUseOgi_->GetValue());
        AppendTailRestFlag(args, dictTailRest_);
        AppendLipQualityFlags(args, dictClosePause_, dictSmoothing_, dictPreserveClosures_, dictAnticipateClosures_, dictAnticipateFricatives_, dictAnticipateStops_);
        if (dictRecursive_->GetValue()) args.Add("--recursive");
        args.Add(dictWorkdirs_->GetValue() ? "--workdirs" : "--no-workdirs");
        args.Add("--batch-report"); args.Add(wxFileName(dictOutputDirPicker_->GetPath(), "batch_report.tsv").GetFullPath());
        args.Add("--unknown-report"); args.Add(dictUnknownReportPicker_->GetPath());
        StartCommand(args, true, CountWavs(dictInputDirPicker_->GetPath(), dictRecursive_->GetValue()));
    }

    void OnRunBatch(wxCommandEvent&) {
        if (!ValidateCommon()) return;
        if (!ValidateDictionaryFile(batchUserDictPicker_->GetPath(), !batchUserDictPicker_->GetPath().empty())) return;
        if (batchInputDirPicker_->GetPath().empty()) { wxMessageBox("Select an input folder.", "Missing input folder", wxOK | wxICON_WARNING, this); return; }
        if (batchOutputDirPicker_->GetPath().empty()) { wxMessageBox("Select an output folder.", "Missing output folder", wxOK | wxICON_WARNING, this); return; }
        wxArrayString args;
        args.Add(ToolPath());
        args.Add("batch-align");
        args.Add(batchInputDirPicker_->GetPath());
        args.Add(batchOutputDirPicker_->GetPath());
        AddModelsFlag(args);
        args.Add("--input"); args.Add(batchInputKind_->GetStringSelection().empty() ? wxString("text") : batchInputKind_->GetStringSelection());
        args.Add("--output"); args.Add(batchOutputKind_->GetStringSelection().empty() ? wxString("lip") : batchOutputKind_->GetStringSelection());
        AppendTextFlags(args, batchMode_->GetStringSelection(), batchUserDictPicker_->GetPath(), batchUseOgi_->GetValue());
        const wxString batchOut = batchOutputKind_->GetStringSelection().empty() ? wxString("lip") : batchOutputKind_->GetStringSelection();
        if (batchOut == "lip" || batchOut == "both") { AppendTailRestFlag(args, batchTailRest_); AppendLipQualityFlags(args, batchClosePause_, batchSmoothing_, batchPreserveClosures_, batchAnticipateClosures_, batchAnticipateFricatives_, batchAnticipateStops_); }
        if (batchRecursive_->GetValue()) args.Add("--recursive");
        args.Add(batchWorkdirs_->GetValue() ? "--workdirs" : "--no-workdirs");
        if (batchFailFast_->GetValue()) args.Add("--fail-fast");
        if (batchResume_->GetValue()) args.Add("--resume");
        wxString jobs = batchJobs_->GetValue(); jobs.Trim(true).Trim(false);
        if (!jobs.empty()) { args.Add("--jobs"); args.Add(jobs); }
        if (!batchDecodeCachePicker_->GetPath().empty()) { args.Add("--decode-cache-dir"); args.Add(batchDecodeCachePicker_->GetPath()); }
        args.Add("--batch-report"); args.Add(wxFileName(batchOutputDirPicker_->GetPath(), "batch_report.tsv").GetFullPath());
        args.Add("--unknown-report"); args.Add(wxFileName(batchOutputDirPicker_->GetPath(), "unknown_words.tsv").GetFullPath());
        lastOutputPath_ = batchOutputDirPicker_->GetPath();
        lastWorkDir_ = batchOutputDirPicker_->GetPath();
        StartCommand(args, true, CountWavs(batchInputDirPicker_->GetPath(), batchRecursive_->GetValue()));
    }

    void OnRunTextToNtp(wxCommandEvent&) {
        if (!ValidateCommon()) return;
        if (!ValidateDictionaryFile(ntpUserDictPicker_->GetPath(), !ntpUserDictPicker_->GetPath().empty())) return;
        const wxString text = TextSourceFromControls(ntpTextFilePicker_, ntpLiteralText_);
        if (text.empty()) { wxMessageBox("Select a text file or enter literal text.", "Missing text", wxOK | wxICON_WARNING, this); return; }
        if (ntpOutputPicker_->GetPath().empty()) { wxMessageBox("Choose an output NTP file.", "Missing output", wxOK | wxICON_WARNING, this); return; }
        wxArrayString args;
        args.Add(ToolPath());
        args.Add("text-to-ntp");
        args.Add(text);
        args.Add(ntpOutputPicker_->GetPath());
        AddModelsFlag(args);
        wxString srcReport, unknownReport;
        if (ntpWriteReports_->GetValue()) {
            wxFileName out(ntpOutputPicker_->GetPath());
            srcReport = wxFileName(out.GetPath(), out.GetName() + "_pronunciations.tsv").GetFullPath();
            unknownReport = wxFileName(out.GetPath(), out.GetName() + "_unknown_words.tsv").GetFullPath();
        }
        AppendTextFlags(args, ntpMode_->GetStringSelection(), ntpUserDictPicker_->GetPath(), ntpUseOgi_->GetValue(), srcReport, unknownReport);
        lastOutputPath_ = ntpOutputPicker_->GetPath();
        StartCommand(args, false);
    }

    void OnRunLookup(wxCommandEvent&) {
        if (!ValidateCommon()) return;
        if (!ValidateDictionaryFile(lookupUserDictPicker_->GetPath(), !lookupUserDictPicker_->GetPath().empty())) return;
        if (lookupWord_->GetValue().empty()) { wxMessageBox("Enter a word to look up.", "Missing word", wxOK | wxICON_WARNING, this); return; }
        wxArrayString args;
        args.Add(ToolPath());
        args.Add("lookup-word");
        args.Add(lookupWord_->GetValue());
        AddModelsFlag(args);
        if (!lookupUserDictPicker_->GetPath().empty()) { args.Add("--override-dict"); args.Add(lookupUserDictPicker_->GetPath()); }
        args.Add(lookupUseOgi_->GetValue() ? "--use-ogi" : "--no-ogi");
        StartCommand(args, false);
    }

    void OnRunCompareTrace(wxCommandEvent&) {
        if (!ValidateCommon()) return;
        if (traceDumpPicker_->GetPath().empty() || traceOutputPhnPicker_->GetPath().empty()) {
            wxMessageBox("Select a trace dump folder and an output PHN file.", "Missing trace inputs", wxOK | wxICON_WARNING, this);
            return;
        }
        wxArrayString args;
        args.Add(ToolPath());
        args.Add("compare-end-to-end-trace");
        args.Add(traceDumpPicker_->GetPath());
        args.Add(ModelRoot());
        args.Add(traceOutputPhnPicker_->GetPath());
        if (!traceWorkDirPicker_->GetPath().empty()) args.Add(traceWorkDirPicker_->GetPath());
        lastOutputPath_ = traceOutputPhnPicker_->GetPath();
        lastWorkDir_ = traceWorkDirPicker_->GetPath();
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
        if (process_) { wxMessageBox("A job is already running.", "Busy", wxOK | wxICON_INFORMATION, this); return; }
        RememberPaths();
        processCommand_ = JoinCommand(args);
        log_->AppendText("\n[starting job]\n");
        process_ = std::make_unique<CsluProcess>(this);
        processPid_ = wxExecute(args, wxEXEC_ASYNC, process_.get());
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
        stopButton_->Enable(true);
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
        if (code == 0 && !lastOutputPath_.empty() && lastOutputPath_.Lower().EndsWith(".phn")) {
            phnPreviewPicker_->SetPath(lastOutputPath_);
        }
    }

    void OnStop(wxCommandEvent&) { StopRunningProcess(true); }

    void StopRunningProcess(bool notify) {
        if (!process_ || processPid_ == 0) return;
        const wxKillError rc = wxProcess::Kill(processPid_, wxSIGTERM, wxKILL_CHILDREN);
        if (notify) log_->AppendText(wxString::Format("\n[cancel requested, wxKill result %d]\n", static_cast<int>(rc)));
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
        wxFileDialog dlg(this, "Save activity log", wxEmptyString, "neolip_gui_log.txt", "Text files (*.txt)|*.txt|All files (*.*)|*.*", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
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
            if (notebook_->GetSelection() == 0) path = workDirPicker_->GetPath();
            else if (notebook_->GetSelection() == 1) path = batchOutputDirPicker_->GetPath();
            else if (notebook_->GetSelection() == 2) path = dictOutputDirPicker_->GetPath();
            else if (notebook_->GetSelection() == 8) path = traceWorkDirPicker_->GetPath();
        }
        if (!path.empty()) wxLaunchDefaultApplication(path);
    }

    void OnOpenOutputFolder(wxCommandEvent&) {
        wxString path = alignOutputDirPicker_->GetPath();
        if (path.empty() && !phnOutputPicker_->GetPath().empty()) path = wxFileName(phnOutputPicker_->GetPath()).GetPath();
        if (!path.empty()) wxLaunchDefaultApplication(path);
    }

    void OnOpenModelFolder(wxCommandEvent&) {
        if (!ModelRoot().empty()) wxLaunchDefaultApplication(ModelRoot());
    }

    void OnValidateModels(wxCommandEvent&) { ValidateModelRootDetailed(true); }

    void OnLoadPhnPreview(wxCommandEvent&) { LoadPhnPreview(phnPreviewPicker_->GetPath()); }

    void OnLoadLastPhnPreview(wxCommandEvent&) {
        if (lastOutputPath_.empty()) { wxMessageBox("No PHN output has been generated in this GUI session.", "No last PHN", wxOK | wxICON_INFORMATION, this); return; }
        phnPreviewPicker_->SetPath(lastOutputPath_);
        LoadPhnPreview(lastOutputPath_);
    }

    void LoadPhnPreview(const wxString& path) {
        if (path.empty() || !wxFileExists(path)) {
            wxMessageBox("Select an existing PHN file.", "Missing PHN",
                         wxOK | wxICON_WARNING, this);
            return;
        }
        try {
            const cslu::PhnFile phn = cslu::readPhn(ToPath(path));
            wxFFile file(path, "rb");
            if (!file.IsOpened()) throw std::runtime_error("could not open PHN file");
            wxString content;
            if (!file.ReadAll(&content, wxConvUTF8)) {
                throw std::runtime_error("could not decode PHN file as UTF-8");
            }
            phnPreviewText_->SetValue(content);
            phnSummary_->SetValue(SummarizePhn(path, phn));
            notebook_->SetSelection(3);
        } catch (const std::exception& error) {
            wxMessageBox(wxString::Format("Could not load PHN: %s", error.what()),
                         "Open failed", wxOK | wxICON_ERROR, this);
        }
    }

    wxString SummarizePhn(const wxString& path, const cslu::PhnFile& phn) const {
        int firstStart = -1;
        int lastEnd = -1;
        std::string firstPhone;
        std::string lastPhone;
        bool monotonic = true;
        int previousEnd = -1;
        for (const auto& interval : phn.intervals) {
            if (firstStart < 0) {
                firstStart = interval.startMs;
                firstPhone = interval.label;
            }
            if (previousEnd >= 0 && interval.startMs != previousEnd) monotonic = false;
            if (interval.endMs < interval.startMs) monotonic = false;
            previousEnd = interval.endMs;
            lastEnd = interval.endMs;
            lastPhone = interval.label;
        }

        wxString summary;
        summary << "file\t" << path << "\n";
        summary << "intervals\t" << phn.intervals.size() << "\n";
        summary << "starts_at\t" << firstStart << "\n";
        summary << "duration_ms\t" << lastEnd << "\n";
        summary << "first_phone\t" << wxui::toWx(firstPhone) << "\n";
        summary << "last_phone\t" << wxui::toWx(lastPhone) << "\n";
        summary << "monotonic\t" << (monotonic ? "yes" : "no") << "\n";
        return summary;
    }

    void RefreshLipGrid() {
        if (!lipGrid_) return;
        const int existingRows = lipGrid_->GetNumberRows();
        if (existingRows > 0) lipGrid_->DeleteRows(0, existingRows);
        if (!lipEditorEntries_.empty()) lipGrid_->AppendRows(static_cast<int>(lipEditorEntries_.size()));
        for (std::size_t i = 0; i < lipEditorEntries_.size(); ++i) {
            const auto& e = lipEditorEntries_[i];
            lipGrid_->SetCellValue(static_cast<int>(i), 0, wxString::Format("%zu", i));
            lipGrid_->SetCellValue(static_cast<int>(i), 1, wxString::Format("%.6f", e.timeSeconds));
            lipGrid_->SetCellValue(static_cast<int>(i), 2, wxString::Format("%d", e.shape));
            lipGrid_->SetCellValue(static_cast<int>(i), 3, KotorShapeName(e.shape));
            lipGrid_->SetReadOnly(static_cast<int>(i), 0, true);
            lipGrid_->SetReadOnly(static_cast<int>(i), 3, true);
        }
        lipGrid_->AutoSizeColumn(3, false);
        UpdateLipEditorStatus();
    }

    bool SyncLipGridToEntries(bool showErrors = true) {
        if (!lipGrid_) return false;
        std::vector<GuiLipEntry> next;
        const int rows = lipGrid_->GetNumberRows();
        next.reserve(static_cast<std::size_t>(std::max(rows, 0)));
        for (int r = 0; r < rows; ++r) {
            double time = 0.0;
            long shape = 0;
            if (!lipGrid_->GetCellValue(r, 1).ToDouble(&time)) {
                if (showErrors) wxMessageBox(wxString::Format("Invalid time at row %d.", r), "Invalid LIP table", wxOK | wxICON_WARNING, this);
                return false;
            }
            if (!lipGrid_->GetCellValue(r, 2).ToLong(&shape) || shape < 0 || shape > 15) {
                if (showErrors) wxMessageBox(wxString::Format("Shape must be 0..15 at row %d.", r), "Invalid LIP table", wxOK | wxICON_WARNING, this);
                return false;
            }
            next.push_back({time, static_cast<std::uint8_t>(shape)});
        }
        lipEditorEntries_ = std::move(next);
        return true;
    }

    void UpdateLipEditorStatus() {
        if (!lipEditorStatus_) return;
        if (lipEditorEntries_.empty()) {
            lipEditorStatus_->SetLabel("No visible mouth keys loaded.");
            return;
        }
        const auto maxIt = std::max_element(lipEditorEntries_.begin(), lipEditorEntries_.end(), [](const GuiLipEntry& a, const GuiLipEntry& b){ return a.timeSeconds < b.timeSeconds; });
        const bool finalRest = !lipEditorEntries_.empty() && lipEditorEntries_.back().shape == 0;
        lipEditorStatus_->SetLabel(wxString::Format("visible_keys=%zu  duration≈%.3fs  final_rest=%s",
            lipEditorEntries_.size(), maxIt->timeSeconds, finalRest ? "yes" : "will be normalized on save"));
    }

    std::vector<int> SelectedLipRows() const {
        std::vector<int> rows;
        if (!lipGrid_) return rows;
        wxArrayInt selected = lipGrid_->GetSelectedRows();
        for (unsigned i = 0; i < selected.GetCount(); ++i) rows.push_back(selected[i]);
        if (rows.empty()) {
            const int cursor = lipGrid_->GetGridCursorRow();
            if (cursor >= 0 && cursor < lipGrid_->GetNumberRows()) rows.push_back(cursor);
        }
        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        return rows;
    }

    void LoadLipEditorPath(const wxString& path) {
        if (path.empty() || !wxFileExists(path)) {
            wxMessageBox("Select an existing .lip file.", "Missing LIP", wxOK | wxICON_WARNING, this);
            return;
        }
        try {
            double duration = 0.0;
            lipEditorEntries_ = ReadStockLipVisibleEntries(path, &duration);
            lipEditorOpenPicker_->SetPath(path);
            if (lipEditorSavePicker_->GetPath().empty()) lipEditorSavePicker_->SetPath(path);
            RefreshLipGrid();
            notebook_->SetSelection(4);
            log_->AppendText(wxString::Format("Loaded LIP: %s\nvisible_keys\t%zu\nduration_seconds\t%.6f\n", path, lipEditorEntries_.size(), duration));
        } catch (const std::exception& e) {
            wxMessageBox(wxString::Format("Could not load LIP: %s", e.what()), "Load failed", wxOK | wxICON_ERROR, this);
        }
    }

    void OpenLipFromGameDirectory(const std::filesystem::path& directory) {
        wxFileDialog dialog(this,
                            "Open KOTOR LIP",
                            wxui::toWx(directory.u8string()),
                            wxEmptyString,
                            "KOTOR LIP files (*.lip)|*.lip|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) {
            LoadLipEditorPath(dialog.GetPath());
        }
    }

    void OnLoadLipEditor(wxCommandEvent&) {
        LoadLipEditorPath(lipEditorOpenPicker_->GetPath());
    }

    void OnSaveLipEditor(wxCommandEvent&) {
        if (!SyncLipGridToEntries(true)) return;
        wxString path = lipEditorSavePicker_->GetPath();
        if (path.empty()) path = lipEditorOpenPicker_->GetPath();
        if (path.empty()) { wxMessageBox("Choose a save path.", "Missing output", wxOK | wxICON_WARNING, this); return; }
        try {
            NormalizeVisibleLipEntries(lipEditorEntries_);
            WriteStockLipVisibleEntries(path, lipEditorEntries_);
            RefreshLipGrid();
            lastOutputPath_ = path;
            log_->AppendText(wxString::Format("Saved KOTOR LIP: %s\nvisible_keys\t%zu\n", path, lipEditorEntries_.size()));
            wxui::setStatusText(*this, "Saved LIP");
        } catch (const std::exception& e) {
            wxMessageBox(wxString::Format("Could not save LIP: %s", e.what()), "Save failed", wxOK | wxICON_ERROR, this);
        }
    }

    void OnInsertLipKey(wxCommandEvent&) {
        if (!SyncLipGridToEntries(false)) return;
        const std::vector<int> rows = SelectedLipRows();
        int insertAfter = lipEditorEntries_.empty() ? -1 : static_cast<int>(lipEditorEntries_.size() - 1);
        if (!rows.empty()) insertAfter = rows.back();
        GuiLipEntry e;
        if (insertAfter >= 0 && insertAfter < static_cast<int>(lipEditorEntries_.size())) {
            e = lipEditorEntries_[static_cast<std::size_t>(insertAfter)];
            e.timeSeconds += 0.050;
        } else {
            e = {0.125, 0};
        }
        const auto pos = lipEditorEntries_.begin() + std::min<std::size_t>(static_cast<std::size_t>(insertAfter + 1), lipEditorEntries_.size());
        lipEditorEntries_.insert(pos, e);
        RefreshLipGrid();
    }

    void OnDeleteLipKeys(wxCommandEvent&) {
        if (!SyncLipGridToEntries(false)) return;
        std::vector<int> rows = SelectedLipRows();
        if (rows.empty()) return;
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            if (*it >= 0 && *it < static_cast<int>(lipEditorEntries_.size())) lipEditorEntries_.erase(lipEditorEntries_.begin() + *it);
        }
        if (lipEditorEntries_.empty()) lipEditorEntries_.push_back({0.125, 0});
        RefreshLipGrid();
    }

    void OnShiftLipKeys(wxCommandEvent&) {
        if (!SyncLipGridToEntries(true)) return;
        double delta = 0.0;
        if (!lipShiftAmount_->GetValue().ToDouble(&delta)) { wxMessageBox("Enter a numeric shift amount in seconds.", "Invalid shift", wxOK | wxICON_WARNING, this); return; }
        const std::vector<int> rows = SelectedLipRows();
        if (rows.empty()) return;
        for (int r : rows) {
            if (r >= 0 && r < static_cast<int>(lipEditorEntries_.size())) lipEditorEntries_[static_cast<std::size_t>(r)].timeSeconds = std::max(0.0, lipEditorEntries_[static_cast<std::size_t>(r)].timeSeconds + delta);
        }
        RefreshLipGrid();
    }

    void OnNormalizeLipKeys(wxCommandEvent&) {
        if (!SyncLipGridToEntries(true)) return;
        NormalizeVisibleLipEntries(lipEditorEntries_);
        RefreshLipGrid();
    }

    GuiWaveform LoadTimelineWaveformViaBackend(const wxString& audioPath) {
        try {
            return ReadSimpleWaveform(audioPath);
        } catch (...) {
            const wxString tmp = CreateTimelineTempFile("neolip_timeline_decoded_");
            wxArrayString args;
            args.Add(ToolPath());
            args.Add("decode-audio-wav");
            args.Add(audioPath);
            args.Add(tmp);
            try {
                const long rc = wxExecute(args, wxEXEC_SYNC);
                if (rc != 0 || !wxFileExists(tmp)) {
                    throw std::runtime_error("backend decode-audio-wav failed; check the NeoLIP installation and audio decoder setup");
                }
                GuiWaveform waveform = ReadSimpleWaveform(tmp);
                RemoveFileBestEffort(tmp);
                return waveform;
            } catch (...) {
                RemoveFileBestEffort(tmp);
                throw;
            }
        }
    }

    void UpdateTimelineStatus() {
        if (!timelineStatus_) return;
        const double audioDur = timelineWaveform_.durationSeconds();
        const double lastKey = timelineEntries_.empty() ? 0.0 : std::max_element(timelineEntries_.begin(), timelineEntries_.end(), [](const GuiLipEntry& a, const GuiLipEntry& b){ return a.timeSeconds < b.timeSeconds; })->timeSeconds;
        const bool finalRest = !timelineEntries_.empty() && timelineEntries_.back().shape == 0;
        timelineStatus_->SetLabel(wxString::Format("audio %.3fs   lip %.3fs   keys %zu   last key %.3fs   final rest %s",
            audioDur, timelineLipDuration_, timelineEntries_.size(), lastKey, finalRest ? "yes" : "will be normalized on export"));
    }

    void RefreshTimelinePanel() {
        if (!timelinePanel_) return;
        const auto intervals = ReadGuiPhnIntervals(timelinePhnPicker_ ? timelinePhnPicker_->GetPath() : wxString());
        timelinePanel_->SetData(timelineWaveform_, timelineEntries_, intervals, timelineLipDuration_);
        if (timelineZoom_) timelinePanel_->SetZoom(timelineZoom_->GetValue());
        UpdateTimelineStatus();
    }

    void OnLoadLipTimeline(wxCommandEvent&) {
        const wxString lip = timelineLipPicker_->GetPath();
        if (lip.empty() || !wxFileExists(lip)) { wxMessageBox("Select an existing .lip file.", "Missing LIP", wxOK | wxICON_WARNING, this); return; }
        try {
            double duration = 0.0;
            timelineEntries_ = ReadStockLipVisibleEntries(lip, &duration);
            timelineLipDuration_ = duration;
            if (timelineSavePicker_->GetPath().empty()) timelineSavePicker_->SetPath(lip);
            if (!timelineAudioPicker_->GetPath().empty() && wxFileExists(timelineAudioPicker_->GetPath())) timelineWaveform_ = LoadTimelineWaveformViaBackend(timelineAudioPicker_->GetPath());
            else timelineWaveform_ = GuiWaveform{};
            RefreshTimelinePanel();
            log_->AppendText(wxString::Format("Loaded LIP timeline: %s\nvisible_keys\t%zu\nlip_duration_seconds\t%.6f\naudio_duration_seconds\t%.6f\n", lip, timelineEntries_.size(), timelineLipDuration_, timelineWaveform_.durationSeconds()));
        } catch (const std::exception& e) {
            wxMessageBox(wxString::Format("Could not load timeline: %s", e.what()), "Load failed", wxOK | wxICON_ERROR, this);
        }
    }

    void OnTimelineLoadFromEditor(wxCommandEvent&) {
        if (!SyncLipGridToEntries(false)) return;
        timelineEntries_ = lipEditorEntries_;
        timelineLipDuration_ = timelineEntries_.empty() ? 0.0 : timelineEntries_.back().timeSeconds;
        if (!lipEditorOpenPicker_->GetPath().empty()) timelineLipPicker_->SetPath(lipEditorOpenPicker_->GetPath());
        if (!lipEditorSavePicker_->GetPath().empty()) timelineSavePicker_->SetPath(lipEditorSavePicker_->GetPath());
        try {
            if (!timelineAudioPicker_->GetPath().empty() && wxFileExists(timelineAudioPicker_->GetPath())) timelineWaveform_ = LoadTimelineWaveformViaBackend(timelineAudioPicker_->GetPath());
        } catch (const std::exception& e) {
            log_->AppendText(wxString::Format("Timeline audio load warning: %s\n", e.what()));
        }
        RefreshTimelinePanel();
        notebook_->SetSelection(5);
    }

    void OnSaveLipTimeline(wxCommandEvent&) {
        if (timelinePanel_) timelineEntries_ = timelinePanel_->Entries();
        wxString path = timelineSavePicker_->GetPath();
        if (path.empty()) path = timelineLipPicker_->GetPath();
        if (path.empty()) { wxMessageBox("Choose a save path.", "Missing output", wxOK | wxICON_WARNING, this); return; }
        try {
            NormalizeVisibleLipEntries(timelineEntries_);
            WriteStockLipVisibleEntries(path, timelineEntries_);
            timelineLipDuration_ = timelineEntries_.empty() ? 0.0 : timelineEntries_.back().timeSeconds;
            RefreshTimelinePanel();
            lastOutputPath_ = path;
            log_->AppendText(wxString::Format("Exported edited KOTOR LIP: %s\nvisible_keys\t%zu\nduration_seconds\t%.6f\n", path, timelineEntries_.size(), timelineLipDuration_));
            wxui::setStatusText(*this, "Exported LIP");
        } catch (const std::exception& e) {
            wxMessageBox(wxString::Format("Could not save LIP: %s", e.what()), "Save failed", wxOK | wxICON_ERROR, this);
        }
    }

    void OnTimelineNormalize(wxCommandEvent&) {
        if (timelinePanel_) timelineEntries_ = timelinePanel_->Entries();
        NormalizeVisibleLipEntries(timelineEntries_);
        timelineLipDuration_ = timelineEntries_.empty() ? 0.0 : timelineEntries_.back().timeSeconds;
        RefreshTimelinePanel();
    }

    void OnTimelineShapeChanged(wxCommandEvent&) {
        if (!timelinePanel_ || !timelineShapeChoice_) return;
        timelinePanel_->SetSelectedShape(timelineShapeChoice_->GetSelection());
        timelineEntries_ = timelinePanel_->Entries();
        UpdateTimelineStatus();
    }

    void OnTimelineZoomChanged(wxCommandEvent&) {
        if (timelinePanel_ && timelineZoom_) timelinePanel_->SetZoom(timelineZoom_->GetValue());
    }

    void OnTimelinePlayCursor(wxCommandEvent&) {
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
            if (!timelineSound_->IsOk()) { wxLaunchDefaultApplication(tmp); return; }
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
        SetAppName("neolip_gui");
        auto* frame = new MainFrame();
        frame->Show(true);
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(NeoLipGuiApp);
