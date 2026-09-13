#include "engine.hpp"
#include "dialogue_batch.hpp"
#include "audio.hpp"
#include "playback.hpp"
#include "wav.hpp"
#include "NeoSettings.hpp"
#include "NeoViewState.hpp"
#include "NeoWxUi.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include <wx/wx.h>
#include <wx/settings.h>
#include <wx/notebook.h>
#include <wx/thread.h>
#include <wx/collpane.h>
#include <wx/splitter.h>
#include <wx/filepicker.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/fileconf.h>
#include <wx/sizer.h>
#include <wx/wrapsizer.h>
#include <wx/wupdlock.h>
#include <wx/scrolwin.h>
#include <wx/filedlg.h>
#include <wx/dirdlg.h>
#include <wx/gauge.h>
#include <wx/grid.h>
#include <wx/textdlg.h>
#include <wx/choicdlg.h>
#include <wx/timer.h>
#include <wx/menu.h>
#include <wx/listctrl.h>
#include <wx/dcbuffer.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#ifndef __EMSCRIPTEN__
#include <wx/dnd.h>
#include <thread>
#endif
#include "neolip_icon.xpm"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstddef>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>
#include <neoshared/wasm_dialog_compat.h>
#include <neolip_wasm_runtime.h>
#endif
namespace fs = std::filesystem;
namespace {
constexpr const char* kConfigName = "neolip_gui";
constexpr int ID_SaveLipEditor = wxID_HIGHEST + 124;
constexpr int ID_SaveLipEditorAs = wxID_HIGHEST + 125;
constexpr int ID_InsertLipKey = wxID_HIGHEST + 126;
constexpr int ID_DeleteLipKeys = wxID_HIGHEST + 127;
constexpr int ID_ShiftLipKeys = wxID_HIGHEST + 128;
constexpr int ID_RepairLipKeys = wxID_HIGHEST + 129;
constexpr int ID_TimelinePlayCursor = wxID_HIGHEST + 134;
constexpr int ID_TimelineShapeChanged = wxID_HIGHEST + 135;
constexpr int ID_TimelineZoomChanged = wxID_HIGHEST + 136;
constexpr int ID_ToggleDarkMode = wxID_HIGHEST + 137;
constexpr int ID_FontIncrease = wxID_HIGHEST + 138;
constexpr int ID_FontDecrease = wxID_HIGHEST + 139;
constexpr int ID_FontReset = wxID_HIGHEST + 140;
constexpr int ID_UndoLip = wxID_HIGHEST + 141;
constexpr int ID_RedoLip = wxID_HIGHEST + 142;
constexpr int ID_SetLipDuration = wxID_HIGHEST + 143;

fs::path ToPath(const wxString& value) { return fs::u8path(wxui::toStd(value)); }
wxString ToWx(const fs::path& value) { return wxui::toWx(value.u8string()); }
bool IsRegularFile(const wxString& path) { std::error_code ec; return !path.empty() && fs::is_regular_file(ToPath(path), ec); }
void AddPickerRow(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, wxWindow* control) {
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    grid->Add(control, 1, wxEXPAND);
}
using GuiLipEntry = cslu::KotorKeyframe;
using GuiWaveform = cslu::DecodedWaveform;
wxString KotorShapeName(int shape) {
    if (shape < 0 || shape > 15) return "Out of range";
    return wxString::Format("%d: ", shape) + wxui::toWx(cslu::kotorLipShapeName(static_cast<std::uint8_t>(shape)));
}
class LipTimelinePanel final : public wxScrolledWindow {
public:
    explicit LipTimelinePanel(wxWindow* parent)
        :wxScrolledWindow(parent,wxID_ANY,wxDefaultPosition,parent->FromDIP(wxSize(-1,270)),wxBORDER_SIMPLE|wxWANTS_CHARS) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        // This canvas owns its scrolling geometry; there is no child sizer.
        SetAutoLayout(false);
        SetScrollRate(FromDIP(20), 0);
        SetName("Mouth animation timeline");
        SetToolTip("Space: play/pause. Arrows: seek 10 ms; Ctrl+arrows: 100 ms. Shift+drag: select a loop. Home/End: clip limits. F: fit clip. Z: zoom to selection.");
        Bind(wxEVT_PAINT,&LipTimelinePanel::OnPaint,this);
        Bind(wxEVT_LEFT_DOWN,&LipTimelinePanel::OnLeftDown,this);
        Bind(wxEVT_LEFT_UP,&LipTimelinePanel::OnLeftUp,this);
        Bind(wxEVT_MOTION,&LipTimelinePanel::OnMotion,this);
        Bind(wxEVT_LEFT_DCLICK,[this](wxMouseEvent& e){
            int x=0,y=0;CalcUnscrolledPosition(e.GetX(),e.GetY(),&x,&y);
            if(y>=FromDIP(138)&&y<=FromDIP(174))return;
            if(selected_<0)return;
            if(HasCapture())ReleaseMouse();
            dragging_=selecting_=false;
            wxArrayString choices;for(int i=0;i<16;++i)choices.Add(KotorShapeName(i));
            wxSingleChoiceDialog dialog(this,"Choose the pose for this key.","Change mouth pose",choices);
            dialog.SetSelection(entries_[static_cast<std::size_t>(selected_)].shape);
            if(dialog.ShowModal()==wxID_OK)SetSelectedShape(dialog.GetSelection());
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST,[this](wxMouseCaptureLostEvent&){dragging_=selecting_=false;});
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            event.Skip();
            QueueViewportUpdate();
        });
        Bind(wxEVT_CHAR,&LipTimelinePanel::OnKey,this);
        QueueViewportUpdate();
    }
    bool AcceptsFocus() const override {return true;}
    void SetData(std::shared_ptr<const neolip::AudioBuffer> audio,std::vector<GuiLipEntry> entries,double lipDuration) {
        if(audio!=audio_){audio_=std::move(audio);peaks_=neolip::WaveformPeaks(audio_);}
        entries_=std::move(entries);lipDuration_=lipDuration;
        sorted_=std::is_sorted(entries_.begin(),entries_.end(),[](const auto& a,const auto& b){return a.timeSeconds<b.timeSeconds;});
        duration_=std::max({.001,std::isfinite(lipDuration_)?lipDuration_:0.0,Waveform().durationSeconds()});
        cursorSeconds_=std::clamp(cursorSeconds_,0.0,duration_);
        if(selected_>=static_cast<int>(entries_.size()))selected_=static_cast<int>(entries_.size())-1;
        if(selected_<0&&!entries_.empty())selected_=0;
        SetRange(rangeA_,rangeB_);if(fit_)FitClip();else UpdateVirtualSize();Refresh();NotifySelection();
    }
    const std::vector<GuiLipEntry>& Entries()const{return entries_;}
    const GuiWaveform& Waveform()const{static const GuiWaveform empty;return audio_?audio_->waveform:empty;}
    double CursorSeconds()const{return cursorSeconds_;}
    std::pair<double,double> Selection()const{return {std::min(rangeA_,rangeB_),std::max(rangeA_,rangeB_)};}
    void SetRange(double a,double b){rangeA_=std::clamp(a,0.0,duration_);rangeB_=std::clamp(b,0.0,duration_);Refresh();}
    void SetCursor(double seconds,bool follow=false){
        cursorSeconds_=std::clamp(seconds,0.0,duration_);
        if(follow){int x=0,y=0;CalcUnscrolledPosition(0,0,&x,&y);const auto cx=XFromTime(cursorSeconds_);if(cx<x+Margin()||cx>x+GetClientSize().x-FromDIP(20)){int unitX=0,unitY=0;GetScrollPixelsPerUnit(&unitX,&unitY);if(unitX)Scroll(std::max(0,cx-GetClientSize().x/3)/unitX,0);}}
        if (!follow) NotifySelection();
        Refresh();
    }
    void SetZoom(int z){fit_=false;pixelsPerSecond_=FromDIP(160)*std::clamp(z,1,20);UpdateVirtualSize();Refresh();}
    void FitClip() {
        fit_ = true;
        QueueViewportUpdate();
    }
    void ZoomSelection(){const auto range=Selection();if(range.second<=range.first)return;fit_=false;pixelsPerSecond_=std::clamp(double(std::max(50,GetClientSize().x-2*Margin()))/(range.second-range.first),.001,double(FromDIP(3200)));UpdateVirtualSize();int x=0,y=0;GetScrollPixelsPerUnit(&x,&y);if(x)Scroll(std::max(0,XFromTime(range.first)-Margin())/x,0);Refresh();}
    void SetSelectedShape(int shape){
        if(selected_<0||selected_>=static_cast<int>(entries_.size()))return;
        const auto value=static_cast<std::uint8_t>(std::clamp(shape,0,15));auto& key=entries_[static_cast<std::size_t>(selected_)];
        if(key.shape==value)return;if(beforeEdit_)beforeEdit_();key.shape=value;Refresh();NotifySelection();
    }
    void SetWords(std::vector<neolip::AlignedWord> words){words_=std::move(words);Refresh();}
    void SetWordCallback(std::function<void(std::size_t)> f){wordCallback_=std::move(f);}
    void SetBeforeEditCallback(std::function<void()> f){beforeEdit_=std::move(f);}
    void SetSelectionCallback(std::function<void(int,const GuiLipEntry*)> f){selectionCallback_=std::move(f);}
    void SetSeekCallback(std::function<void(double)> f){seekCallback_=std::move(f);}
    void SetPlayCallback(std::function<void()> f){playCallback_=std::move(f);}
private:
    std::shared_ptr<const neolip::AudioBuffer> audio_;
    neolip::WaveformPeaks peaks_;
    std::vector<GuiLipEntry> entries_;
    std::vector<neolip::AlignedWord> words_;
    std::function<void(std::size_t)> wordCallback_;
    double lipDuration_=0,duration_=1,cursorSeconds_=0,pixelsPerSecond_=160,rangeA_=0,rangeB_=0;
    int selected_=-1;bool dragging_=false,dragEdited_=false,selecting_=false,fit_=true,sorted_=true;
    bool viewportUpdateQueued_ = false;
    bool updatingVirtualSize_ = false;
    std::function<void()> beforeEdit_,playCallback_;
    std::function<void(double)> seekCallback_;
    std::function<void(int,const GuiLipEntry*)> selectionCallback_;
    int Margin()const{return FromDIP(62);}
    int XFromTime(double t)const{return Margin()+static_cast<int>(std::lround(std::clamp(std::isfinite(t)?t*pixelsPerSecond_:0.0,0.0,100000000.0)));}
    double TimeFromX(int x)const{return std::max(0.0,double(x-Margin())/pixelsPerSecond_);}
    void QueueViewportUpdate() {
        if (viewportUpdateQueued_ || IsBeingDeleted()) return;
        viewportUpdateQueued_ = true;
        // Changing scrollbars can synchronously raise another size event. Wait
        // until wxWidgets has finished the current layout, and coalesce events.
        CallAfter([this]() {
            if (IsBeingDeleted()) return;
            const wxSize before = GetClientSize();
            if (before.x > 0 && before.y > 0) {
                if (fit_) {
                    const int drawable = std::max(1, before.x - 2 * Margin());
                    pixelsPerSecond_ = std::max(.001, double(drawable) / duration_);
                }
                UpdateVirtualSize();
                int scrollX = 0, scrollY = 0;
                GetViewStart(&scrollX, &scrollY);
                if (fit_ && (scrollX != 0 || scrollY != 0)) Scroll(0, 0);
                Refresh(false);
            }
            viewportUpdateQueued_ = false;
            // Showing/removing a scrollbar can change the client dimensions.
            // Refit once those dimensions settle, never recurse from OnSize.
            if (GetClientSize() != before) QueueViewportUpdate();
        });
    }
    void UpdateVirtualSize() {
        if (updatingVirtualSize_) return;
        const wxSize client = GetClientSize();
        if (client.x <= 0 || client.y <= 0) return;
        // Fit mode must not invent a minimum scroll width for a hidden/narrow
        // notebook page: that made fitting and scrollbar layout fight each other.
        const int width = fit_ ? client.x : std::max(client.x, XFromTime(duration_) + Margin());
        const wxSize desired(width, std::max(client.y, FromDIP(265)));
        if (GetVirtualSize() == desired) return;
        updatingVirtualSize_ = true;
        SetVirtualSize(desired);
        updatingVirtualSize_ = false;
    }
    void NotifySelection(){if(selectionCallback_)selectionCallback_(selected_,selected_>=0&&selected_<static_cast<int>(entries_.size())?&entries_[static_cast<std::size_t>(selected_)]:nullptr);}
    void Seek(double t){SetCursor(t);if(seekCallback_)seekCallback_(cursorSeconds_);}
    int Hit(int x)const{int best=-1,distance=FromDIP(10);for(int i=0;i<static_cast<int>(entries_.size());++i){const int d=std::abs(x-XFromTime(entries_[static_cast<std::size_t>(i)].timeSeconds));if(d<distance){distance=d;best=i;}}return best;}
    void OnPaint(wxPaintEvent&){
        const auto palette=wxui::themePalette(wxui::currentDarkMode());
        wxAutoBufferedPaintDC dc(this);PrepareDC(dc);dc.SetBackground(wxBrush(palette.field));dc.Clear();
        int left=0,top=0,right=0,bottom=0;CalcUnscrolledPosition(0,0,&left,&top);CalcUnscrolledPosition(GetClientSize().x,GetClientSize().y,&right,&bottom);
        const int mid=FromDIP(85),height=FromDIP(90),keyY=FromDIP(195),bottomY=FromDIP(245);
        const auto range=Selection();if(range.second>range.first){dc.SetPen(*wxTRANSPARENT_PEN);dc.SetBrush(wxBrush(wxui::currentDarkMode()?wxColour(42,52,65):wxColour(224,234,248)));dc.DrawRectangle(XFromTime(range.first),FromDIP(18),XFromTime(range.second)-XFromTime(range.first),bottomY-FromDIP(18));}
        dc.SetTextForeground(palette.text);dc.SetPen(wxPen(palette.gridLine));
        double step=.05;while(step*pixelsPerSecond_<FromDIP(65))step*=2;
        for(double t=std::floor(TimeFromX(left)/step)*step;t<=std::min(duration_,TimeFromX(right))+step*.01;t+=step){const auto x=XFromTime(t);dc.DrawLine(x,FromDIP(18),x,bottomY);dc.DrawText(wxString::Format("%.2fs",t),x+FromDIP(2),FromDIP(3));}
        dc.DrawLine(left,mid,right,mid);const auto& wave=Waveform();
        if(wave.sampleRate&&!wave.mono.empty()){
            dc.SetPen(wxPen(wxColour(80,140,205)));
            for(int x=std::max(Margin(),left);x<std::min(XFromTime(wave.durationSeconds()),right+1);++x){const auto a=static_cast<std::size_t>(TimeFromX(x)*wave.sampleRate),b=static_cast<std::size_t>(std::ceil(TimeFromX(x+1)*wave.sampleRate));const auto peak=peaks_.range(a,b);dc.DrawLine(x,mid-static_cast<int>(peak.second*height/2),x,mid-static_cast<int>(peak.first*height/2));}
        }else dc.DrawText("Load matching audio to preview",std::max(left,Margin())+FromDIP(10),mid);
        dc.SetPen(wxPen(palette.text));dc.DrawLine(left,keyY,right,keyY);
        std::size_t first=0,last=entries_.size();
        if(sorted_){auto it=std::lower_bound(entries_.begin(),entries_.end(),TimeFromX(left-FromDIP(15)),[](const auto& e,double t){return e.timeSeconds<t;});first=static_cast<std::size_t>(it-entries_.begin());if(first)--first;last=static_cast<std::size_t>(std::upper_bound(entries_.begin(),entries_.end(),TimeFromX(right+FromDIP(15)),[](double t,const auto& e){return t<e.timeSeconds;})-entries_.begin());}
        std::size_t activeA=entries_.size(),activeB=entries_.size();
        if(sorted_&&!entries_.empty()){auto it=std::upper_bound(entries_.begin(),entries_.end(),cursorSeconds_,[](double t,const auto& e){return t<e.timeSeconds;});activeB=std::min(static_cast<std::size_t>(it-entries_.begin()),entries_.size()-1);activeA=it==entries_.begin()?0:static_cast<std::size_t>(it-entries_.begin()-1);}
        const int d=FromDIP(7);
        for(auto i=first;i<last;++i){const auto& e=entries_[i];const int x=XFromTime(e.timeSeconds);if(x<left-d||x>right+d)continue;
            wxPoint pts[]={{x,keyY-d},{x+d,keyY},{x,keyY+d},{x-d,keyY}};
            dc.SetBrush(wxBrush(static_cast<int>(i)==selected_?wxColour(235,195,85):(i==activeA||i==activeB)?wxColour(90,175,225):wxColour(115,175,115)));dc.SetPen(wxPen(palette.text));dc.DrawPolygon(4,pts);
            if(static_cast<int>(i)==selected_ || pixelsPerSecond_>FromDIP(320))dc.DrawText(wxString::Format("%u",unsigned(e.shape)),x+d, keyY-FromDIP(23));
        }
        for(auto i=std::max<std::size_t>(1,first);i<last;++i){const int a=XFromTime(entries_[i-1].timeSeconds),b=XFromTime(entries_[i].timeSeconds);if(b-left>0&&a<right&&b-a>FromDIP(65))dc.DrawText(entries_[i-1].shape==entries_[i].shape?"hold":"blend",std::max(a,left)+FromDIP(4),keyY+FromDIP(15));}
        const auto end=XFromTime(lipDuration_);dc.SetPen(wxPen(palette.text,FromDIP(2)));if(end>=left&&end<=right){dc.DrawLine(end,FromDIP(18),end,bottomY);dc.DrawText("end",end+FromDIP(3),FromDIP(225));}
        const auto cursor=XFromTime(cursorSeconds_);dc.SetPen(wxPen(wxColour(215,75,65),FromDIP(2)));dc.DrawLine(cursor,FromDIP(18),cursor,bottomY);
        for(const auto& word:words_) {
            const int x=XFromTime(word.startSeconds),end=XFromTime(word.endSeconds);
            if(end<left || x>right || end<=x)continue;
            const int y=FromDIP(140),h=FromDIP(30);
            const bool active=cursorSeconds_>=word.startSeconds&&cursorSeconds_<word.endSeconds;
            dc.SetPen(wxPen(palette.gridLine));dc.SetBrush(wxBrush(active?wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT):palette.field));
            dc.DrawRectangle(x,y,end-x,h);
            dc.SetClippingRegion(x+2,y+1,std::max(1,end-x-4),h-2);
            dc.SetTextForeground(active?wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT):palette.text);dc.DrawText(wxui::toWx(word.text),x+FromDIP(4),y+FromDIP(5));
            dc.DestroyClippingRegion();
        }
        if(HasFocus()){dc.SetPen(wxPen(palette.text,1,wxPENSTYLE_DOT));dc.SetBrush(*wxTRANSPARENT_BRUSH);dc.DrawRectangle(left+2,top+2,std::max(0,right-left-4),std::max(0,bottom-top-4));}
    }
    void OnLeftDown(wxMouseEvent& e){SetFocus();int x=0,y=0;CalcUnscrolledPosition(e.GetX(),e.GetY(),&x,&y);
        if(!e.ShiftDown()&&y>=FromDIP(138)&&y<=FromDIP(174)) {
            const double t=TimeFromX(x);
            for(std::size_t i=0;i<words_.size();++i)if(t>=words_[i].startSeconds&&t<=words_[i].endSeconds){if(wordCallback_)wordCallback_(i);return;}
        }
        Seek(std::min(duration_,TimeFromX(x)));
        selecting_=e.ShiftDown();dragEdited_=false;
        if(selecting_){rangeA_=rangeB_=cursorSeconds_;selected_=-1;}
        else selected_=Hit(x);
        dragging_=!selecting_&&selected_>0&&selected_+1<static_cast<int>(entries_.size());
        if((selecting_||dragging_)&&!HasCapture())CaptureMouse();NotifySelection();Refresh();}
    void OnMotion(wxMouseEvent& e){if(!e.Dragging()||!e.LeftIsDown())return;int x=0,y=0;CalcUnscrolledPosition(e.GetX(),e.GetY(),&x,&y);
        if(selecting_){rangeB_=std::clamp(TimeFromX(x),0.0,duration_);Refresh();return;}
        if(!dragging_||selected_<=0||selected_+1>=static_cast<int>(entries_.size()))return;
        const auto i=static_cast<std::size_t>(selected_);const double low=std::nextafter(static_cast<float>(entries_[i-1].timeSeconds),std::numeric_limits<float>::infinity()),high=std::nextafter(static_cast<float>(entries_[i+1].timeSeconds),-std::numeric_limits<float>::infinity());
        if(!std::isfinite(low)||!std::isfinite(high)||low>high)return;const double t=static_cast<float>(std::clamp(TimeFromX(x),low,high));if(t==entries_[i].timeSeconds)return;
        if(!dragEdited_){if(beforeEdit_)beforeEdit_();dragEdited_=true;}entries_[i].timeSeconds=t;cursorSeconds_=t;NotifySelection();Refresh();}
    void OnLeftUp(wxMouseEvent&){if(HasCapture())ReleaseMouse();dragging_=selecting_=false;NotifySelection();}
    void OnKey(wxKeyEvent& e){const auto key=e.GetKeyCode();const auto delta=e.ControlDown()?.1:.01;
        if(key==WXK_SPACE){if(playCallback_)playCallback_();return;}
        double next=cursorSeconds_;if(key==WXK_LEFT)next-=delta;else if(key==WXK_RIGHT)next+=delta;else if(key==WXK_HOME)next=0;else if(key==WXK_END)next=duration_;else if(key=='F'||key=='f'){FitClip();return;}else if(key=='Z'||key=='z'){ZoomSelection();return;}else{e.Skip();return;}
        if(e.ShiftDown()){if(rangeA_==rangeB_)rangeA_=cursorSeconds_;rangeB_=std::clamp(next,0.0,duration_);}Seek(next);}
};


#ifndef __EMSCRIPTEN__
class FileDrop final : public wxFileDropTarget {
    std::function<bool(const wxArrayString&)> handler_;
public:
    explicit FileDrop(std::function<bool(const wxArrayString&)> handler) : handler_(std::move(handler)) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& files) override { return handler_(files); }
};
#endif

// Only value objects cross the worker/UI boundary. No worker touches a widget.
struct UiMessage {
    enum class Kind { Progress, Preview, Single, Plan, Item, Batch, Scan, Lookup, Audio, BatchAudio, UserWords, LegacyImported, Error };
    Kind kind = Kind::Error;
    neolip::Progress progress;
    neolip::Result single;
    neolip::BatchPlan plan;
    std::shared_ptr<neolip::DialoguePlan> dialogue;
    neolip::BatchResult batch;
    neolip::ScanResult scan;
    cslu::TextToNtpResult lookup;
    std::shared_ptr<const neolip::AudioBuffer> audio;
    fs::path path;
    std::vector<neolip::UserPronunciation> userEntries;
    std::string error;
};
wxDEFINE_EVENT(NEOLIP_ENGINE_EVENT, wxThreadEvent);

class PronunciationDialog final : public wxDialog {
    neolip::Assets assets_;
    wxTextCtrl *word_, *sounds_, *phones_;
    wxStaticText* preview_;
    neolip::UserPronunciation result_;
public:
    PronunciationDialog(wxWindow* parent, const neolip::Assets& assets,
                        const neolip::UserPronunciation& entry)
        : wxDialog(parent, wxID_ANY, "Add pronunciation", wxDefaultPosition,
                   wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), assets_(assets) {
        auto* outer = new wxBoxSizer(wxVERTICAL);
        auto* intro = new wxStaticText(this, wxID_ANY,
            "Describe the pronunciation with familiar words, for example: bog uh.\nNeoLIP saves it for future jobs; the bundled dictionaries stay unchanged.");
        outer->Add(intro, 0, wxEXPAND | wxALL, 12);
        auto* grid = new wxFlexGridSizer(2, 8, 8); grid->AddGrowableCol(1, 1);
        word_ = new wxTextCtrl(this, wxID_ANY, wxui::toWx(entry.word));
        sounds_ = new wxTextCtrl(this, wxID_ANY, wxui::toWx(entry.soundsLike));
        AddPickerRow(this, grid, "Word", word_); AddPickerRow(this, grid, "Sounds like", sounds_);
        outer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        auto* advanced = new wxCollapsiblePane(this, wxID_ANY, "Advanced: CSLU phone expression");
        auto* box = new wxBoxSizer(wxVERTICAL);
        phones_ = new wxTextCtrl(advanced->GetPane(), wxID_ANY, wxui::toWx(entry.phones),
            wxDefaultPosition, wxSize(-1, 70), wxTE_MULTILINE);
        box->Add(new wxStaticText(advanced->GetPane(), wxID_ANY,
            "Leave empty to use Sounds like. An explicit phone expression takes precedence."), 0, wxALL, 6);
        box->Add(phones_, 1, wxEXPAND | wxALL, 6); advanced->GetPane()->SetSizer(box);
        outer->Add(advanced, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
        auto* previewButton = new wxButton(this, wxID_ANY, "Check pronunciation");
        outer->Add(previewButton, 0, wxALL, 12);
        preview_ = new wxStaticText(this, wxID_ANY, "");
        outer->Add(preview_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        outer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 12);
        SetSizer(outer); SetMinSize(wxSize(520, 330)); SetSize(620, 380); CentreOnParent();
        advanced->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent&) { Layout(); });
        // Editing the readable guide invalidates a previously generated/loaded
        // phone sequence, so a hidden advanced field cannot override new edits.
        sounds_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { phones_->ChangeValue(wxString{}); });
        previewButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Resolve(false); });
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (Resolve(true)) EndModal(wxID_OK); }, wxID_OK);
        wxui::applyTheme(this, wxui::currentDarkMode());
    }
    const neolip::UserPronunciation& Entry() const { return result_; }
private:
    bool Resolve(bool) {
        try {
            result_ = neolip::resolvePronunciation(assets_,
                {wxui::toStd(word_->GetValue()), wxui::toStd(sounds_->GetValue()), wxui::toStd(phones_->GetValue())});
            preview_->SetLabel("Pronunciation: " + wxui::toWx(result_.phones));
            preview_->Wrap(560); Layout(); return true;
        } catch (const std::exception& error) {
            wxMessageBox(wxui::toWx(error.what()), "Check pronunciation", wxOK | wxICON_WARNING, this); return false;
        }
    }
};

class MainFrame final : public wxFrame {
public:
    MainFrame() : wxFrame(nullptr, wxID_ANY, "NeoLIP v1.0.0") {
        SetIcon(wxIcon(neolip_icon_xpm));
        BuildMenus(); BuildUi(); LoadConfig(); BindEditor();
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { OnPlaybackTick(); }, playbackTimer_.GetId());
        Bind(wxEVT_CHAR_HOOK, &MainFrame::OnKeyboard, this);
        wxui::configureResponsiveWindow(*this, wxSize(1000, 830), wxSize(760, 570));
        settings_.restoreWindowPlacement(*this);
        wxui::constrainWindowToDisplay(*this);
        Bind(NEOLIP_ENGINE_EVENT, &MainFrame::OnEngineMessage, this);
        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
        fontScaleWheelFilter_.attach(this, [this](int steps) { ChangeFontScaleSteps(steps); });
        neoview::bindFontScaleDpiRefresh(this, [this]() { ApplyFontScale(); });
#ifndef __EMSCRIPTEN__
        SetDropTarget(new FileDrop([this](const wxArrayString& files) { return HandleDrop(files); }));
        wavPicker_->SetDropTarget(new FileDrop([this](const wxArrayString& files) { return HandleDrop(files); }));
        literalText_->SetDropTarget(new FileDrop([this](const wxArrayString& files) { return HandleDrop(files); }));
#endif
        ApplyDarkMode(settings_.darkMode());
        wxui::setStatusText(*this, "Choose audio and enter its spoken text.");
        // Opening the application must not read dictionaries or validate an old
        // import. Load the list only when its page is used; migration is explicit.
        notebook_->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& event) {
            event.Skip();
            if (event.GetSelection() == 3 && !userWordsLoaded_ && !running_) LoadUserWords();
        });
    }
    ~MainFrame() override {
        cancel_.store(true);
#ifndef __EMSCRIPTEN__
        if (worker_.joinable()) worker_.join();
#endif
        playbackTimer_.Stop(); playback_.stop();
        SaveConfig();
    }
private:
    wxNotebook* notebook_ = nullptr;
    wxFilePickerCtrl* wavPicker_ = nullptr;
    wxTextCtrl* literalText_ = nullptr;
    wxChoice *movement_ = nullptr, *batchMovement_ = nullptr, *existingPolicy_ = nullptr;
    wxStaticText *outputLabel_ = nullptr, *transcriptLabel_ = nullptr, *batchSummary_ = nullptr, *batchOutputLabel_ = nullptr;
    wxDirPickerCtrl *batchInput_ = nullptr, *scanInput_ = nullptr;
    wxChoice* batchSource_ = nullptr;
    wxPanel* dialogueOptions_ = nullptr;
    wxFilePickerCtrl *dialoguePicker_ = nullptr, *dialogueTlk_ = nullptr;
    wxStaticText *batchIntro_ = nullptr, *batchInputLabel_ = nullptr;
    wxButton *dialogueReview_ = nullptr, *batchScan_ = nullptr;
    std::optional<neolip::DialoguePlan> dialoguePlan_;
    wxString scannedDialogue_, scannedTalkTable_;
    wxCheckBox *recursive_ = nullptr, *scanRecursive_ = nullptr;
    wxButton *batchRun_ = nullptr, *cancelButton_ = nullptr, *openEditorButton_ = nullptr,
        *showFolderButton_ = nullptr, *newButton_ = nullptr, *addUnknownButton_ = nullptr, *retryButton_ = nullptr;
    wxPanel* resultPanel_ = nullptr;
    wxStaticText *resultTitle_ = nullptr, *resultBody_ = nullptr;
    wxGauge* gauge_ = nullptr;
    wxCollapsiblePane* detailsPane_ = nullptr;
    wxTextCtrl* details_ = nullptr;
    wxListCtrl *batchList_ = nullptr, *userList_ = nullptr, *unknownList_ = nullptr;
    wxTextCtrl* lookup_ = nullptr;
    std::optional<neolip::Assets> assets_;
    std::vector<neolip::UserPronunciation> userEntries_;
    bool userWordsLoaded_ = false;
    wxButton* legacyImportButton_ = nullptr;
    std::vector<std::string> unknownWords_;
    neolip::ScanResult scanResult_;
    neolip::BatchPlan batchPlan_;
    neolip::BatchResult lastBatch_;
    std::optional<neolip::CreateRequest> lastCreate_;
    fs::path createOutput_, lastAudio_, batchOutput_, lastOutput_;
    wxString lastAudioDir_, lastBatchDir_, lastOutputDir_, legacyDictionary_;
    bool scannedRecursive_ = true, batchRetry_ = false;
    bool textDirty_ = false, suppressText_ = false, running_ = false,
         closingAfterJob_ = false, lastWasBatch_ = false;
    std::atomic<bool> cancel_{false};
#ifndef __EMSCRIPTEN__
    std::thread worker_;
#endif
    neosettings::AppSettings settings_{kConfigName};
    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    double fontScale_ = neoview::kDefaultFontScale;
    wxMenuItem* darkModeMenuItem_ = nullptr;
    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;

    // Existing timeline/table editor, without PHN or redundant output pickers.
    wxFilePickerCtrl *timelineAudioPicker_ = nullptr, *timelineLipPicker_ = nullptr;
    LipTimelinePanel* timelinePanel_ = nullptr;
    wxStaticText *timelineStatus_ = nullptr, *timelineSelection_ = nullptr, *lipEditorStatus_ = nullptr;
    wxChoice* timelineShapeChoice_ = nullptr;
    wxSlider* timelineZoom_ = nullptr;
    wxGrid* lipGrid_ = nullptr;
    wxSplitterWindow* editorSplitter_ = nullptr;
    wxPanel* inspectorPane_ = nullptr;
    wxTextCtrl* lipShiftAmount_ = nullptr;
    std::vector<GuiLipEntry> lipEditorEntries_, timelineEntries_;
    std::shared_ptr<const neolip::AudioBuffer> timelineAudio_;
    const GuiWaveform& TimelineWaveform() const { static const GuiWaveform empty; return timelineAudio_ ? timelineAudio_->waveform : empty; }
    neolip::AudioPlayback playback_;
    wxTimer playbackTimer_{this};
    wxButton* playButton_ = nullptr;
    wxCheckBox* loopSelection_ = nullptr;
    wxChoice* previewMovement_ = nullptr;
    wxPanel* reviewPanel_ = nullptr;
    wxChoice* reviewChoice_ = nullptr;
    std::vector<neolip::ReviewWarning> reviewWarnings_;
    std::vector<neolip::Result> batchRowResults_;
    std::optional<neolip::Result> pendingAudioReview_;
    std::vector<std::size_t> visibleBatchRows_;
    wxChoice* batchFilter_ = nullptr;
    wxStaticText* documentState_ = nullptr;
    wxStaticText* blendInspector_ = nullptr;
    wxChoice* wordChoice_ = nullptr;
    wxCheckBox* mutePreview_ = nullptr;
    wxSlider* previewVolume_ = nullptr;
    std::vector<neolip::AlignedWord> editorWords_;
    std::string editorTranscript_;
    fs::path editorInput_;
    int editorChannel_ = -1;
    double replayEnd_ = -1;

    std::shared_ptr<neolip::Engine> singleEngine_;
    neolip::Result lastSingle_;
    int createChannel_ = -1;
    bool playbackTicking_ = false, presetPreviewAvailable_ = false;
    bool clearingHighlights_ = false, presetOnlyChange_ = false;
    int displayedPreviewSelection_ = 0;
    double timelineLipDuration_ = 0.0;
    fs::path editorPath_;
    bool synchronizingEditViews_ = false, lipEditorDirty_ = false;
    struct EditSnapshot { std::vector<GuiLipEntry> keys; double duration = 0; };
    std::vector<EditSnapshot> undoLip_, redoLip_;
    EditSnapshot savedLip_;

    bool SameSnapshot(const EditSnapshot& snapshot) const {
        if (snapshot.duration != timelineLipDuration_ || snapshot.keys.size() != lipEditorEntries_.size()) return false;
        for (std::size_t i = 0; i < snapshot.keys.size(); ++i)
            if (snapshot.keys[i].timeSeconds != lipEditorEntries_[i].timeSeconds || snapshot.keys[i].shape != lipEditorEntries_[i].shape) return false;
        return true;
    }
    void RememberLipEdit() {
        PausePlayback();
        presetOnlyChange_ = false;
        if (undoLip_.empty() || !SameSnapshot(undoLip_.back())) {
            if (undoLip_.size() >= 100) undoLip_.erase(undoLip_.begin());
            undoLip_.push_back({lipEditorEntries_, timelineLipDuration_});
        }
        redoLip_.clear();
    }
    void RestoreLipEdit(bool redo) {
        if (running_) return;
        PausePlayback();
        auto& source = redo ? redoLip_ : undoLip_;
        auto& target = redo ? undoLip_ : redoLip_;
        if (source.empty()) return;
        target.push_back({lipEditorEntries_, timelineLipDuration_});
        auto snapshot = std::move(source.back()); source.pop_back();
        lipEditorEntries_ = std::move(snapshot.keys); timelineLipDuration_ = snapshot.duration; presetOnlyChange_=false;
        timelineEntries_ = lipEditorEntries_; lipEditorDirty_ = !SameSnapshot(savedLip_);
        RefreshLipGrid(); RefreshTimelinePanel();
    }

    wxButton* Button(wxWindow* parent, wxSizer* row, const wxString& label,
                     std::function<void()> handler) {
        auto* button = new wxButton(parent, wxID_ANY, label);
        row->Add(button, 0, wxRIGHT | wxBOTTOM, 7);
        button->Bind(wxEVT_BUTTON, [handler = std::move(handler)](wxCommandEvent&) { handler(); });
        return button;
    }
    wxChoice* MovementChoice(wxWindow* parent) {
        auto* choice = new wxChoice(parent, wxID_ANY);
        choice->Append("Natural"); choice->Append("Minimal smoothing"); choice->SetSelection(0);
        choice->SetToolTip("Both preserve WAV-aligned pose order and holds. Natural uses broader bounded blends; Minimal smoothing uses shorter transitions.");
        return choice;
    }
    neolip::MouthMovement SelectedMovement(wxChoice* choice) const {
        return choice->GetSelection() == 1 ? neolip::MouthMovement::MinimalSmoothing : neolip::MouthMovement::Natural;
    }
    void BuildMenus() {
        auto* menuBar = new wxMenuBar(); auto* file = new wxMenu();
        auto* gameMenu = new wxMenu();
        gameMenu->Append(wxID_ANY, "Saved game directories")->Enable(false);
        file->AppendSubMenu(gameMenu, "Use Game Directory for &Dialogue");
        // The shared menu probes saved paths (including unavailable drives).
        // Construct it on demand, not before the main window can be displayed.
        Bind(wxEVT_MENU_OPEN, [this, gameMenu](wxMenuEvent& event) {
            if (event.GetMenu() == gameMenu && !gameDirectoryMenu_) {
                gameDirectoryMenu_ = std::make_unique<neogames::OpenGameDirectoryMenu>(
                    *this, *gameMenu,
                    [this](const neogames::SavedGameDirectory& directory) { UseDialogueFolder(directory.path); },
                    neogames::GameDirectoryGameIds{"kotor", "kotor2"});
            }
            event.Skip();
        });
        file->AppendSeparator(); file->Append(wxID_EXIT, "E&xit"); menuBar->Append(file, "&File");
        auto* view = new wxMenu(); darkModeMenuItem_ = view->AppendCheckItem(ID_ToggleDarkMode, "Dark mode");
        view->AppendSeparator(); view->Append(ID_FontIncrease, "Increase Font Size\tCtrl++");
        view->Append(ID_FontDecrease, "Decrease Font Size\tCtrl+-"); view->Append(ID_FontReset, "Reset Font Size\tCtrl+0");
        menuBar->Append(view, "&View"); SetMenuBar(menuBar);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
        Bind(wxEVT_MENU, [this](wxCommandEvent& e) { ApplyDarkMode(e.IsChecked()); }, ID_ToggleDarkMode);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { ChangeFontScaleSteps(1); }, ID_FontIncrease);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { ChangeFontScaleSteps(-1); }, ID_FontDecrease);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { fontScale_ = 1; settings_.setFontScale(fontScale_); ApplyFontScale(); }, ID_FontReset);
    }
    void ApplyFontScale() { neoview::applyFontScale(this, fontScale_); if (timelinePanel_) timelinePanel_->FitClip(); }
    void ChangeFontScaleSteps(int steps) {
        fontScale_ = neoview::steppedFontScale(fontScale_, steps); settings_.setFontScale(fontScale_); ApplyFontScale();
    }
    void ApplyDarkMode(bool enabled) {
        darkModeMenuItem_->Check(enabled); settings_.setDarkMode(enabled); wxui::applyTheme(this, enabled);
        ApplyFontScale(); if (timelinePanel_) timelinePanel_->Refresh();
    }
    void BuildUi() {
        auto* outer = new wxBoxSizer(wxVERTICAL); SetSizer(outer);
        notebook_ = new wxNotebook(this, wxID_ANY);
        notebook_->AddPage(BuildCreatePage(notebook_), "Create", true);
        notebook_->AddPage(BuildEditPage(notebook_), "Edit");
        notebook_->AddPage(BuildBatchPage(notebook_), "Batch");
        notebook_->AddPage(BuildPronunciationPage(notebook_), "Pronunciations");
        outer->Add(notebook_, 1, wxEXPAND | wxALL, 10);
        resultPanel_ = new wxPanel(this); auto* result = new wxBoxSizer(wxVERTICAL); resultPanel_->SetSizer(result);
        resultTitle_ = new wxStaticText(resultPanel_, wxID_ANY, "Ready");
        auto titleFont = resultTitle_->GetFont(); titleFont.SetWeight(wxFONTWEIGHT_BOLD); resultTitle_->SetFont(titleFont);
        result->Add(resultTitle_, 0, wxEXPAND | wxBOTTOM, 5);
        resultBody_ = new wxStaticText(resultPanel_, wxID_ANY, ""); result->Add(resultBody_, 0, wxEXPAND | wxBOTTOM, 5);
        auto* progress = new wxBoxSizer(wxHORIZONTAL);
        gauge_ = new wxGauge(resultPanel_, wxID_ANY, 1000); progress->Add(gauge_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        cancelButton_ = Button(resultPanel_, progress, "Cancel", [this]() { cancel_.store(true); resultTitle_->SetLabel("Cancelling safely..."); });
        result->Add(progress, 0, wxEXPAND | wxBOTTOM, 5); gauge_->Hide(); cancelButton_->Hide();
        auto* actions = new wxWrapSizer(wxHORIZONTAL);
        openEditorButton_ = Button(resultPanel_, actions, "Open in editor", [this]() { OpenLatestResult(); });
#ifdef __EMSCRIPTEN__
        showFolderButton_ = Button(resultPanel_, actions, "Save result", [this]() { ExportLastOutput(); });
#else
        showFolderButton_ = Button(resultPanel_, actions, "Show in folder", [this]() { ExportLastOutput(); });
#endif
        newButton_ = Button(resultPanel_, actions, "Create another", [this]() { ClearCreate(); });
        addUnknownButton_ = Button(resultPanel_, actions, "Add pronunciations", [this]() { AddUnknownPronunciations(); });
        retryButton_ = Button(resultPanel_, actions, "Retry", [this]() { Retry(); });
        result->Add(actions, 0, wxEXPAND);
        reviewPanel_ = new wxPanel(resultPanel_);
        auto* reviewRow = new wxBoxSizer(wxHORIZONTAL); reviewPanel_->SetSizer(reviewRow);
        reviewChoice_ = new wxChoice(reviewPanel_, wxID_ANY); reviewChoice_->SetName("Sections to review");
        reviewRow->Add(reviewChoice_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        Button(reviewPanel_, reviewRow, "Review section", [this]() { ReviewSection(); });
        result->Add(reviewPanel_, 0, wxEXPAND | wxTOP, FromDIP(5)); reviewPanel_->Hide();
        detailsPane_ = new wxCollapsiblePane(resultPanel_, wxID_ANY, "Technical details");
        auto* detailsBox = new wxBoxSizer(wxVERTICAL);
        details_ = new wxTextCtrl(detailsPane_->GetPane(), wxID_ANY, wxString{}, wxDefaultPosition, wxSize(-1, 90), wxTE_MULTILINE | wxTE_READONLY);
        detailsBox->Add(details_, 1, wxEXPAND);
        auto* copyRow = new wxBoxSizer(wxHORIZONTAL);
        Button(detailsPane_->GetPane(), copyRow, "Copy details", [this]() {
            if (wxTheClipboard->Open()) { wxTheClipboard->SetData(new wxTextDataObject(details_->GetValue())); wxTheClipboard->Close(); }
        });
        detailsBox->Add(copyRow, 0, wxTOP, 4); detailsPane_->GetPane()->SetSizer(detailsBox);
        detailsPane_->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent&) { Layout(); });
        result->Add(detailsPane_, 0, wxEXPAND | wxTOP, 4);
        outer->Add(resultPanel_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);
        resultPanel_->Hide(); wxui::createStatusBar(*this);
    }
    wxPanel* BuildCreatePage(wxWindow* parent) {
        auto* panel = new wxPanel(parent); auto* outer = new wxBoxSizer(wxVERTICAL); panel->SetSizer(outer);
        outer->Add(new wxStaticText(panel, wxID_ANY, "Choose audio and enter exactly what the speaker says. Numbers should be written as spoken words."), 0, wxALL, 12);
        auto* grid = new wxFlexGridSizer(2, 8, 8); grid->AddGrowableCol(1);
        wavPicker_ = new wxFilePickerCtrl(panel, wxID_ANY, wxString{}, "Choose audio",
            "WAV / MP3 audio (*.wav;*.mp3)|*.wav;*.WAV;*.mp3;*.MP3|All files (*.*)|*.*", wxDefaultPosition, wxDefaultSize,
            wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
        wavPicker_->SetName("Source audio recording"); AddPickerRow(panel, grid, "Audio", wavPicker_); outer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        transcriptLabel_ = new wxStaticText(panel, wxID_ANY, "Spoken text");
        outer->Add(transcriptLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
        literalText_ = new wxTextCtrl(panel, wxID_ANY, wxString{}, wxDefaultPosition, FromDIP(wxSize(-1, 200)), wxTE_MULTILINE | wxTE_RICH2);
        outer->Add(literalText_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        auto* textActions = new wxBoxSizer(wxHORIZONTAL);
        Button(panel, textActions, "Load transcript...", [this]() { ImportTranscript(); });
        Button(panel, textActions, "Say as...", [this]() { SaySelectedText(); });
        Button(panel, textActions, "Check words", [this]() { CheckTranscript(); });
        literalText_->SetName("Spoken transcript");
        literalText_->SetName("Spoken transcript");
        literalText_->SetToolTip("The visible text is authoritative. Select a name or number, then Say as, to replace only that selection with its spoken form. The original transcript file is not edited.");
        literalText_->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent&) {
            wxMenu menu; const auto say = wxWindow::NewControlId(), add = wxWindow::NewControlId();
            menu.Append(say, "Say selected text as..."); menu.Append(add, "Add selected word pronunciation...");
            menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { SaySelectedText(); }, say);
            menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { AddSelectedTranscriptWord(); }, add);
            menu.AppendSeparator(); menu.Append(wxID_CUT, "Cut"); menu.Append(wxID_COPY, "Copy"); menu.Append(wxID_PASTE, "Paste");
            menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { literalText_->Cut(); }, wxID_CUT);
            menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { literalText_->Copy(); }, wxID_COPY);
            menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { literalText_->Paste(); }, wxID_PASTE);
            literalText_->PopupMenu(&menu);
        });
        textActions->AddStretchSpacer(); textActions->Add(new wxStaticText(panel, wxID_ANY, "Mouth movement"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        movement_ = MovementChoice(panel); textActions->Add(movement_, 0);
        outer->Add(textActions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        outputLabel_ = new wxStaticText(panel, wxID_ANY, "Output: same name and folder as the audio");
        outer->Add(outputLabel_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        auto* actions = new wxWrapSizer(wxHORIZONTAL);
        auto* create = Button(panel, actions, "Create LIP", [this]() { RunCreate(); }); create->SetDefault();
        Button(panel, actions, "Save somewhere else...", [this]() { ChooseCreateOutput(); });
        Button(panel, actions, "Compare mouth presets...", [this]() { PreviewPreset(SelectedMovement(movement_)); });
        Button(panel, actions, "Use audio folder", [this]() { if (!lastAudio_.empty()) { createOutput_ = neolip::defaultLipPath(lastAudio_); UpdateOutputLabel(); } });
        outer->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        wavPicker_->Bind(wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent& event) { AudioSelected(ToPath(event.GetPath())); });
        literalText_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { if (!suppressText_) { textDirty_ = true; ClearTranscriptHighlights(); } });
        movement_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { batchMovement_->SetSelection(movement_->GetSelection()); });
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
        timelineAudioPicker_->SetName("Preview audio"); timelineLipPicker_->SetName("LIP animation file");
        AddPickerRow(panel, grid, "Audio", timelineAudioPicker_);
        AddPickerRow(panel, grid, "LIP", timelineLipPicker_);
        outer->Add(grid, 0, wxEXPAND | wxALL, 10);

        auto* controls = new wxWrapSizer(wxHORIZONTAL);
        controls->Add(new wxButton(panel, ID_SaveLipEditor, "Save"), 0, wxRIGHT, 8);
        controls->Add(new wxButton(panel, ID_SaveLipEditorAs, "Save As..."), 0, wxRIGHT, 8);
        playButton_ = new wxButton(panel, ID_TimelinePlayCursor, "Play");
        playButton_->SetToolTip("Play or pause (Space when editing the timeline)"); controls->Add(playButton_, 0, wxRIGHT, FromDIP(8));
        Button(panel, controls, "Stop", [this]() { StopPlayback(); });
        loopSelection_ = new wxCheckBox(panel, wxID_ANY, "Loop selection");
        loopSelection_->SetToolTip("Shift-drag the timeline to select the loop range.");
        controls->Add(loopSelection_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        loopSelection_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { ConfigureLoop(); });
        Button(panel, controls, "Fit clip", [this]() { timelinePanel_->FitClip(); });
        Button(panel, controls, "Zoom to selection", [this]() { timelinePanel_->ZoomSelection(); });
        controls->Add(new wxButton(panel, ID_UndoLip, "Undo"), 0, wxRIGHT, 8);
        controls->Add(new wxButton(panel, ID_RedoLip, "Redo"), 0, wxRIGHT, 8);
        controls->Add(new wxButton(panel, ID_SetLipDuration, "Set duration..."), 0, wxRIGHT, 8);
        controls->Add(new wxButton(panel, ID_RepairLipKeys, "Repair keys..."), 0, wxRIGHT, 12);
        controls->Add(new wxStaticText(panel, wxID_ANY, "Shape"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        timelineShapeChoice_ = new wxChoice(panel, ID_TimelineShapeChanged);
        for (int i = 0; i < 16; ++i) timelineShapeChoice_->Append(KotorShapeName(i));
        timelineShapeChoice_->SetSelection(0);
        timelineShapeChoice_->SetToolTip("Pose IDs sample (ID + 1) / 16 of the head model talk animation. Phoneme labels are legacy assignments; geometry depends on the model.");
        controls->Add(timelineShapeChoice_, 0, wxRIGHT, 12);
        controls->Add(new wxStaticText(panel, wxID_ANY, "Zoom"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        timelineZoom_ = new wxSlider(panel, ID_TimelineZoomChanged, 1, 1, 20,
                                     wxDefaultPosition, FromDIP(wxSize(150, -1)), wxSL_HORIZONTAL);
        controls->Add(timelineZoom_, 0, wxRIGHT, 12);
        outer->Add(controls, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        auto* presets = new wxWrapSizer(wxHORIZONTAL);
        presets->Add(new wxStaticText(panel, wxID_ANY, "Compare generated movement"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        previewMovement_ = MovementChoice(panel); previewMovement_->Enable(false); presets->Add(previewMovement_, 0, wxRIGHT, FromDIP(8));
        previewMovement_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { PreviewPreset(SelectedMovement(previewMovement_)); });
        Button(panel, presets, "Save selected result", [this]() { SaveLipEditor(false); });
        presets->Add(new wxStaticText(panel, wxID_ANY, "Preset changes are previews until saved."), 0, wxALIGN_CENTER_VERTICAL);
        outer->Add(presets, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        documentState_=new wxStaticText(panel,wxID_ANY,"No document");
        outer->Add(documentState_,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(8));
        auto* wordRow=new wxWrapSizer(wxHORIZONTAL);
        wordRow->Add(new wxStaticText(panel,wxID_ANY,"Aligned word"),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(6));
        wordChoice_=new wxChoice(panel,wxID_ANY);wordChoice_->SetName("Aligned words from source recording");
        wordChoice_->SetMinSize(FromDIP(wxSize(230,-1)));wordChoice_->Enable(false);
        wordRow->Add(wordChoice_,0,wxRIGHT,FromDIP(6));
        wordChoice_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){if(wordChoice_->GetSelection()>=0)SelectAlignedWord(static_cast<std::size_t>(wordChoice_->GetSelection()));});
        Button(panel,wordRow,"Replay selection",[this](){ReplaySelection();});
        Button(panel,wordRow,"Correct word...",[this](){CorrectAlignedWord();});
        auto* inspector=new wxCheckBox(panel,wxID_ANY,"Key inspector");
        inspector->Bind(wxEVT_CHECKBOX,[this,inspector](wxCommandEvent&){
            if(inspector->GetValue()){inspectorPane_->Show();editorSplitter_->SplitHorizontally(editorSplitter_->GetWindow1(),inspectorPane_,FromDIP(320));}
            else {editorSplitter_->Unsplit(inspectorPane_);inspectorPane_->Hide();}
            Layout();
        });wordRow->Add(inspector,0,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(8));
        outer->Add(wordRow,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(8));
        auto* soundRow=new wxWrapSizer(wxHORIZONTAL);
        soundRow->Add(new wxStaticText(panel,wxID_ANY,"Preview volume"),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(6));
        previewVolume_=new wxSlider(panel,wxID_ANY,100,0,100,wxDefaultPosition,FromDIP(wxSize(140,-1)));
        previewVolume_->SetName("Preview volume; does not affect generated LIP");
        mutePreview_=new wxCheckBox(panel,wxID_ANY,"Mute");
        soundRow->Add(previewVolume_,0,wxRIGHT,FromDIP(6));soundRow->Add(mutePreview_,0,wxALIGN_CENTER_VERTICAL);
        const auto gain=[this](wxCommandEvent&){playback_.setVolume(mutePreview_->GetValue()?0.f:previewVolume_->GetValue()/100.f);};
        previewVolume_->Bind(wxEVT_SLIDER,gain);mutePreview_->Bind(wxEVT_CHECKBOX,gain);
        outer->Add(soundRow,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(8));

        timelineSelection_ = new wxStaticText(panel, wxID_ANY, "No key selected.");
        timelineStatus_ = new wxStaticText(panel, wxID_ANY,
            "Open a LIP to edit its mouth keys. A matching WAV is loaded automatically.");
        outer->Add(timelineSelection_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
        outer->Add(timelineStatus_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        auto* splitter = new wxSplitterWindow(
            panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxSP_LIVE_UPDATE | wxSP_3D);
        splitter->SetMinimumPaneSize(FromDIP(160));
        splitter->SetSashGravity(0.56);

        auto* timelinePane = new wxPanel(splitter);
        auto* timelineSizer = new wxBoxSizer(wxVERTICAL);
        timelinePane->SetSizer(timelineSizer);
        timelinePanel_ = new LipTimelinePanel(timelinePane);
        timelinePanel_->SetWordCallback([this](std::size_t i){SelectAlignedWord(i);});
        timelinePanel_->SetBeforeEditCallback([this]() { PausePlayback(); if (!synchronizingEditViews_) RememberLipEdit(); });
        timelinePanel_->SetPlayCallback([this]() { TogglePlayback(); });
        timelinePanel_->SetSeekCallback([this](double seconds) { SeekPlayback(seconds); });
        timelinePanel_->SetSelectionCallback([this](int idx, const GuiLipEntry* entry) {
            if (entry != nullptr) {
                timelineSelection_->SetLabel(wxString::Format(
                    "selected key %d   time %.3fs   shape %s",
                    idx, entry->timeSeconds, KotorShapeName(entry->shape)));
                if (timelineShapeChoice_ != nullptr) {
                    timelineShapeChoice_->SetSelection(entry->shape <= 15 ? entry->shape : wxNOT_FOUND);
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
                    lipGrid_->SetGridCursor(idx, 1); lipGrid_->SelectRow(idx);
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
        blendInspector_=new wxStaticText(tablePane,wxID_ANY,wxString{});
        tableSizer->Add(blendInspector_,0,wxEXPAND|wxALL,FromDIP(6));
        outer->Detach(timelineSelection_);timelineSelection_->Reparent(tablePane);
        tableSizer->Add(timelineSelection_,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(6));
        auto* tableControls = new wxBoxSizer(wxHORIZONTAL);
        tableControls->Add(new wxButton(tablePane, ID_InsertLipKey, "Insert key"), 0, wxRIGHT, 8);
        tableControls->Add(new wxButton(tablePane, ID_DeleteLipKeys, "Delete selected"), 0, wxRIGHT, 8);
        lipShiftAmount_ = new wxTextCtrl(tablePane, wxID_ANY, "0.050", wxDefaultPosition, FromDIP(wxSize(80, -1)));
        tableControls->Add(new wxStaticText(tablePane, wxID_ANY, "Shift seconds"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        tableControls->Add(lipShiftAmount_, 0, wxRIGHT, 8);
        tableControls->Add(new wxButton(tablePane, ID_ShiftLipKeys, "Shift selected"), 0, wxRIGHT, 8);
        tableControls->AddStretchSpacer(1);
        tableSizer->Add(tableControls, 0, wxEXPAND | wxALL, 6);
        lipEditorStatus_ = new wxStaticText(tablePane, wxID_ANY, "No mouth keys loaded.");
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
        lipGrid_->SetColSize(3, FromDIP(220));
        lipGrid_->EnableEditing(true);
        lipGrid_->SetSelectionMode(wxGrid::wxGridSelectRows);
        tableSizer->Add(lipGrid_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

        editorSplitter_=splitter;inspectorPane_=tablePane;
        splitter->SplitHorizontally(timelinePane, tablePane, FromDIP(340));
        splitter->Unsplit(tablePane);tablePane->Hide();
        outer->Add(splitter, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        return panel;
    }

    wxPanel* BuildBatchPage(wxWindow* parent) {
        auto* panel = new wxPanel(parent); auto* outer = new wxBoxSizer(wxVERTICAL); panel->SetSizer(outer);
        auto* sourceRow = new wxBoxSizer(wxHORIZONTAL);
        sourceRow->Add(new wxStaticText(panel, wxID_ANY, "Source"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        batchSource_ = new wxChoice(panel, wxID_ANY);
        batchSource_->Append("Audio + transcript files"); batchSource_->Append("Dialogue (DLG)"); batchSource_->SetSelection(0);
        sourceRow->Add(batchSource_, 1, wxEXPAND);
        outer->Add(sourceRow, 0, wxEXPAND | wxALL, FromDIP(12));
        batchIntro_ = new wxStaticText(panel, wxID_ANY, "Match each WAV or MP3 with a same-name .txt or .lab transcript.");
        outer->Add(batchIntro_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        dialogueOptions_ = new wxPanel(panel); auto* dlgFields = new wxBoxSizer(wxVERTICAL); dialogueOptions_->SetSizer(dlgFields);
        dlgFields->Add(new wxStaticText(dialogueOptions_, wxID_ANY, "Dialogue file"), 0, wxBOTTOM, FromDIP(4));
        dialoguePicker_ = new wxFilePickerCtrl(dialogueOptions_, wxID_ANY, wxString{}, "Choose dialogue", "KotOR dialogue (*.dlg)|*.dlg;*.DLG|All files|*", wxDefaultPosition, wxDefaultSize, wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
        dlgFields->Add(dialoguePicker_, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        dlgFields->Add(new wxStaticText(dialogueOptions_, wxID_ANY, "Talk table (automatic; choose only to override the selected folder's dialog.tlk)"), 0, wxBOTTOM, FromDIP(4));
        dialogueTlk_ = new wxFilePickerCtrl(dialogueOptions_, wxID_ANY, wxString{}, "Choose talk table", "Talk table (*.tlk)|*.tlk;*.TLK|All files|*", wxDefaultPosition, wxDefaultSize, wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
        dlgFields->Add(dialogueTlk_, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        outer->Add(dialogueOptions_, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12)); dialogueOptions_->Hide();
        batchInputLabel_ = new wxStaticText(panel, wxID_ANY, "Audio/transcript folder");
        outer->Add(batchInputLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        batchInput_ = new wxDirPickerCtrl(panel, wxID_ANY, wxString{}, "Choose a game directory or any search folder");
        outer->Add(batchInput_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        auto* options = new wxWrapSizer(wxHORIZONTAL);
        recursive_ = new wxCheckBox(panel, wxID_ANY, "Include subfolders"); recursive_->SetValue(true);
        options->Add(recursive_, 0, wxRIGHT | wxBOTTOM | wxALIGN_CENTER_VERTICAL, 10);
        options->Add(new wxStaticText(panel, wxID_ANY, "Existing LIPs"), 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
        existingPolicy_ = new wxChoice(panel, wxID_ANY); existingPolicy_->Append("Skip existing"); existingPolicy_->Append("Replace existing"); existingPolicy_->SetSelection(0);
        options->Add(existingPolicy_, 0, wxRIGHT | wxBOTTOM, 12);
        options->Add(new wxStaticText(panel, wxID_ANY, "Mouth movement"), 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
        batchMovement_ = MovementChoice(panel); options->Add(batchMovement_, 0, wxBOTTOM, 7);
        outer->Add(options, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
        batchOutputLabel_ = new wxStaticText(panel, wxID_ANY, "Output: beside each audio file");
        outer->Add(batchOutputLabel_, 0, wxEXPAND | wxALL, 12);
        auto* actions = new wxWrapSizer(wxHORIZONTAL);
        batchScan_ = Button(panel, actions, "Scan folder", [this]() { ScanBatch(); });
#ifndef __EMSCRIPTEN__
        Button(panel, actions, "Choose a different output folder...", [this]() { ChooseBatchOutput(); });
#endif
        Button(panel, actions, "Use default output", [this]() { batchOutput_.clear(); InvalidateBatch(); });
        batchRun_ = Button(panel, actions, "Create LIP files", [this]() { RunBatch(false); }); batchRun_->Enable(false);
        outer->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
        batchSummary_ = new wxStaticText(panel, wxID_ANY, "Choose a folder to find matching pairs.");
        outer->Add(batchSummary_, 0, wxEXPAND | wxALL, 12);
        auto* filterRow=new wxWrapSizer(wxHORIZONTAL);
        filterRow->Add(new wxStaticText(panel,wxID_ANY,"Show"),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(6));
        batchFilter_=new wxChoice(panel,wxID_ANY);
        for(const char* name:{"All","Needs pronunciation","Needs channel","Review suggested","Created","Skipped","Failed / cancelled"})batchFilter_->Append(name);
        batchFilter_->SetSelection(0);batchFilter_->SetName("Filter batch results");batchFilter_->Enable(false);
        filterRow->Add(batchFilter_,0,wxRIGHT,FromDIP(8));
        batchFilter_->Bind(wxEVT_CHOICE,[this](wxCommandEvent&){RefreshBatchRows();});
        Button(panel,filterRow,"Previous problem",[this](){NextBatchProblem(-1);});
        Button(panel,filterRow,"Next problem",[this](){NextBatchProblem(1);});
        Button(panel,filterRow,"Choose voice channel...",[this](){ChooseBatchChannel();});
        dialogueReview_ = Button(panel, filterRow, "Review dialogue line...", [this]() {
            ReviewDialogueRow(batchList_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED));
        }); dialogueReview_->Hide();
        outer->Add(filterRow,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(12));
        batchList_ = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
        batchList_->InsertColumn(0, "Audio", wxLIST_FORMAT_LEFT, 320);
        batchList_->InsertColumn(1, "Result", wxLIST_FORMAT_LEFT, FromDIP(450));
        batchList_->InsertColumn(2, "Spoken text", wxLIST_FORMAT_LEFT, FromDIP(420));
        batchList_->InsertColumn(3, "Recording", wxLIST_FORMAT_LEFT, FromDIP(400));
        batchList_->SetName("Batch results");
        batchList_->SetToolTip("Activate a completed row to review its audio and LIP. Activate a problem row to open it in Create.");
        batchList_->Bind(wxEVT_LIST_ITEM_ACTIVATED,[this](wxListEvent& event){OpenBatchRow(event.GetIndex());});
        outer->Add(batchList_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        batchInput_->Bind(wxEVT_DIRPICKER_CHANGED, [this](wxFileDirPickerEvent&) {
            // A new search tree must not keep another installation's explicit TLK.
            if (DialogueMode()) dialogueTlk_->SetPath(wxString{});
            batchOutput_.clear(); lastBatchDir_ = batchInput_->GetPath(); InvalidateBatch(); ScanBatch();
        });
        batchSource_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateBatchMode(); InvalidateBatch(); });
        dialoguePicker_->Bind(wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent&) {
            batchOutput_.clear();
            if (batchInput_->GetPath().empty()) batchInput_->SetPath(ToWx(ToPath(dialoguePicker_->GetPath()).parent_path()));
            InvalidateBatch(); ScanBatch();
        });
        dialogueTlk_->Bind(wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent&) { InvalidateBatch(); ScanBatch(); });
        recursive_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { InvalidateBatch(); ScanBatch(); });
        existingPolicy_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateBatchSummary(); });
        batchMovement_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { movement_->SetSelection(batchMovement_->GetSelection()); });
        return panel;
    }
    wxPanel* BuildPronunciationPage(wxWindow* parent) {
        auto* panel = new wxPanel(parent); auto* outer = new wxBoxSizer(wxVERTICAL); panel->SetSizer(outer);
        outer->Add(new wxStaticText(panel, wxID_ANY,
            "CSLU and KotOR pronunciations are built in. Add only words or pronunciations you need to change."), 0, wxALL, 12);
        auto* lookupRow = new wxBoxSizer(wxHORIZONTAL);
        lookup_ = new wxTextCtrl(panel, wxID_ANY); lookupRow->Add(lookup_, 1, wxRIGHT, 8);
        Button(panel, lookupRow, "Check word", [this]() { LookupWord(); });
        outer->Add(lookupRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        auto* actions = new wxWrapSizer(wxHORIZONTAL);
        Button(panel, actions, "Add pronunciation...", [this]() { EditPronunciation({}); });
        Button(panel, actions, "Edit selected...", [this]() { EditSelectedPronunciation(); });
        Button(panel, actions, "Remove selected", [this]() { RemoveSelectedPronunciation(); });
        Button(panel, actions, "Import dictionary...", [this]() { ImportUserDictionary(); });
        legacyImportButton_ = Button(panel, actions, "Import previous dictionary", [this]() { MigrateLegacyPronunciations(); });
        legacyImportButton_->SetToolTip("Import the dictionary saved by an older version. Your original file is never changed.");
        legacyImportButton_->Hide();
        outer->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
        userList_ = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 145), wxLC_REPORT | wxLC_SINGLE_SEL);
        userList_->InsertColumn(0, "Your word", wxLIST_FORMAT_LEFT, 160);
        userList_->InsertColumn(1, "Sounds like", wxLIST_FORMAT_LEFT, 210);
        userList_->InsertColumn(2, "Pronunciation", wxLIST_FORMAT_LEFT, 400);
        outer->Add(userList_, 1, wxEXPAND | wxALL, 12);
        userList_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { EditSelectedPronunciation(); });
        outer->Add(new wxStaticText(panel, wxID_ANY, "Find unknown words in a transcript folder"), 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
        scanInput_ = new wxDirPickerCtrl(panel, wxID_ANY, wxString{}, "Choose transcript folder");
        outer->Add(scanInput_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        auto* scanActions = new wxWrapSizer(wxHORIZONTAL);
        scanRecursive_ = new wxCheckBox(panel, wxID_ANY, "Include subfolders"); scanRecursive_->SetValue(true);
        scanActions->Add(scanRecursive_, 0, wxRIGHT | wxBOTTOM | wxALIGN_CENTER_VERTICAL, 10);
        Button(panel, scanActions, "Check folder", [this]() { ScanPronunciations(); });
        Button(panel, scanActions, "Add selected word...", [this]() { AddSelectedUnknown(); });
        Button(panel, scanActions, "Export report...", [this]() { ExportScan(); });
        outer->Add(scanActions, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
        unknownList_ = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 120), wxLC_REPORT | wxLC_SINGLE_SEL);
        unknownList_->InsertColumn(0, "Unknown word", wxLIST_FORMAT_LEFT, 270);
        unknownList_->InsertColumn(1, "Occurrences", wxLIST_FORMAT_LEFT, 120);
        outer->Add(unknownList_, 1, wxEXPAND | wxALL, 12);
        unknownList_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { AddSelectedUnknown(); });
        return panel;
    }
    void LoadConfig() {
        wxFileConfig config(kConfigName);
#ifndef __EMSCRIPTEN__
        lastAudioDir_ = config.Read("Preferences/LastAudioFolder", wxString{});
        lastBatchDir_ = config.Read("Preferences/LastBatchFolder", wxString{});
        lastOutputDir_ = config.Read("Preferences/LastOutputFolder", wxString{});
        wavPicker_->SetInitialDirectory(lastAudioDir_); batchInput_->SetInitialDirectory(lastBatchDir_);
        dialoguePicker_->SetInitialDirectory(lastBatchDir_); dialogueTlk_->SetInitialDirectory(lastBatchDir_);
        scanInput_->SetInitialDirectory(lastBatchDir_);
#endif
        long movement = 0; config.Read("Preferences/MouthMovement", &movement, 0L);
        movement_->SetSelection(movement == 1 ? 1 : 0); batchMovement_->SetSelection(movement_->GetSelection());
        fontScale_ = settings_.fontScale();
        // Keep the old reference until an explicit import succeeds. Do not open
        // this potentially large, stale, or remote file during application launch.
        legacyDictionary_ = config.Read("user_dict", wxString{});
        legacyImportButton_->Show(!legacyDictionary_.empty());
        for (const char* key : {
            "alignment_graph_version", "anticipate_closures_ms", "anticipate_fricatives_ms", "anticipate_stops_ms",
            "batch_anticipate_closures_ms", "batch_anticipate_fricatives_ms", "batch_anticipate_stops_ms", "batch_close_on_pause_ms",
            "batch_fail_fast", "batch_input_dir", "batch_jobs", "batch_lip_smoothing",
            "batch_output_dir", "batch_preserve_closures", "batch_recursive", "batch_resume",
            "batch_tail_rest_ms", "batch_use_ogi", "close_on_pause_ms", "create_output_lip",
            "create_transcript_file", "create_transcript_text", "create_wav", "dict_input_dir",
            "dict_output_dir", "dict_recursive", "dict_source_report", "dict_unknown_report",
            "lip_editor_open", "lip_editor_save", "lip_smoothing", "mode",
            "models_root", "preserve_closures", "tail_rest_ms", "timeline_audio",
            "timeline_lip", "timeline_phn", "timeline_save", "use_ogi"}) config.DeleteEntry(key, false);
        config.Flush();
    }
    void MigrateLegacyPronunciations() {
        if (running_ || legacyDictionary_.empty()) return;
        const fs::path path = ToPath(legacyDictionary_);
        const fs::path executable = ToPath(wxStandardPaths::Get().GetExecutablePath());
        const fs::path userData = UserDataPath();
        // Discovery, parsing and per-entry validation run in the existing job
        // worker. Failure/cancellation keeps both the old reference and file.
        StartJob([path, executable, userData](const neolip::Context& context) {
            cslu::ScopedWorkControl control(context.work);
            context.work.check();
            if (context.progress) context.progress({"Importing previous pronunciations", 0, 0});
            if (!fs::is_regular_file(path))
                throw std::runtime_error("The previous custom dictionary is missing. Its saved location has been retained: " + path.u8string());
            const auto assets = neolip::discoverAssets(executable, userData);
            neolip::requireAssets(assets);
            const auto same = [&](const fs::path& other) {
                std::error_code ec;
                return fs::equivalent(path, other, ec) && !ec;
            };
            if (!same(assets.userDictionary) && !same(assets.dictionaries / "kotor_override.dict") &&
                !same(assets.dictionaries / "custom_dictionary.txt")) {
                const auto entries = neolip::readUserPronunciations(path);
                if (!entries.empty()) neolip::importUserPronunciations(assets, entries, false);
            }
            UiMessage reply;
            reply.kind = UiMessage::Kind::LegacyImported;
            reply.userEntries = neolip::readUserPronunciations(assets.userDictionary);
            return reply;
        });
    }
    void SaveConfig() {
        wxFileConfig config(kConfigName);
#ifndef __EMSCRIPTEN__
        config.Write("Preferences/LastAudioFolder", lastAudioDir_);
        config.Write("Preferences/LastBatchFolder", lastBatchDir_);
        config.Write("Preferences/LastOutputFolder", lastOutputDir_);
#endif
        config.Write("Preferences/MouthMovement", static_cast<long>(movement_->GetSelection())); config.Flush();
        settings_.saveWindowPlacement(*this);
#ifdef __EMSCRIPTEN__
        EM_ASM({ if (window.neoToolsFlushSettings) window.neoToolsFlushSettings(); });
#endif
    }
    fs::path UserDataPath() const {
#ifdef __EMSCRIPTEN__
        return "/home/web_user/.config/neotools/neolip";
#else
        return ToPath(wxStandardPaths::Get().GetUserDataDir());
#endif
    }
    const neolip::Assets& Assets() {
        if (!assets_) assets_ = neolip::discoverAssets(ToPath(wxStandardPaths::Get().GetExecutablePath()), UserDataPath());
        neolip::requireAssets(*assets_);
        return *assets_;
    }
    std::shared_ptr<neolip::Engine> SingleEngine() {
        if (!singleEngine_) singleEngine_ = std::make_shared<neolip::Engine>(Assets());
        return singleEngine_;
    }
    void ClearTranscriptHighlights() {
        if (clearingHighlights_ || !literalText_) return;
        clearingHighlights_ = true;
        const auto palette = wxui::themePalette(wxui::currentDarkMode());
        literalText_->SetStyle(0, literalText_->GetLastPosition(), wxTextAttr(palette.text, palette.field));
        unknownWords_.clear(); clearingHighlights_ = false;
    }
    long TextPositionForByte(const std::string& text,std::size_t byte) const {
        const auto prefix=wxui::toWx(text.substr(0,byte));long line=0,column=0;
        for(auto ch:prefix){if(ch=='\n'){++line;column=0;}else if(ch!='\r')++column;}
        const long position=literalText_->XYToPosition(column,line);
        return position>=0?position:static_cast<long>(prefix.length());
    }
    void HighlightUnknownWords() {
        const auto palette=wxui::themePalette(wxui::currentDarkMode());
        literalText_->SetStyle(0,literalText_->GetLastPosition(),wxTextAttr(palette.text,palette.field));
        const auto text=wxui::toStd(literalText_->GetValue());
        try {
            for(const auto& token:cslu::tokenizeTranscript(text))
                if(std::find(unknownWords_.begin(),unknownWords_.end(),token.word)!=unknownWords_.end())
                    literalText_->SetStyle(TextPositionForByte(text,token.beginByte),TextPositionForByte(text,token.endByte),
                        wxTextAttr(palette.text,wxui::currentDarkMode()?wxColour(110,65,35):wxColour(255,226,160)));
        } catch(const std::exception& e){ShowProblem(e.what());}
    }
    void SaySelectedText() {
        if (running_) return;
        long begin = 0, end = 0; literalText_->GetSelection(&begin, &end);
        if (begin == end) {
            SetResult("Select the text to pronounce", "Select a number, name or phrase in Spoken text, then choose Say as.");
            literalText_->SetFocus(); return;
        }
        const auto selected = literalText_->GetRange(begin, end);
        wxTextEntryDialog dialog(this, "Replace the selected text with exactly what is spoken.\nFor example: one hundred, or aitch kay forty seven.\nOnly this visible transcript changes; the source file and your dictionary do not.\n\nSelected: " + selected,
            "Say selected text as", wxString{});
        if (dialog.ShowModal() != wxID_OK) return;
        auto replacement = dialog.GetValue(); replacement.Trim().Trim(false);
        if (replacement.empty()) return;
        literalText_->Replace(begin, end, replacement);
        literalText_->SetSelection(begin, begin + static_cast<long>(replacement.length())); literalText_->SetFocus();
        textDirty_ = true; CheckTranscript();
    }
    void AddSelectedTranscriptWord() {
        auto word = literalText_->GetStringSelection(); word.Trim().Trim(false);
        if (word.empty()) { SetResult("Select a word first", "Select one word in the transcript to save a reusable pronunciation."); return; }
        if (word.find_first_of(" \t\r\n") != wxString::npos) { SetResult("Choose one dictionary word", "For a phrase or a number, use Say as instead."); return; }
        EditPronunciation({wxui::toStd(word), {}, {}});
        if (!running_) CheckTranscript();
    }
    void CheckTranscript() {
        if (running_ || literalText_->IsEmpty()) return;
        try {
            const auto text = wxui::toStd(literalText_->GetValue()); auto engine = SingleEngine();
            StartJob([engine, text](const neolip::Context& context) {
                UiMessage message; message.kind = UiMessage::Kind::Lookup; message.path = "transcript-check";
                message.lookup = engine->pronounce(text, context); return message;
            });
        } catch (const std::exception& e) { ShowProblem(e.what()); }
    }
    int ChooseAudioChannel(const neolip::AudioBuffer& audio,const fs::path& source) {
        wxDialog dialog(this,wxID_ANY,"Choose voice channel",wxDefaultPosition,wxDefaultSize,wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER);
        auto* box=new wxBoxSizer(wxVERTICAL);
        box->Add(new wxStaticText(&dialog,wxID_ANY,"Listen and choose the channel containing the voice.\nThis choice does not trim the recording or change its time zero."),0,wxALL,FromDIP(12));
        auto* channels=new wxChoice(&dialog,wxID_ANY);
        for(std::size_t i=0;i<audio.channelRms.size();++i){
            const wxString name=audio.sourceChannels==2?(i==0?wxString("Left"):wxString("Right")):wxString::Format("Channel %zu",i+1);
            const auto level=audio.channelRms[i]>0?wxString::Format("%.1f dBFS RMS",20*std::log10(audio.channelRms[i])):wxString("silent");
            channels->Append(name+" - "+level);
        }
        if(!audio.channelRms.empty())channels->SetSelection(static_cast<int>(std::max_element(audio.channelRms.begin(),audio.channelRms.end())-audio.channelRms.begin()));
        box->Add(channels,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(12));
        neolip::AudioPlayback audition;
        auto* row=new wxWrapSizer(wxHORIZONTAL);
        const auto listen=[&,this](int channel){
            try { wxBusyCursor busy;audition.stop();audition.load(neolip::decodeAudio(source,channel));audition.play(); }
            catch(const std::exception& e){wxMessageBox(wxui::toWx(e.what()),"Channel preview",wxOK|wxICON_WARNING,&dialog);}
        };
        if(audio.sourceChannels==2){Button(&dialog,row,"Listen left",[&](){listen(0);});Button(&dialog,row,"Listen right",[&](){listen(1);});}
        else Button(&dialog,row,"Listen selected",[&](){listen(channels->GetSelection());});
        Button(&dialog,row,"Stop",[&](){audition.stop();});
        box->Add(row,0,wxLEFT|wxRIGHT,FromDIP(12));
        box->Add(dialog.CreateStdDialogButtonSizer(wxOK|wxCANCEL),0,wxEXPAND|wxALL,FromDIP(12));
        dialog.SetSizerAndFit(box);dialog.CentreOnParent();wxui::applyTheme(&dialog,wxui::currentDarkMode());
        const auto accepted=dialog.ShowModal();audition.stop();
        return accepted==wxID_OK?channels->GetSelection():-1;
    }
    void ShowReviewWarnings(const std::vector<neolip::ReviewWarning>& warnings) {
        reviewWarnings_ = warnings; reviewChoice_->Clear();
        for (const auto& warning : warnings) {
            auto text = wxui::toWx(warning.message); if (text.length() > 75) text = text.Left(72) + "...";
            reviewChoice_->Append(wxString::Format("%.2f-%.2fs: ", warning.startSeconds, warning.endSeconds) + text);
        }
        reviewPanel_->Show(!warnings.empty());
        if (!warnings.empty()) { reviewChoice_->SetSelection(0); reviewChoice_->SetToolTip(wxui::toWx(warnings.front().message)); }
        Layout();
    }
    void SetEditorWords(const std::vector<neolip::AlignedWord>& words,const std::string& text) {
        editorWords_=words;editorTranscript_=text;wordChoice_->Clear();
        for(const auto& word:words)wordChoice_->Append(wxui::toWx(word.text)+wxString::Format("  %.2f-%.2fs",word.startSeconds,word.endSeconds));
        wordChoice_->Enable(!words.empty());
        wordChoice_->SetToolTip(words.empty()?"Word timings are available for lines generated in this session; a LIP alone does not contain its transcript.":"Times come from the original audio alignment. Editing mouth keys does not change the word boundaries.");
        timelinePanel_->SetWords(words);
    }
    void SelectAlignedWord(std::size_t i) {
        if(i>=editorWords_.size()||running_)return;
        PausePlayback();const auto& word=editorWords_[i];wordChoice_->SetSelection(static_cast<int>(i));
        timelinePanel_->SetRange(word.startSeconds,word.endSeconds);timelinePanel_->SetCursor(word.startSeconds);SeekPlayback(word.startSeconds);
        timelineSelection_->SetLabel(wxui::toWx(word.text)+wxString::Format("  %.3f-%.3fs (source alignment)",word.startSeconds,word.endSeconds));
    }
    void ReplaySelection() {
        if(running_||!timelineAudio_)return;
        auto range=timelinePanel_->Selection();
        if(range.second<=range.first){range.first=timelinePanel_->CursorSeconds();range.second=range.first+1;}
        range.first=std::max(0.0,range.first-.15);range.second=std::min(TimelineWaveform().durationSeconds(),range.second+.15);
        PausePlayback();loopSelection_->SetValue(false);timelinePanel_->SetCursor(range.first);
        TogglePlayback();replayEnd_=range.second;
    }
    void CorrectAlignedWord() {
        const int index=wordChoice_->GetSelection();
        if(running_||index<0||static_cast<std::size_t>(index)>=editorWords_.size())return;
        const auto word=editorWords_[static_cast<std::size_t>(index)];const auto input=editorInput_;const auto output=editorPath_;const auto text=editorTranscript_;const int channel=editorChannel_;
        if(!ConfirmLipEditorChanges()||!AudioSelected(input))return;
        SetTranscript(text,"Original aligned transcript");createOutput_=output;createChannel_=channel;UpdateOutputLabel();notebook_->SetSelection(0);
        literalText_->SetSelection(TextPositionForByte(text,word.beginByte),TextPositionForByte(text,word.endByte));literalText_->SetFocus();
        SetResult("Correct selected word", "Use Say as for this line, or Add pronunciation for a reusable word. Create again to realign the full line; your saved LIP is not changed until then.");
    }
    void UpdateDocumentState() {
        if(documentState_)documentState_->SetLabel(editorPath_.empty()?wxString("No document"):
            (lipEditorDirty_?(presetOnlyChange_?wxString("Unsaved preset preview"):wxString("Unsaved edits")):wxString("Saved")));
    }
    void LoadPreparedResult(const neolip::Result& result, bool unsaved) {
        if(!result.previewLip||!result.audio)return;
        const bool sameDocument=editorPath_==result.output && !editorPath_.empty();
        const bool sameAudio=timelineAudio_==result.audio;
        const double cursor=sameAudio?timelinePanel_->CursorSeconds():0;
        const auto range=sameAudio?timelinePanel_->Selection():std::pair<double,double>{0,0};
        if(unsaved&&sameDocument)RememberLipEdit();
        else {undoLip_.clear();redoLip_.clear();}
        StopPlayback();if(!sameAudio)loopSelection_->SetValue(false);
        editorPath_=result.output;editorInput_=result.input;editorChannel_=result.audioChannel;
        timelineAudio_=result.audio;playback_.load(timelineAudio_);
        if(unsaved&&!sameDocument){
            try {double duration=0;auto entries=cslu::readKotorKeyframes(result.output,&duration);savedLip_={std::move(entries),duration};}
            catch(const std::exception&){savedLip_={};}
        }
        timelineLipDuration_=result.previewLip->durationSeconds;lipEditorEntries_.clear();
        for(const auto& key:result.previewLip->frames)lipEditorEntries_.push_back({key.timeSeconds,key.shape});
        timelineEntries_=lipEditorEntries_;
        if(!unsaved)savedLip_={lipEditorEntries_,timelineLipDuration_};
        lipEditorDirty_=!SameSnapshot(savedLip_);presetOnlyChange_=unsaved;
        timelineLipPicker_->SetPath(ToWx(result.output));timelineAudioPicker_->SetPath(ToWx(result.input));
        presetPreviewAvailable_=bool(result.prepared);previewMovement_->Enable(presetPreviewAvailable_);
        previewMovement_->SetSelection(movement_->GetSelection());displayedPreviewSelection_=movement_->GetSelection();
        SetEditorWords(result.words,result.transcript);
        RefreshLipGrid();RefreshTimelinePanel();notebook_->SetSelection(1);
        timelinePanel_->SetRange(range.first,range.second);timelinePanel_->SetCursor(cursor);playback_.seek(cursor);
    }
    bool OpenLatestResult() {
        if(running_||lastOutput_.empty())return false;
        // Review/navigation is not Revert. Keep live edits, saved state and Undo history.
        if(editorPath_==lastOutput_){notebook_->SetSelection(1);return true;}
        if(!ConfirmLipEditorChanges())return false;
        if(lastSingle_.output==lastOutput_ && lastSingle_.audio) {
            try {
                auto current=lastSingle_;
                current.previewLip=std::make_shared<const cslu::LipFile>(cslu::readLip(lastOutput_));
                LoadPreparedResult(current,false); // Disk curve, cached source audio; never cached old keys.
            } catch(const std::exception& e){ShowProblem(e.what());return false;}
        } else LoadLipEditorPath(ToWx(lastOutput_));
        return editorPath_==lastOutput_;
    }
    void PreviewPreset(neolip::MouthMovement movement) {
        if (running_) return;
        if (!lastCreate_ || !singleEngine_) { SetResult("Create a LIP first", "After creation, compare Natural and Minimal smoothing without repeating acoustic alignment."); return; }
        if (lipEditorDirty_ && !presetOnlyChange_ && !ConfirmLipEditorChanges()) {
            previewMovement_->SetSelection(displayedPreviewSelection_); return;
        }
        auto request = *lastCreate_; request.spokenText = wxui::toStd(literalText_->GetValue());
        request.audio = ToPath(wavPicker_->GetPath()); request.audioChannel = createChannel_; request.movement = movement;
        request.output = createOutput_; auto engine = singleEngine_;
        movement_->SetSelection(movement == neolip::MouthMovement::MinimalSmoothing ? 1 : 0);
        previewMovement_->SetSelection(movement_->GetSelection());
        StartJob([engine, request](const neolip::Context& context) { UiMessage message; message.kind = UiMessage::Kind::Preview;
            message.single = engine->preview(request, context); return message; });
    }
    void ReviewSection() {
        const int selected = reviewChoice_->GetSelection();
        if (selected < 0 || selected >= static_cast<int>(reviewWarnings_.size())) return;
        const auto warning = reviewWarnings_[static_cast<std::size_t>(selected)];
        if (!OpenLatestResult()) return;
        if (timelinePanel_) { timelinePanel_->SetRange(warning.startSeconds, warning.endSeconds); timelinePanel_->SetCursor(warning.startSeconds); timelinePanel_->ZoomSelection(); }
        resultBody_->SetLabel(wxui::toWx(warning.message) + "\nThis is a review flag, not an accuracy score.");
        resultBody_->Wrap(std::max(350, GetClientSize().x - FromDIP(50))); Layout();
    }
    bool ConfigureLoop() {
        try {
            if (!timelinePanel_) return false;
            const auto range = timelinePanel_->Selection();
            playback_.setLoop(loopSelection_->GetValue(), range.first, range.second);
            return true;
        } catch (const std::exception& e) {
            loopSelection_->SetValue(false); playback_.setLoop(false, 0, 0); ShowProblem(e.what()); return false;
        }
    }
    void TogglePlayback() {
        if (running_) return;
        replayEnd_=-1;
        if (playback_.playing()) { PausePlayback(); return; }
        try {
            if (!timelineAudio_) throw std::runtime_error("Load matching audio before playback.");
            if (!ConfigureLoop()) return;
            const auto requested = timelinePanel_->CursorSeconds();
            if (std::abs(requested - playback_.position()) > .5 / timelineAudio_->waveform.sampleRate)
                playback_.seek(requested);
            playback_.play();
            playbackTicking_ = true; playButton_->SetLabel("Pause"); playbackTimer_.Start(20);
        } catch (const std::exception& e) { ShowProblem(e.what()); }
    }
    void PausePlayback() {
        const bool updatePosition = playbackTicking_ || playback_.playing();
        playback_.pause(); playbackTimer_.Stop(); playbackTicking_ = false;
        if (updatePosition && timelinePanel_) { timelinePanel_->SetCursor(playback_.position(), true); UpdateTimelineStatus(); }
        if (playButton_) playButton_->SetLabel("Play");
    }
    void StopPlayback() {
        playbackTimer_.Stop(); playback_.stop(); playbackTicking_ = false;
        if (playButton_) playButton_->SetLabel("Play");
        if (timelinePanel_) timelinePanel_->SetCursor(0);
    }
    void SeekPlayback(double seconds) {
        try {
            // Clicking/dragging pauses first, so selecting a loop or editing a
            // key cannot race with a moving audio cursor.
            PausePlayback(); playback_.seek(seconds);
            if (timelinePanel_) { timelinePanel_->SetCursor(seconds, true); UpdateTimelineStatus(); }
        } catch (const std::exception& e) { ShowProblem(e.what()); }
    }
    void OnPlaybackTick() {
        if (!playbackTicking_ || !timelinePanel_) return;
        const auto deviceError=playback_.pollError();
        if(!deviceError.empty()){PausePlayback();ShowProblem(deviceError);return;}
        timelinePanel_->SetCursor(playback_.position(), true);
        UpdateTimelineStatus();
        if(replayEnd_>=0 && playback_.position()>=replayEnd_){replayEnd_=-1;PausePlayback();}
        if (!playback_.playing()) PausePlayback();
    }
    void OnKeyboard(wxKeyEvent& event) {
        if (running_ || !notebook_ || notebook_->GetSelection() != 1) { event.Skip(); return; }
        wxWindow* focus = wxWindow::FindFocus();
        if (wxDynamicCast(focus, wxTextCtrl) || (lipGrid_ && lipGrid_->IsCellEditControlEnabled())) { event.Skip(); return; }
        const auto key = event.GetKeyCode();
        if (event.CmdDown() && (key == 'S' || key == 's')) { SaveLipEditor(event.ShiftDown()); return; }
        if (event.CmdDown() && (key == 'Z' || key == 'z')) { RestoreLipEdit(event.ShiftDown()); return; }
        if (event.CmdDown() && (key == 'Y' || key == 'y')) { RestoreLipEdit(true); return; }
        if (key == WXK_SPACE) { TogglePlayback(); return; }
        if (key == WXK_ESCAPE) { StopPlayback(); return; }
        if (key == WXK_DELETE && focus == timelinePanel_) { wxCommandEvent command; OnDeleteLipKeys(command); return; }
        event.Skip();
    }

    void ShowProblem(const std::string& message) {
        SetResult("Could not complete the operation", wxui::toWx(message));
        details_->ChangeValue(wxui::toWx(message)); detailsPane_->Show(); Layout();
    }
    void SetResult(const wxString& title, const wxString& body = wxString{}) {
        resultPanel_->Show(); resultTitle_->SetLabel(title); resultBody_->SetLabel(body);
        resultBody_->Wrap(std::max(350, GetClientSize().GetWidth() - 50));
        for (auto* button : {openEditorButton_, showFolderButton_, newButton_, addUnknownButton_, retryButton_}) button->Hide();
        detailsPane_->Collapse(); detailsPane_->Hide();
        if (reviewPanel_) reviewPanel_->Hide();
        Layout();
    }
    bool ConfirmReplace(const fs::path& path) {
        std::error_code ec;
        if (!fs::exists(path, ec)) return true;
        return wxMessageBox("Replace the existing file?\n\n" + ToWx(path), "Replace LIP",
                            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) == wxYES;
    }
    bool ConfirmDiscardTranscript() {
        if (!textDirty_ || literalText_->IsEmpty()) return true;
        return wxMessageBox("Discard the edited transcript before loading another input?", "Unsaved transcript",
                            wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) == wxYES;
    }
    void SetTranscript(const std::string& text, const wxString& source) {
        suppressText_ = true; literalText_->ChangeValue(wxui::toWx(text)); suppressText_ = false; textDirty_ = false;
        transcriptLabel_->SetLabel(source.empty() ? wxString("Spoken text") : "Spoken text - " + source + " (editable)");
    }
    bool AudioSelected(const fs::path& path) {
        if (running_) { wavPicker_->SetPath(ToWx(lastAudio_)); return false; }
        if (path == lastAudio_) return true;
        std::error_code ec;
        if (path.empty() || !fs::is_regular_file(path, ec)) return false;
        if (!ConfirmDiscardTranscript()) { wavPicker_->SetPath(ToWx(lastAudio_)); return false; }
        PausePlayback(); createChannel_ = -1; lastSingle_ = {}; if (singleEngine_) singleEngine_->clearCache();
        lastAudio_ = path; wavPicker_->SetPath(ToWx(path)); createOutput_ = neolip::defaultLipPath(path);
        lastAudioDir_ = ToWx(path.parent_path()); SetTranscript({}, {}); UpdateOutputLabel();
        try {
            if (!path.empty() && fs::is_regular_file(path)) {
                const auto transcript = neolip::matchingTranscript(path);
                if (!transcript.empty()) SetTranscript(neolip::readTranscript(transcript), ToWx(transcript.filename()));
            }
        } catch (const std::exception& e) { ShowProblem(e.what()); }
        return true;
    }
    void ImportTranscript(const std::optional<fs::path>& supplied = std::nullopt) {
        if (running_ || !ConfirmDiscardTranscript()) return;
        const auto load = [this](std::optional<fs::path> path) {
            if (!path) return;
            try { SetTranscript(neolip::readTranscript(*path), ToWx(path->filename())); }
            catch (const std::exception& e) { ShowProblem(e.what()); }
        };
        if (supplied) load(supplied);
        else wxui::requestOpenFile(this, "Load transcript", "Text transcripts (*.txt;*.lab)|*.txt;*.lab|All files (*.*)|*.*", load);
    }
    void UpdateOutputLabel() {
        outputLabel_->SetLabel(createOutput_.empty() ? wxString("Output: same name and folder as the audio") : "Output: " + ToWx(createOutput_));
        outputLabel_->Wrap(std::max(350, GetClientSize().GetWidth() - 70)); Layout();
    }
    std::optional<fs::path> ChooseLipOutput(const std::string& title, const fs::path& suggested) {
        const std::string filename = suggested.empty() ? "dialogue.lip" : suggested.filename().u8string();
#ifdef __EMSCRIPTEN__
        return wxui::chooseSaveFile(this, title, "LIP files (*.lip)|*.lip", filename);
#else
        wxString directory = lastOutputDir_;
        if (directory.empty() && !suggested.empty()) directory = ToWx(suggested.parent_path());
        wxFileDialog dialog(this, wxui::toWx(title), directory, wxui::toWx(filename), "LIP files (*.lip)|*.lip", wxFD_SAVE);
        if (dialog.ShowModal() != wxID_OK) return std::nullopt;
        return ToPath(dialog.GetPath());
#endif
    }
    void ChooseCreateOutput() {
        if (running_) return;
        const auto selected = ChooseLipOutput("Save LIP somewhere else", createOutput_);
        if (!selected) return;
        createOutput_ = *selected; if (createOutput_.extension().empty()) createOutput_ += ".lip";
        lastOutputDir_ = ToWx(createOutput_.parent_path()); UpdateOutputLabel();
    }
    void ClearCreate() {
        if (running_ || !ConfirmDiscardTranscript()) return;
        lastAudio_.clear(); createOutput_.clear(); lastCreate_.reset(); unknownWords_.clear();
        PausePlayback(); createChannel_ = -1; lastSingle_ = {}; if (singleEngine_) singleEngine_->clearCache();
        wavPicker_->SetPath(wxString{}); SetTranscript({}, {}); UpdateOutputLabel(); notebook_->SetSelection(0);
        resultPanel_->Hide(); Layout();
    }
#ifndef __EMSCRIPTEN__
    bool HandleDrop(const wxArrayString& files) {
        if (running_ || files.empty()) return false;
        if (files.size() != 1) { ShowProblem("Drop one WAV, MP3, LIP, DLG, transcript or folder. Use Batch for multiple recordings."); return false; }
        const auto path = ToPath(files[0]);
        try {
            if (fs::is_directory(path)) {
                notebook_->SetSelection(2); batchInput_->SetPath(ToWx(path));
                if (DialogueMode()) dialogueTlk_->SetPath(wxString{});
                batchOutput_.clear(); lastBatchDir_ = ToWx(path); InvalidateBatch(); ScanBatch(); return true;
            }
            const auto ext = wxFileName(files[0]).GetExt().Lower();
            if (ext == "dlg") {
                notebook_->SetSelection(2); batchSource_->SetSelection(1); UpdateBatchMode();
                dialoguePicker_->SetPath(ToWx(path));
                if (batchInput_->GetPath().empty()) batchInput_->SetPath(ToWx(path.parent_path()));
                batchOutput_.clear(); InvalidateBatch(); ScanBatch(); return true;
            }
            if (ext == "wav" || ext == "mp3") { notebook_->SetSelection(0); return AudioSelected(path); }
            if (ext == "lip") { if (ConfirmLipEditorChanges()) LoadLipEditorPath(files[0]); return true; }
            if (ext == "txt" || ext == "lab") { notebook_->SetSelection(0); ImportTranscript(path); return true; }
            ShowProblem("Drop a WAV, LIP, DLG, .txt/.lab transcript, or a batch folder.");
        } catch (const std::exception& e) { ShowProblem(e.what()); }
        return false;
    }
#endif
    void Post(UiMessage message) {
        auto* event = new wxThreadEvent(NEOLIP_ENGINE_EVENT);
        event->SetPayload(std::make_shared<UiMessage>(std::move(message))); wxQueueEvent(this, event);
    }
    void StartJob(std::function<UiMessage(const neolip::Context&)> operation) {
        PausePlayback();
        if (running_) return;
#ifndef __EMSCRIPTEN__
        if (worker_.joinable()) worker_.join();
#endif
        cancel_.store(false); running_ = true; notebook_->Disable();
        SetResult("Working...", "You can cancel without leaving a partial LIP.");
        gauge_->Show(); gauge_->Pulse(); cancelButton_->Show(); cancelButton_->Enable(); Layout();
        const auto run = [this, operation = std::move(operation)]() {
            neolip::Context context; context.work.cancelled = &cancel_;
#ifdef __EMSCRIPTEN__
            auto lastYield = std::chrono::steady_clock::now();
            context.work.yield = [&lastYield]() {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastYield >= std::chrono::milliseconds(25)) {
                    lastYield = now; emscripten_sleep(1);
                }
            };
#endif
            context.progress = [this](const neolip::Progress& progress) {
                UiMessage message; message.kind = UiMessage::Kind::Progress; message.progress = progress; Post(std::move(message));
            };
            try { Post(operation(context)); }
            catch (const cslu::OperationCancelled&) { UiMessage message; message.error = "Cancelled. No partial output was saved."; Post(std::move(message)); }
            catch (const std::exception& e) { UiMessage message; message.error = e.what(); Post(std::move(message)); }
            catch (...) { UiMessage message; message.error = "Unexpected engine error. No unfinished LIP was saved."; Post(std::move(message)); }
        };
#ifdef __EMSCRIPTEN__
        CallAfter(run);
#else
        try { worker_ = std::thread(run); }
        catch (const std::exception& e) { running_ = false; notebook_->Enable(); gauge_->Hide(); cancelButton_->Hide(); ShowProblem(e.what()); }
#endif
    }
    void RunCreate() {
        if (running_) return;
        try {
            const auto path = ToPath(wavPicker_->GetPath());
            if (path.empty() || !fs::is_regular_file(path)) throw std::runtime_error("Choose an existing WAV file.");
            if (path != lastAudio_ && !AudioSelected(path)) return;
            const auto assets = Assets();
            if (createOutput_.empty()) createOutput_ = neolip::defaultLipPath(path);
            const bool existing = fs::exists(createOutput_);
            if (existing && !ConfirmReplace(createOutput_)) return;
            neolip::CreateRequest request{path, wxui::toStd(literalText_->GetValue()), createOutput_, SelectedMovement(movement_), existing};
            if (request.spokenText.empty()) throw std::runtime_error("Enter the words spoken in the recording.");
            request.audioChannel = createChannel_;
            lastCreate_ = request; lastWasBatch_ = false; unknownWords_.clear();
            if (!singleEngine_) singleEngine_ = std::make_shared<neolip::Engine>(assets);
            auto engine = singleEngine_;
            StartJob([engine, request](const neolip::Context& context) {
                UiMessage message; message.kind = UiMessage::Kind::Single;
                message.single = engine->create(request, context); return message;
            });
        } catch (const std::exception& e) { ShowProblem(e.what()); }
    }
    bool DialogueMode() const { return batchSource_ && batchSource_->GetSelection() == 1; }
    fs::path BatchOutputPath() const {
        if (!batchOutput_.empty()) return batchOutput_;
        return DialogueMode() ? neolip::defaultDialogueOutput(ToPath(dialoguePicker_->GetPath())) : ToPath(batchInput_->GetPath());
    }
    void UpdateBatchMode() {
        const bool dlg = DialogueMode(); dialogueOptions_->Show(dlg); recursive_->Show(!dlg);
        dialogueReview_->Show(dlg);
        wxListItem heading; heading.SetText(dlg ? "Dialogue line / LIP" : "Audio"); batchList_->SetColumn(0, heading);
        batchScan_->SetLabel(dlg ? "Scan dialogue" : "Scan folder");
        batchInputLabel_->SetLabel(dlg ? "Game directory / search folder (all subfolders are searched automatically)" : "Audio/transcript folder");
        batchIntro_->SetLabel(dlg ? "Choose a DLG and a game directory or any folder. Sound and VO_ResRef recordings are resolved automatically; review only unresolved lines." : "Match each WAV or MP3 with a same-name .txt or .lab transcript.");
        Layout();
    }
    void InvalidateBatch() {
        batchPlan_ = {}; dialoguePlan_.reset(); batchRowResults_.clear(); visibleBatchRows_.clear();
        batchRun_->Enable(false); batchList_->DeleteAllItems();
        batchFilter_->Enable(false); batchFilter_->SetSelection(0); dialogueReview_->Enable(false);
        batchSummary_->SetLabel(DialogueMode() ? "Choose a DLG and a game directory or search folder, then scan." : "Scan the folder before creating LIPs.");
        if (DialogueMode()) batchOutputLabel_->SetLabel("Output: " + ToWx(BatchOutputPath()));
        else batchOutputLabel_->SetLabel(batchOutput_.empty() ? wxString("Output: beside each audio file") : "Output: " + ToWx(batchOutput_));
        Layout();
    }
    void ShowDialogueRows() {
        if (!dialoguePlan_) return;
        dialogueReview_->Enable(true);
        batchList_->Freeze(); batchList_->DeleteAllItems(); visibleBatchRows_.clear(); batchFilter_->Enable(false);
        for (const auto& item : dialoguePlan_->rows) {
            std::string label = item.line.label();
            if (!item.line.speaker.empty()) label += " / " + item.line.speaker;
            if (!item.output.empty()) label += " / " + item.output.filename().u8string();
            const long row = batchList_->InsertItem(batchList_->GetItemCount(), wxui::toWx(label));
            batchList_->SetItem(row, 1, wxui::toWx(neolip::dialogueStateName(item.state) + ": " + item.detail));
            batchList_->SetItem(row, 2, wxui::toWx(item.spokenText));
            if (!item.audio.empty()) batchList_->SetItem(row, 3, ToWx(item.audio.lexically_relative(batchPlan_.inputRoot)));
        }
        batchList_->Thaw();
    }
    void ReviewDialogueRow(long index) {
        if (running_ || !dialoguePlan_ || !batchRowResults_.empty() || index < 0 || static_cast<std::size_t>(index) >= dialoguePlan_->rows.size()) return;
        auto& source = dialoguePlan_->rows[static_cast<std::size_t>(index)];
        wxDialog dialog(this, wxID_ANY, "Review " + wxui::toWx(source.line.label()), wxDefaultPosition, FromDIP(wxSize(760, 520)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* box = new wxBoxSizer(wxVERTICAL); dialog.SetSizer(box);
        auto* provenance = new wxStaticText(&dialog, wxID_ANY, "LIP: " + ToWx(source.output.filename()) +
            "\nSound: " + wxui::toWx(source.line.soundResRef) + "   VO_ResRef: " + wxui::toWx(source.line.voiceResRef) +
            "\n" + wxui::toWx(source.detail) + "\nChanges apply to this batch only; the DLG and TLK are never modified.");
        provenance->Wrap(FromDIP(700)); box->Add(provenance, 0, wxEXPAND | wxALL, FromDIP(12));
        auto* include = new wxCheckBox(&dialog, wxID_ANY, "Include this line when its text and audio are ready"); include->SetValue(!source.excluded);
        box->Add(include, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        box->Add(new wxStaticText(&dialog, wxID_ANY, "Recording (resolved automatically; change only to correct a match)"), 0, wxLEFT | wxRIGHT, FromDIP(12));
        auto* audio = new wxFilePickerCtrl(&dialog, wxID_ANY, ToWx(source.audio), "Choose recorded voice", "Audio|*.wav;*.WAV;*.mp3;*.MP3|All files|*", wxDefaultPosition, wxDefaultSize, wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
        audio->SetInitialDirectory(ToWx(batchPlan_.inputRoot)); box->Add(audio, 0, wxEXPAND | wxALL, FromDIP(12));
        if (source.audioCandidates.size() > 1) {
            auto* candidates = new wxChoice(&dialog, wxID_ANY);
            for (const auto& candidate : source.audioCandidates) candidates->Append(ToWx(candidate.lexically_relative(batchPlan_.inputRoot)));
            candidates->Bind(wxEVT_CHOICE, [audio, candidates, choices = source.audioCandidates](wxCommandEvent&) {
                const int selected = candidates->GetSelection(); if (selected >= 0) audio->SetPath(ToWx(choices[static_cast<std::size_t>(selected)]));
            });
            box->Add(candidates, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        }
        box->Add(new wxStaticText(&dialog, wxID_ANY, "Exact spoken words (replace game tokens; English recording required)"), 0, wxLEFT | wxRIGHT, FromDIP(12));
        auto* text = new wxTextCtrl(&dialog, wxID_ANY, wxui::toWx(source.spokenText), wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE);
        box->Add(text, 1, wxEXPAND | wxALL, FromDIP(12));
        box->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(12));
        dialog.CentreOnParent(); wxui::applyTheme(&dialog, wxui::currentDarkMode());
        if (dialog.ShowModal() != wxID_OK) return;
        try {
            const auto selectedAudio = ToPath(audio->GetPath());
            if (selectedAudio != source.audio) source.audioResolution = "User-selected recording";
            source.audio = selectedAudio; source.spokenText = wxui::toStd(text->GetValue());
            source.excluded = !include->GetValue(); source.textConfirmed = true;
            neolip::rebuildDialogueBatch(*dialoguePlan_); batchPlan_ = dialoguePlan_->batch;
            ShowDialogueRows(); UpdateBatchSummary();
        } catch (const std::exception& error) {
            batchRun_->Enable(false); ShowProblem(error.what());
        }
    }
    void ChooseBatchOutput() {
        const auto path = wxui::chooseDirectory(this, "Choose output folder");
        if (!path) return;
        batchOutput_ = *path; lastOutputDir_ = ToWx(*path); InvalidateBatch(); ScanBatch();
    }
    bool BatchVisible(const neolip::Result& item) const {
        switch(batchFilter_?batchFilter_->GetSelection():0){
        case 1:return item.status==neolip::Status::UnknownWords;
        case 2:return item.status==neolip::Status::ChannelChoice;
        case 3:return (item.status==neolip::Status::Created||item.status==neolip::Status::Skipped)&&!item.warnings.empty();
        case 4:return item.status==neolip::Status::Created;
        case 5:return item.status==neolip::Status::Skipped;
        case 6:return item.status==neolip::Status::Failed||item.status==neolip::Status::Cancelled;
        default:return true;
        }
    }
    void AppendBatchRow(std::size_t index) {
        batchFilter_->Enable(true);
        const auto& item=batchRowResults_[index];if(!BatchVisible(item))return;
        visibleBatchRows_.push_back(index);const auto relative=item.input.lexically_relative(batchPlan_.inputRoot);
        const long row=batchList_->InsertItem(batchList_->GetItemCount(),ToWx(relative.empty()?item.input.filename():relative));
        batchList_->SetItem(row,1,wxui::toWx(item.sourceLabel.empty() ? item.message : item.sourceLabel + ": " + item.message));
        batchList_->SetItem(row,2,wxui::toWx(item.transcript));
        batchList_->SetItem(row,3,ToWx(relative.empty()?item.input:relative));
    }
    void RefreshBatchRows() {
        batchList_->Freeze();batchList_->DeleteAllItems();visibleBatchRows_.clear();
        for(std::size_t i=0;i<batchRowResults_.size();++i)AppendBatchRow(i);
        batchList_->Thaw();
    }
    std::optional<std::size_t> BatchIndex(long row) const {
        if(row<0||static_cast<std::size_t>(row)>=visibleBatchRows_.size())return {};
        return visibleBatchRows_[static_cast<std::size_t>(row)];
    }
    void NextBatchProblem(int direction) {
        const long count=batchList_->GetItemCount();if(count==0)return;
        const long selected=batchList_->GetNextItem(-1,wxLIST_NEXT_ALL,wxLIST_STATE_SELECTED);
        for(long step=1;step<=count;++step){
            const long start=selected>=0?selected:(direction>0?-1:0);
            const long row=(start+direction*step+count*2)%count;
            if (dialoguePlan_ && batchRowResults_.empty()) {
                const auto state = dialoguePlan_->rows[static_cast<std::size_t>(row)].state;
                if (state == neolip::DialogueState::Ready || state == neolip::DialogueState::Duplicate ||
                    state == neolip::DialogueState::NoVoice || state == neolip::DialogueState::Excluded) continue;
            } else {
                const auto index=BatchIndex(row);if(!index)continue;
                const auto& item=batchRowResults_[*index];
                if((item.status==neolip::Status::Created||item.status==neolip::Status::Skipped)&&item.warnings.empty())continue;
            }
            if(selected>=0)batchList_->SetItemState(selected,0,wxLIST_STATE_SELECTED);
            batchList_->SetItemState(row,wxLIST_STATE_SELECTED|wxLIST_STATE_FOCUSED,wxLIST_STATE_SELECTED|wxLIST_STATE_FOCUSED);
            batchList_->EnsureVisible(row);batchList_->SetFocus();return;
        }
    }
    void OpenBatchRow(long row) {
        if (dialoguePlan_ && batchRowResults_.empty()) { ReviewDialogueRow(row); return; }
        const auto index=BatchIndex(row);if(running_||!index)return;
        const auto item=batchRowResults_[*index];
        if(item.status==neolip::Status::Created||item.status==neolip::Status::Skipped){
            if(editorPath_!=item.output&&!ConfirmLipEditorChanges())return;
            lastOutput_=item.output;
            if(editorPath_!=item.output){pendingAudioReview_=item;LoadLipEditorPath(ToWx(item.output),item.input,item.audioChannel,item.words,item.transcript);}
            else notebook_->SetSelection(1);
            if(!running_){pendingAudioReview_.reset();SetResult(wxui::toWx(item.message));ShowReviewWarnings(item.warnings);}
        } else if(item.status==neolip::Status::ChannelChoice)ChooseBatchChannel(row);
        else {
            if (!item.sourceLabel.empty() && item.input == lastAudio_ && !ConfirmDiscardTranscript()) return;
            if (!AudioSelected(item.input)) return;
            if (!item.sourceLabel.empty()) SetTranscript(item.transcript, wxui::toWx(item.sourceLabel + " (DLG)"));
            createOutput_=item.output;createChannel_=item.audioChannel;UpdateOutputLabel();notebook_->SetSelection(0);SetResult("Review this input",wxui::toWx(item.message));
        }
    }
    void ChooseBatchChannel(long row=-1) {
        if(running_)return;
        if(row<0)row=batchList_->GetNextItem(-1,wxLIST_NEXT_ALL,wxLIST_STATE_SELECTED);
        const auto index=BatchIndex(row);if(!index){SetResult("Select a batch result first");return;}
        const auto& selected=batchRowResults_[*index];
        if(selected.status==neolip::Status::Created||selected.status==neolip::Status::Skipped){
            SetResult("This LIP is already complete","To use a different voice channel, open its audio in Create and explicitly regenerate the line.");return;
        }
        const auto path=selected.input;
        StartJob([path](const neolip::Context& context){cslu::ScopedWorkControl control(context.work);UiMessage message;
            message.kind=UiMessage::Kind::BatchAudio;message.path=path;message.audio=neolip::decodeAudio(path);return message;});
    }
    void ScanBatch() {
        if (running_ || batchInput_->GetPath().empty()) return;
        const auto input = ToPath(batchInput_->GetPath()); const auto output = BatchOutputPath();
        const bool recursive = recursive_->GetValue(); scannedRecursive_ = recursive; lastBatchDir_ = ToWx(input);
        scannedDialogue_ = dialoguePicker_->GetPath(); scannedTalkTable_ = dialogueTlk_->GetPath();
        if (DialogueMode()) {
            if (scannedDialogue_.empty()) { SetResult("Choose a DLG file first"); return; }
            const neolip::DialogueRequest request{ToPath(scannedDialogue_), input, ToPath(scannedTalkTable_), output};
            StartJob([request](const neolip::Context& context) {
                UiMessage message; message.kind = UiMessage::Kind::Plan;
                message.dialogue = std::make_shared<neolip::DialoguePlan>(neolip::planDialogue(request, context));
                message.plan = message.dialogue->batch; return message;
            });
        } else StartJob([input, output, recursive](const neolip::Context& context) {
            UiMessage message; message.kind = UiMessage::Kind::Plan;
            message.plan = neolip::planBatch(input, output, recursive, context); return message;
        });
    }
    void UpdateBatchSummary() {
        const auto count = batchPlan_.pairs.size(); std::size_t existing = 0;
        for (const auto& pair : batchPlan_.pairs) { std::error_code ec; if (fs::exists(pair.output, ec)) ++existing; }
        wxString text;
        if (dialoguePlan_) {
            std::size_t skipped = 0, needsAttention = 0, shared = 0;
            for (const auto& row : dialoguePlan_->rows) {
                if (row.state == neolip::DialogueState::NoVoice || row.state == neolip::DialogueState::Excluded) ++skipped;
                else if (row.state == neolip::DialogueState::Duplicate) ++shared;
                else if (row.state != neolip::DialogueState::Ready) ++needsAttention;
            }
            text = wxString::Format("%zu unique LIPs ready; %zu shared nodes; %zu unvoiced/excluded; %zu need review; %zu existing LIPs.", count, shared, skipped, needsAttention, existing);
            text += wxString::Format("\n%zu recordings indexed under the selected folder.", dialoguePlan_->recordingsIndexed);
            text += dialoguePlan_->request.talkTable.empty() ? wxString("\nNo TLK selected/found; embedded text only.") : "\nTLK: " + ToWx(dialoguePlan_->request.talkTable);
            batchOutputLabel_->SetLabel("Output: " + ToWx(batchPlan_.outputRoot));
        } else {
            text = wxString::Format("%zu matching audio/transcript pairs; %zu audio files without transcripts; %zu existing LIPs.", count, batchPlan_.missingTranscripts.size(), existing);
            if (!batchPlan_.problems.empty()) text += wxString::Format(" %zu conflicting inputs are excluded; unaffected pairs can be processed.",batchPlan_.conflicts.size());
        }
        batchSummary_->SetLabel(text); batchSummary_->Wrap(std::max(350, GetClientSize().GetWidth() - 70));
        batchRun_->SetLabel(wxString::Format("Create %zu LIP files", count));
        batchRun_->Enable(count > 0 && !running_); Layout();
    }
    void RunBatch(bool retryOnly) {
        if (running_) return;
        try {
            auto plan = batchPlan_;
            const auto currentInput = fs::weakly_canonical(fs::absolute(ToPath(batchInput_->GetPath())));
            const auto currentOutput = fs::weakly_canonical(fs::absolute(BatchOutputPath()));
            if (currentInput != plan.inputRoot || currentOutput != plan.outputRoot ||
                recursive_->GetValue() != scannedRecursive_ ||
                (DialogueMode() && (dialoguePicker_->GetPath() != scannedDialogue_ || dialogueTlk_->GetPath() != scannedTalkTable_))) {
                InvalidateBatch(); ScanBatch(); return;
            }
            if (dialoguePlan_) {
                const auto unresolved = std::count_if(dialoguePlan_->rows.begin(), dialoguePlan_->rows.end(), [](const auto& row) {
                    return row.state != neolip::DialogueState::Ready && row.state != neolip::DialogueState::Duplicate && row.state != neolip::DialogueState::NoVoice && row.state != neolip::DialogueState::Excluded;
                });
                if (unresolved && wxMessageBox(wxString::Format("Create %zu ready LIPs?\n%zu unresolved dialogue nodes will not be processed. You can review them in the scan first.", plan.pairs.size(), static_cast<std::size_t>(unresolved)), "Process ready dialogue lines", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) return;
            }
            if(!dialoguePlan_ && !plan.problems.empty() && wxMessageBox(wxString::Format("Process %zu unaffected pairs?\nConflicting or unsafe inputs listed in the scan remain excluded.",plan.pairs.size()),
                "Process unaffected pairs",wxYES_NO|wxNO_DEFAULT|wxICON_WARNING,this)!=wxYES)return;
            if (plan.pairs.empty()) throw std::runtime_error("Scan the source and resolve at least one recording/text pair first.");
            bool replace = existingPolicy_->GetSelection() == 1;
            if (retryOnly) {
                std::set<fs::path> failed;
                for (const auto& item : lastBatch_.items)
                    if (item.status != neolip::Status::Created && item.status != neolip::Status::Skipped) failed.insert(item.output);
                plan.pairs.erase(std::remove_if(plan.pairs.begin(), plan.pairs.end(), [&](const auto& pair) { return !failed.count(pair.output); }), plan.pairs.end());
            }
            if (plan.pairs.empty()) throw std::runtime_error("There are no failed pairs to retry.");
            if (replace) {
                const auto count = std::count_if(plan.pairs.begin(), plan.pairs.end(), [](const auto& pair) { return fs::exists(pair.output); });
                if (count && wxMessageBox(wxString::Format("Replace %zu existing LIP files?", static_cast<std::size_t>(count)),
                    "Replace existing LIPs", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) return;
            }
            const auto assets = Assets(); const auto movement = SelectedMovement(batchMovement_);
            lastWasBatch_ = true; batchRetry_ = retryOnly;
            if (!retryOnly) lastBatch_ = {};
            unknownWords_.clear(); batchRowResults_.clear(); visibleBatchRows_.clear(); batchList_->DeleteAllItems();
            dialogueReview_->Enable(false);
            StartJob([this, assets, plan, movement, replace](const neolip::Context& context) {
                UiMessage message; message.kind = UiMessage::Kind::Batch;
                neolip::Engine engine(assets);
                message.batch = engine.batch(plan, movement, replace, context, [this](const neolip::Result& item) {
                    UiMessage update; update.kind = UiMessage::Kind::Item; update.single = item; Post(std::move(update));
                }); return message;
            });
        } catch (const std::exception& e) { ShowProblem(e.what()); }
    }
    void OnEngineMessage(wxThreadEvent& event) {
        auto message = event.GetPayload<std::shared_ptr<UiMessage>>();
        if (message->kind == UiMessage::Kind::Progress) {
            if (!cancel_.load()) resultTitle_->SetLabel(wxui::toWx(message->progress.message));
            if (message->progress.total) {
                const auto value = static_cast<int>(1000.0 * static_cast<double>(message->progress.completed) /
                    static_cast<double>(message->progress.total));
                gauge_->SetValue(std::clamp(value, 0, 1000));
                resultBody_->SetLabel(wxString::Format("%zu of %zu", message->progress.completed, message->progress.total));
            } else { gauge_->Pulse(); resultBody_->SetLabel("You can cancel safely."); }
            return;
        }
        if(message->kind==UiMessage::Kind::Item){batchRowResults_.push_back(message->single);AppendBatchRow(batchRowResults_.size()-1);return;}
#ifndef __EMSCRIPTEN__
        if (worker_.joinable()) worker_.join();
#endif
        running_ = false; notebook_->Enable(); gauge_->Hide(); cancelButton_->Hide();
        if (closingAfterJob_) { CallAfter([this]() { Close(); }); return; }
        switch (message->kind) {
        case UiMessage::Kind::Preview:
            if (message->single.status == neolip::Status::Previewed) {
                lastSingle_ = message->single; LoadPreparedResult(lastSingle_, true);
                SetResult("Mouth preset preview", "No file has been changed. Play this version, switch presets, then Save selected result.");
                ShowReviewWarnings(lastSingle_.warnings);
            } else {
                previewMovement_->SetSelection(displayedPreviewSelection_); movement_->SetSelection(displayedPreviewSelection_);
                if (message->single.status == neolip::Status::Cancelled) SetResult("Cancelled", "The saved file has not changed.");
                else ShowProblem(message->single.message);
            }
            break;
        case UiMessage::Kind::Single: {
            const auto& result = message->single;
            if (result.status == neolip::Status::Created) {
                lastOutput_ = result.output; textDirty_ = false; lastSingle_ = result;
                if(editorPath_==result.output)LoadPreparedResult(result,false);
                SetResult(wxui::toWx(result.message), wxString::Format("Duration: %.2f seconds   Mouth keys: %zu", result.durationSeconds, result.mouthKeys));
                openEditorButton_->Show(); showFolderButton_->Show(); newButton_->Show();
                ShowReviewWarnings(result.warnings);
            } else if (result.status == neolip::Status::UnknownWords) {
                unknownWords_ = result.unknownWords;
                SetResult("Some words need pronunciations", UnknownListText() + "\nSelect a word or number in the transcript and use Say as, or add a reusable pronunciation.");
                HighlightUnknownWords();
                addUnknownButton_->Show(); retryButton_->Show();
            } else if (result.status == neolip::Status::ChannelChoice && result.audio) {
                const int channel = ChooseAudioChannel(*result.audio,result.input);
                if (channel >= 0) { createChannel_ = channel; CallAfter([this]() { RunCreate(); }); }
                else SetResult("No output written", "Select an audio channel before creating this LIP.");
            } else if (result.status == neolip::Status::Cancelled) SetResult("Cancelled", wxui::toWx(result.message));
            else { ShowProblem(result.message); retryButton_->Show(); }
            break;
        }
        case UiMessage::Kind::Plan:
            batchPlan_ = std::move(message->plan); batchRowResults_.clear();visibleBatchRows_.clear(); batchList_->DeleteAllItems();
            batchFilter_->SetSelection(0);batchFilter_->Enable(false);
            dialoguePlan_.reset();
            if (message->dialogue) {
                dialoguePlan_ = std::move(*message->dialogue); ShowDialogueRows();
                resultPanel_->Hide(); UpdateBatchSummary(); break;
            }
            for (const auto& pair : batchPlan_.pairs) {
                const long row = batchList_->InsertItem(batchList_->GetItemCount(), ToWx(pair.audio.lexically_relative(batchPlan_.inputRoot)));
                batchList_->SetItem(row, 1, "Transcript found: " + ToWx(pair.transcript.filename()));
            }
            for (const auto& pair : batchPlan_.conflicts) {
                const long row=batchList_->InsertItem(batchList_->GetItemCount(),ToWx(pair.audio.lexically_relative(batchPlan_.inputRoot)));
                batchList_->SetItem(row,1,"Excluded: conflicting destination " + ToWx(pair.output.lexically_relative(batchPlan_.outputRoot)));
            }
            for (const auto& path : batchPlan_.missingTranscripts) {
                const long row = batchList_->InsertItem(batchList_->GetItemCount(), ToWx(path.lexically_relative(batchPlan_.inputRoot)));
                batchList_->SetItem(row, 1, "No .txt or .lab transcript - will not be processed");
            }
            for (const auto& problem : batchPlan_.problems) {
                const long row = batchList_->InsertItem(batchList_->GetItemCount(), "Folder conflict");
                batchList_->SetItem(row, 1, wxui::toWx(problem));
            }
            resultPanel_->Hide(); UpdateBatchSummary(); break;
        case UiMessage::Kind::Batch: {
            if (!batchRetry_) lastBatch_ = std::move(message->batch);
            else {
                // Retain already completed items so retry summaries and browser
                // ZIP exports still include the entire original batch.
                for (auto& updated : message->batch.items) {
                    auto old = std::find_if(lastBatch_.items.begin(), lastBatch_.items.end(),
                        [&](const auto& item) { return item.output == updated.output; });
                    if (old == lastBatch_.items.end()) lastBatch_.items.push_back(std::move(updated));
                    else *old = std::move(updated);
                }
                lastBatch_.created = lastBatch_.skipped = lastBatch_.failed = 0; lastBatch_.cancelled = false;
                for (const auto& item : lastBatch_.items) {
                    if (item.status == neolip::Status::Created) ++lastBatch_.created;
                    else if (item.status == neolip::Status::Skipped) ++lastBatch_.skipped;
                    else if (item.status == neolip::Status::Cancelled) lastBatch_.cancelled = true;
                    else ++lastBatch_.failed;
                }
            }
            unknownWords_.clear();
            std::set<std::string> unknown; std::string details;
            for (const auto& item : lastBatch_.items) {
                unknown.insert(item.unknownWords.begin(), item.unknownWords.end());
                if (item.status == neolip::Status::Failed) details += item.input.u8string() + ": " + item.message + "\n";
            }
            unknownWords_.assign(unknown.begin(), unknown.end());
            SetResult(lastBatch_.cancelled ? "Batch cancelled" : "Batch finished",
                wxString::Format("%zu created, %zu kept, %zu failed. Only .lip files were saved.",
                    lastBatch_.created, lastBatch_.skipped, lastBatch_.failed));
            lastOutput_ = batchPlan_.outputRoot;
            showFolderButton_->Show(lastBatch_.created > 0 || lastBatch_.skipped > 0);
            retryButton_->Show(lastBatch_.failed > 0 || lastBatch_.cancelled);
            addUnknownButton_->Show(!unknownWords_.empty());
            if (!unknownWords_.empty()) resultBody_->SetLabel(resultBody_->GetLabel() + "\n" + UnknownListText());
            const auto reviewCount = std::count_if(lastBatch_.items.begin(), lastBatch_.items.end(), [](const auto& item) { return !item.warnings.empty(); });
            if (reviewCount) resultBody_->SetLabel(resultBody_->GetLabel() + wxString::Format("\n%zu results have review flags. Activate a result row to inspect it.", static_cast<std::size_t>(reviewCount)));
            if (!details.empty()) { details_->ChangeValue(wxui::toWx(details)); detailsPane_->Show(); }
            batchRowResults_=lastBatch_.items;RefreshBatchRows();
            UpdateBatchSummary(); break;
        }
        case UiMessage::Kind::Scan:
            scanResult_ = std::move(message->scan); RefreshUnknownList();
            SetResult("Pronunciation check finished", wxString::Format("%zu transcript files checked; %zu unknown words.", scanResult_.files, scanResult_.words.size()));
            if (!scanResult_.errors.empty()) {
                wxString detail;
                for (const auto& error : scanResult_.errors) detail += wxui::toWx(error) + "\n";
                details_->ChangeValue(detail); detailsPane_->Show();
            }
            break;
        case UiMessage::Kind::Lookup: {
            if (message->path == fs::path("transcript-check")) {
                unknownWords_ = message->lookup.unknownWords; HighlightUnknownWords();
                SetResult(unknownWords_.empty() ? "Pronunciations ready" : "Check highlighted words",
                    unknownWords_.empty() ? wxString("All words have pronunciations. This checks the text, not the audio alignment.") : UnknownListText());
                addUnknownButton_->Show(!unknownWords_.empty()); break;
            }
            wxString text;
            for (const auto& word : message->lookup.wordInfo)
                text += wxui::toWx(word.word) + ": " + (word.pronunciation.empty() ? wxString("Unknown") : wxui::toWx(word.pronunciation)) + "\n";
            SetResult("Pronunciation", text); break;
        }
        case UiMessage::Kind::BatchAudio: {
            if(!message->audio)break;
            const int channel=ChooseAudioChannel(*message->audio,message->path);
            if(channel>=0){
                // Several dialogue outputs can share one recording. Change only
                // unsuccessful jobs, never metadata for an already saved LIP.
                std::set<fs::path> pending;
                for (const auto& item : batchRowResults_)
                    if (item.input == message->path && item.status != neolip::Status::Created && item.status != neolip::Status::Skipped) pending.insert(item.output);
                for (auto& pair : batchPlan_.pairs) if (pending.count(pair.output)) pair.audioChannel = channel;
                const auto update=[&](std::vector<neolip::Result>& items){for(auto& item:items)if(pending.count(item.output)){item.audioChannel=channel;
                    if(item.status==neolip::Status::ChannelChoice)item.message="Voice channel "+std::to_string(channel+1)+" selected; ready to retry";}};
                update(batchRowResults_);update(lastBatch_.items);RefreshBatchRows();
                SetResult("Voice channel selected","The choice is kept for this batch. Retry unsuccessful pairs to create the LIP.");retryButton_->Show();
            }
            break;
        }
        case UiMessage::Kind::Audio:
            if (message->audio && message->audio->channelChoiceRequired) {
                const int channel = ChooseAudioChannel(*message->audio,message->path);
                if (channel >= 0) {
                    const auto path = message->path;
                    StartJob([path, channel](const neolip::Context& context) { cslu::ScopedWorkControl control(context.work);
                        UiMessage reply; reply.kind = UiMessage::Kind::Audio; reply.path = path;
                        reply.audio = neolip::decodeAudio(path, channel); return reply; });
                }
            } else {
                editorInput_=message->path;editorChannel_=message->audio?message->audio->selectedChannel:-1;
                timelineAudio_ = std::move(message->audio); playback_.load(timelineAudio_); RefreshTimelinePanel(); resultPanel_->Hide();
                if (pendingAudioReview_) { const auto item = *pendingAudioReview_; pendingAudioReview_.reset(); SetResult(wxui::toWx(item.message)); ShowReviewWarnings(item.warnings); }
            }
            break;
        case UiMessage::Kind::UserWords:
            userEntries_ = std::move(message->userEntries);
            DisplayUserWords();
            resultPanel_->Hide();
            break;
        case UiMessage::Kind::LegacyImported: {
            userEntries_ = std::move(message->userEntries);
            DisplayUserWords();
            wxFileConfig config(kConfigName);
            config.DeleteEntry("user_dict", false);
            config.Flush();
            legacyDictionary_.clear();
            legacyImportButton_->Hide();
            SetResult("Custom pronunciations imported", "Your original dictionary was not changed. Existing managed entries were preserved.");
            break;
        }
        case UiMessage::Kind::Error:
            pendingAudioReview_.reset();
            if (cancel_.load()) SetResult("Cancelled", wxui::toWx(message->error));
            else ShowProblem(message->error);
            break;
        default: break;
        }
        Layout(); SaveConfig();
    }
    wxString UnknownListText() const {
        wxString words;
        for (const auto& word : unknownWords_) {
            if (!words.empty()) words += ", ";
            words += wxui::toWx(word);
            if (words.length() > 500) { words += " ..."; break; }
        }
        return "NeoLIP does not know how to pronounce: " + words;
    }
    void Retry() {
        if (lastWasBatch_) RunBatch(true);
        else RunCreate(); // Re-read the edited visible transcript, not an old snapshot.
    }
    void LoadUserWords() {
        if (running_) return;
        const auto path = UserDataPath() / "pronunciations.dict";
        StartJob([path](const neolip::Context& context) {
            cslu::ScopedWorkControl control(context.work);
            context.work.check();
            UiMessage reply;
            reply.kind = UiMessage::Kind::UserWords;
            reply.userEntries = neolip::readUserPronunciations(path);
            return reply;
        });
    }
    void DisplayUserWords() {
        wxWindowUpdateLocker lock(userList_);
        userList_->DeleteAllItems();
        for (const auto& entry : userEntries_) {
            const long row = userList_->InsertItem(userList_->GetItemCount(), wxui::toWx(entry.word));
            userList_->SetItem(row, 1, wxui::toWx(entry.soundsLike));
            userList_->SetItem(row, 2, wxui::toWx(entry.phones));
        }
        userWordsLoaded_ = true;
    }
    void RefreshUserWords() {
        if (!userList_) return;
        try {
            userEntries_ = neolip::readUserPronunciations(UserDataPath() / "pronunciations.dict");
            DisplayUserWords();
        } catch (const std::exception& e) { ShowProblem(e.what()); }
    }
    bool EditPronunciation(neolip::UserPronunciation entry) {
        if (running_) return false;
        try {
            const auto assets = Assets();
            PronunciationDialog dialog(this, assets, entry); neoview::applyFontScale(&dialog, fontScale_);
            if (dialog.ShowModal() != wxID_OK) return false;
            const auto selected = dialog.Entry();
            const bool already = std::any_of(userEntries_.begin(), userEntries_.end(), [&](const auto& old) { return old.word == selected.word; });
            if (already && wxMessageBox("Replace your saved pronunciation for " + wxui::toWx(selected.word) + "?",
                "Replace pronunciation", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return false;
            neolip::saveUserPronunciation(assets, selected); RefreshUserWords();
            // An edit that renames a word is an explicit addition; do not erase
            // an existing entry under a different name implicitly.
#ifdef __EMSCRIPTEN__
            EM_ASM({ if (window.neoToolsFlushSettings) window.neoToolsFlushSettings(); });
#endif
            return true;
        } catch (const std::exception& e) { ShowProblem(e.what()); return false; }
    }
    void EditSelectedPronunciation() {
        const long selected = userList_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (selected >= 0 && static_cast<std::size_t>(selected) < userEntries_.size()) EditPronunciation(userEntries_[static_cast<std::size_t>(selected)]);
    }
    void RemoveSelectedPronunciation() {
        if (running_) return;
        const long selected = userList_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (selected < 0 || static_cast<std::size_t>(selected) >= userEntries_.size()) return;
        const auto entry = userEntries_[static_cast<std::size_t>(selected)];
        if (wxMessageBox("Remove your pronunciation for " + wxui::toWx(entry.word) + "? The bundled pronunciation, when present, will be used again.",
            "Remove pronunciation", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
        try { neolip::removeUserPronunciation(Assets(), entry.word); RefreshUserWords(); SaveConfig(); }
        catch (const std::exception& e) { ShowProblem(e.what()); }
    }
    void ImportUserDictionary() {
        if (running_) return;
        wxui::requestOpenFile(this, "Import custom pronunciations", "Dictionary files (*.dict;*.txt)|*.dict;*.txt|All files (*.*)|*.*",
            [this](std::optional<fs::path> path) {
                if (!path) return;
                try {
                    const auto assets = Assets(); const auto entries = neolip::readUserPronunciations(*path);
                    if (entries.empty()) throw std::runtime_error("This dictionary does not contain any entries.");
                    // Resolve all entries first. Invalid imports must not leave a
                    // partially imported dictionary with misleading success.
                    std::vector<neolip::UserPronunciation> checked;
                    for (const auto& entry : entries) checked.push_back(neolip::resolvePronunciation(assets, entry));
                    if (wxMessageBox(wxString::Format("Import %zu pronunciations? Entries with the same word replace your saved custom entries.", checked.size()),
                        "Import pronunciations", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
                    neolip::importUserPronunciations(assets, checked);
                    RefreshUserWords(); SaveConfig(); SetResult("Pronunciations imported", wxString::Format("%zu custom entries imported.", checked.size()));
                } catch (const std::exception& e) { ShowProblem(e.what()); }
            });
    }
    void AddUnknownPronunciations() {
        const auto pending = unknownWords_;
        for (const auto& word : pending) {
            if (!EditPronunciation({word, {}, {}})) break;
            unknownWords_.erase(std::remove(unknownWords_.begin(), unknownWords_.end(), word), unknownWords_.end());
        }
        if (unknownWords_.empty()) { SetResult("Pronunciations saved", "Retry to create the LIP using your new pronunciations."); retryButton_->Show(); }
        else { SetResult("Some words still need pronunciations", UnknownListText()); addUnknownButton_->Show(); retryButton_->Show(); }
        Layout();
    }
    void LookupWord() {
        if (running_) return;
        try {
            const auto assets = Assets(); const auto word = wxui::toStd(lookup_->GetValue());
            if (word.empty()) return;
            StartJob([assets, word](const neolip::Context& context) {
                UiMessage message; message.kind = UiMessage::Kind::Lookup; neolip::Engine engine(assets);
                message.lookup = engine.pronounce(word, context); return message;
            });
        } catch (const std::exception& e) { ShowProblem(e.what()); }
    }
    void ScanPronunciations() {
        if (running_) return;
        try {
            const auto assets = Assets(); const auto folder = ToPath(scanInput_->GetPath()); const bool recursive = scanRecursive_->GetValue();
            if (folder.empty()) throw std::runtime_error("Choose a transcript folder.");
            StartJob([assets, folder, recursive](const neolip::Context& context) {
                UiMessage message; message.kind = UiMessage::Kind::Scan;
                message.scan = neolip::scanUnknownWords(assets, folder, recursive, context); return message;
            });
        } catch (const std::exception& e) { ShowProblem(e.what()); }
    }
    void RefreshUnknownList() {
        unknownList_->DeleteAllItems();
        for (const auto& word : scanResult_.words) {
            const long row = unknownList_->InsertItem(unknownList_->GetItemCount(), wxui::toWx(word.word));
            unknownList_->SetItem(row, 1, wxString::Format("%zu", word.occurrences));
        }
    }
    void AddSelectedUnknown() {
        const long selected = unknownList_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (selected < 0 || static_cast<std::size_t>(selected) >= scanResult_.words.size()) return;
        const auto word = scanResult_.words[static_cast<std::size_t>(selected)].word;
        if (EditPronunciation({word, {}, {}})) {
            scanResult_.words.erase(scanResult_.words.begin() + selected); RefreshUnknownList();
        }
    }
    void ExportScan() {
        if (running_ || scanResult_.files == 0) return;
        const auto selected = wxui::chooseSaveFile(this, "Export unknown-word report", "TSV reports (*.tsv)|*.tsv", "unknown_words.tsv");
        if (!selected || !ConfirmReplace(*selected)) return;
        try { neolip::exportUnknownWords(*selected, scanResult_); wxui::publishBrowserFile(*selected); }
        catch (const std::exception& e) { ShowProblem(e.what()); }
    }
    void ExportLastOutput() {
        if (lastOutput_.empty() || running_) return;
#ifdef __EMSCRIPTEN__
        if (fs::is_regular_file(lastOutput_)) wxui::publishBrowserFile(lastOutput_, lastOutput_.filename().u8string());
        else ExportBatchZip();
#else
        const auto directory = fs::is_directory(lastOutput_) ? lastOutput_ : lastOutput_.parent_path();
        if (!directory.empty()) wxLaunchDefaultApplication(ToWx(directory));
#endif
    }
    void UpdateLipGridRow(int row) {
        if (lipGrid_ == nullptr || row < 0 ||
            row >= static_cast<int>(lipEditorEntries_.size()) ||
            row >= lipGrid_->GetNumberRows()) {
            return;
        }
        const GuiLipEntry& entry = lipEditorEntries_[static_cast<std::size_t>(row)];
        lipGrid_->SetCellValue(row, 0, wxString::Format("%d", row));
        lipGrid_->SetCellValue(row, 1, wxString::Format("%.9g", entry.timeSeconds));
        lipGrid_->SetReadOnly(row, 1, row == 0 || row + 1 == static_cast<int>(lipEditorEntries_.size()));
        lipGrid_->SetCellValue(row, 2, wxString::Format("%d", entry.shape));
        lipGrid_->SetCellValue(row, 3, KotorShapeName(entry.shape));
        lipGrid_->SetReadOnly(row, 0, true);
        lipGrid_->SetReadOnly(row, 3, true);
    }

    void RefreshLipGrid() {
        UpdateDocumentState();
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
            // Unchanged display strings must not re-quantize the original
            // float32 values. This also preserves equal-time imported records.
            if (row < static_cast<int>(lipEditorEntries_.size()) &&
                lipGrid_->GetCellValue(row, 1) == wxString::Format("%.9g", lipEditorEntries_[static_cast<std::size_t>(row)].timeSeconds))
                time = lipEditorEntries_[static_cast<std::size_t>(row)].timeSeconds;
            next.push_back({time, static_cast<std::uint8_t>(shape)});
        }
        lipEditorEntries_ = std::move(next);
        UpdateLipEditorStatus();
        return true;
    }

    void UpdateLipEditorStatus() {
        if (!lipEditorStatus_) return;
        if (lipEditorEntries_.empty()) { lipEditorStatus_->SetLabel("No mouth keys loaded."); return; }
        const auto validation = cslu::validateLipForKotorRuntime(cslu::lipFromKeyframes(lipEditorEntries_, timelineLipDuration_));
        wxString label = wxString::Format("%zu keys (including time zero)   Clip %.6fs   Final pose %u",
            lipEditorEntries_.size(), timelineLipDuration_, static_cast<unsigned>(lipEditorEntries_.back().shape));
        if (!validation.ok) label += "\nNeeds repair: " + wxui::toWx(validation.errors.front());
        else if (!validation.warnings.empty()) label += "\n" + wxui::toWx(validation.warnings.front());
        lipEditorStatus_->SetLabel(label);
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
        RefreshTimelinePanel();
        synchronizingEditViews_ = false;
    }

    void SyncTimelineToEditor() {
        if (timelinePanel_ == nullptr || synchronizingEditViews_) return;
        synchronizingEditViews_ = true;
        timelineEntries_ = timelinePanel_->Entries();
        lipEditorEntries_ = timelineEntries_;
        RefreshLipGrid();
        synchronizingEditViews_ = false;
        UpdateTimelineStatus();
    }

    void BindEditor() {
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SaveLipEditor(false); }, ID_SaveLipEditor);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SaveLipEditor(true); }, ID_SaveLipEditorAs);
        Bind(wxEVT_BUTTON, &MainFrame::OnInsertLipKey, this, ID_InsertLipKey);
        Bind(wxEVT_BUTTON, &MainFrame::OnDeleteLipKeys, this, ID_DeleteLipKeys);
        Bind(wxEVT_BUTTON, &MainFrame::OnShiftLipKeys, this, ID_ShiftLipKeys);
        Bind(wxEVT_BUTTON, &MainFrame::OnRepairLipKeys, this, ID_RepairLipKeys);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RestoreLipEdit(false); }, ID_UndoLip);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RestoreLipEdit(true); }, ID_RedoLip);
        Bind(wxEVT_BUTTON, &MainFrame::OnSetLipDuration, this, ID_SetLipDuration);
        Bind(wxEVT_BUTTON, &MainFrame::OnTimelinePlayCursor, this, ID_TimelinePlayCursor);
        Bind(wxEVT_CHOICE, &MainFrame::OnTimelineShapeChanged, this, ID_TimelineShapeChanged);
        Bind(wxEVT_SLIDER, &MainFrame::OnTimelineZoomChanged, this, ID_TimelineZoomChanged);
        lipGrid_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_DELETE && !lipGrid_->IsCellEditControlEnabled()) { wxCommandEvent command; OnDeleteLipKeys(command); }
            else event.Skip();
        });
        lipGrid_->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent& event) {
            if (!synchronizingEditViews_) {
                RememberLipEdit(); lipEditorDirty_ = true;
                if (SyncLipGridToEntries()) { RefreshLipGrid(); SyncEditorToTimeline(); }
            }
            event.Skip();
        });
        timelineLipPicker_->Bind(wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent& event) {
            if (!running_ && ConfirmLipEditorChanges()) LoadLipEditorPath(event.GetPath());
            else timelineLipPicker_->SetPath(ToWx(editorPath_));
        });
        timelineAudioPicker_->Bind(wxEVT_FILEPICKER_CHANGED, [this](wxFileDirPickerEvent&) { ReloadTimelineAudio(); });
    }
    void LoadLipEditorPath(const wxString& path,const fs::path& knownAudio={},int channel=-1,
                           const std::vector<neolip::AlignedWord>& words={},const std::string& transcript={}) {
        StopPlayback(); if (timelinePanel_) timelinePanel_->SetRange(0, 0); loopSelection_->SetValue(false); presetPreviewAvailable_ = false;
        if (previewMovement_) previewMovement_->Enable(false);
        if (running_) return;
        try {
            if (!IsRegularFile(path)) throw std::runtime_error("Choose an existing .lip file.");
            double duration = 0;
            auto entries = cslu::readKotorKeyframes(ToPath(path), &duration);
            editorPath_ = ToPath(path); lipEditorEntries_ = std::move(entries); timelineEntries_ = lipEditorEntries_;
            timelineLipDuration_ = duration; lipEditorDirty_ = false; timelineLipPicker_->SetPath(path);
            savedLip_ = {lipEditorEntries_, timelineLipDuration_}; undoLip_.clear(); redoLip_.clear();presetOnlyChange_=false;
            editorInput_=knownAudio;editorChannel_=channel;SetEditorWords(words,transcript);
            timelineAudio_.reset(); playback_.load({}); timelineAudioPicker_->SetPath(wxString{});
            RefreshLipGrid(); RefreshTimelinePanel(); notebook_->SetSelection(1);
            resultPanel_->Hide(); wxui::setStatusText(*this, "Loaded " + ToWx(editorPath_.filename()));
            const auto audio = knownAudio.empty()?neolip::matchingAudio(editorPath_):knownAudio;
            editorInput_=audio;
            if (!audio.empty()) { timelineAudioPicker_->SetPath(ToWx(audio)); ReloadTimelineAudio(); }
        } catch (const std::exception& e) { timelineLipPicker_->SetPath(ToWx(editorPath_)); ShowProblem(e.what()); }
    }
    void UseDialogueFolder(const fs::path& directory) {
        if (running_) return;
        batchSource_->SetSelection(1); notebook_->SetSelection(2);
        batchInput_->SetPath(ToWx(directory)); lastBatchDir_ = ToWx(directory);
        dialogueTlk_->SetPath(wxString{}); batchOutput_.clear();
        dialoguePicker_->SetInitialDirectory(ToWx(directory));
        UpdateBatchMode(); InvalidateBatch();
        if (!dialoguePicker_->GetPath().empty()) ScanBatch();
        else SetResult("Game directory selected", "Choose a DLG. NeoLIP will find its recordings in this directory and all subfolders.");
    }
    bool SaveLipEditor(bool saveAs) {
        if (running_ || !SyncLipGridToEntries(true)) return false;
        fs::path path = editorPath_;
        if (saveAs || path.empty()) {
            const auto selected = ChooseLipOutput("Save LIP As", path.empty() ? fs::path("edited.lip") : path);
            if (!selected) return false;
            path = *selected;
            if (path.extension().empty()) path += ".lip";
            if (!ConfirmReplace(path)) return false;
        }
        try {
            auto entries = lipEditorEntries_;
            neolip::saveEditedLip(path, entries, timelineLipDuration_, fs::exists(path));
            lipEditorEntries_ = std::move(entries); timelineEntries_ = lipEditorEntries_;
            editorPath_ = path; lastOutputDir_ = ToWx(path.parent_path());
            timelineLipPicker_->SetPath(ToWx(path)); lipEditorDirty_ = false;
            savedLip_ = {lipEditorEntries_, timelineLipDuration_};presetOnlyChange_=false;
            if(lastCreate_ && path!=lastCreate_->output){presetPreviewAvailable_=false;previewMovement_->Enable(false);}
            RefreshLipGrid(); RefreshTimelinePanel(); lastOutput_ = path;
            SetResult("Saved " + ToWx(path.filename())); showFolderButton_->Show(); Layout();
#ifdef __EMSCRIPTEN__
            wxui::publishBrowserFile(path, path.filename().u8string());
#endif
            return true;
        } catch (const std::exception& e) { ShowProblem(e.what()); return false; }
    }
    bool ConfirmLipEditorChanges() {
        if (!lipEditorDirty_) return true;
        const int answer = wxMessageBox("Save the current LIP changes before continuing?", "Unsaved LIP",
            wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
        if (answer == wxCANCEL) return false;
        return answer == wxNO || SaveLipEditor(false);
    }
    void OnClose(wxCloseEvent& event) {
        if (running_ && event.CanVeto()) {
            if (wxMessageBox("Cancel the current operation and close NeoLIP?", "Operation in progress",
                wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) == wxYES) {
                cancel_.store(true); closingAfterJob_ = true; resultTitle_->SetLabel("Cancelling safely...");
            }
            event.Veto(); return;
        }
        if (event.CanVeto() && !ConfirmLipEditorChanges()) { closingAfterJob_ = false; event.Veto(); return; }
        if (event.CanVeto() && !ConfirmDiscardTranscript()) { closingAfterJob_ = false; event.Veto(); return; }
        cancel_.store(true); event.Skip();
    }
    void ReloadTimelineAudio() {
        if (running_) return;
        StopPlayback(); loopSelection_->SetValue(false);
        timelineAudio_.reset(); playback_.load({}); RefreshTimelinePanel();
        const auto path = ToPath(timelineAudioPicker_->GetPath());
        if (path.empty()) { timelineAudio_.reset(); playback_.load({}); RefreshTimelinePanel(); return; }
        const int channel=(path==editorInput_)?editorChannel_:-1;
        if(path!=editorInput_)SetEditorWords({},{});
        StartJob([path,channel](const neolip::Context& context) {
            cslu::ScopedWorkControl control(context.work); context.work.check();
            UiMessage message; message.kind = UiMessage::Kind::Audio; message.path = path;
            message.audio = neolip::decodeAudio(path,channel); return message;
        });
    }
    void UpdateTimelineStatus() {
        if (!timelineStatus_) return;
        wxString label = wxString::Format("Audio: %.2fs   LIP: %.2fs   Keys: %zu",
            TimelineWaveform().durationSeconds(), timelineLipDuration_, timelineEntries_.size());
        wxString poseDetails;
        if (!timelineEntries_.empty() && timelinePanel_) {
            try {
                const auto pose = cslu::sampleLipPose(cslu::lipFromKeyframes(timelineEntries_, timelineLipDuration_), timelinePanel_->CursorSeconds());
                if (pose.fromShape == pose.toShape)
                    poseDetails += wxString::Format("   Cursor %.3fs: hold pose %u (talk %.2f%%)", timelinePanel_->CursorSeconds(),
                        static_cast<unsigned>(pose.fromShape), pose.fromTalkPosition * 100);
                else poseDetails += wxString::Format("   Cursor %.3fs: pose %u %.1f%% + pose %u %.1f%%",
                    timelinePanel_->CursorSeconds(), static_cast<unsigned>(pose.fromShape), (1-pose.weight)*100,
                    static_cast<unsigned>(pose.toShape), pose.weight*100);
                if (pose.completed) poseDetails += "   End pose (game blend-out is separate)";
            } catch (const std::exception&) { poseDetails += "   Blend preview unavailable until timing errors are repaired"; }
        }
        if(timelinePanel_)label+=wxString::Format("   Position: %.3fs",timelinePanel_->CursorSeconds());
        timelineStatus_->SetLabel(label);
        if(blendInspector_)blendInspector_->SetLabel(poseDetails);
    }
    void RefreshTimelinePanel() {
        UpdateDocumentState();
        if (!timelinePanel_) return;
        const bool previous = synchronizingEditViews_; synchronizingEditViews_ = true;
        timelinePanel_->SetData(timelineAudio_, timelineEntries_, timelineLipDuration_);
        synchronizingEditViews_ = previous;
        UpdateTimelineStatus();
    }
    void OnTimelineShapeChanged(wxCommandEvent&) {
        if (!timelinePanel_ || lipEditorEntries_.empty()) return;
        timelinePanel_->SetSelectedShape(timelineShapeChoice_->GetSelection()); SyncTimelineToEditor(); lipEditorDirty_ = true;
    }
    void OnTimelineZoomChanged(wxCommandEvent&) { if (timelinePanel_) timelinePanel_->SetZoom(timelineZoom_->GetValue()); }
    void OnTimelinePlayCursor(wxCommandEvent&) { TogglePlayback(); }
    void OnInsertLipKey(wxCommandEvent&) {
        if (!SyncLipGridToEntries(false) || lipEditorEntries_.size() < 2) return;
        const auto rows = SelectedLipRows();
        const std::size_t index = rows.empty() ? lipEditorEntries_.size() - 2 :
            std::min(static_cast<std::size_t>(std::max(0, rows.back())), lipEditorEntries_.size() - 2);
        const auto& a = lipEditorEntries_[index]; const auto& b = lipEditorEntries_[index + 1];
        const double t = static_cast<float>((a.timeSeconds + b.timeSeconds) / 2);
        if (!(t > a.timeSeconds && t < b.timeSeconds)) { ShowProblem("There is no representable time between these keys."); return; }
        RememberLipEdit();
        lipEditorEntries_.insert(lipEditorEntries_.begin() + static_cast<std::ptrdiff_t>(index + 1), {t, a.shape});
        lipEditorDirty_ = true; RefreshLipGrid(); SyncEditorToTimeline();
    }

    void OnDeleteLipKeys(wxCommandEvent&) {
        if (!SyncLipGridToEntries(false)) return;
        auto rows = SelectedLipRows();
        rows.erase(std::remove_if(rows.begin(), rows.end(), [this](int row) {
            return row <= 0 || row + 1 >= static_cast<int>(lipEditorEntries_.size());
        }), rows.end());
        if (rows.empty()) { ShowProblem("The time-zero and end keys are real poses and cannot be deleted. Their shapes can be edited; use Set duration to move the end."); return; }
        RememberLipEdit();
        for (auto it=rows.rbegin(); it!=rows.rend(); ++it) lipEditorEntries_.erase(lipEditorEntries_.begin()+*it);
        lipEditorDirty_=true; RefreshLipGrid(); SyncEditorToTimeline();
    }

    void OnShiftLipKeys(wxCommandEvent&) {
        if (!SyncLipGridToEntries(true)) return;
        double delta=0;
        if (!lipShiftAmount_->GetValue().ToDouble(&delta) || !std::isfinite(delta)) { ShowProblem("Enter a finite shift in seconds."); return; }
        const auto rows=SelectedLipRows(); if(rows.empty()) return;
        auto next = lipEditorEntries_;
        for (const int row: rows)
            if(row > 0 && row + 1 < static_cast<int>(next.size())) next[static_cast<std::size_t>(row)].timeSeconds += delta;
        const auto check=cslu::validateLipForKotorRuntime(cslu::lipFromKeyframes(next,timelineLipDuration_));
        if(!check.ok) { ShowProblem("That shift would invalidate the clip: " + check.errors.front()); return; }
        RememberLipEdit(); lipEditorEntries_=std::move(next); lipEditorDirty_=true; RefreshLipGrid(); SyncEditorToTimeline();
    }

    void OnSetLipDuration(wxCommandEvent&) {
        if (lipEditorEntries_.size() < 2 || !SyncLipGridToEntries(true)) return;
        wxTextEntryDialog dialog(this, "New clip duration in seconds. Only the end key moves; its pose is preserved.",
            "Set clip duration", wxString::Format("%.9g", timelineLipDuration_));
        if(dialog.ShowModal()!=wxID_OK) return;
        double duration=0;
        if(!dialog.GetValue().ToDouble(&duration) || !std::isfinite(duration) || !std::isfinite(static_cast<float>(duration)) || duration<=0 ||
            static_cast<float>(duration) <= lipEditorEntries_[lipEditorEntries_.size()-2].timeSeconds) {
            ShowProblem("Duration must be finite and later than the last interior key."); return;
        }
        RememberLipEdit(); timelineLipDuration_=static_cast<float>(duration);
        lipEditorEntries_.back().timeSeconds=timelineLipDuration_;
        lipEditorDirty_=true; RefreshLipGrid(); SyncEditorToTimeline();
    }

    void OnRepairLipKeys(wxCommandEvent&) {
        if (lipEditorEntries_.empty()) return;
        if(wxMessageBox("Repair will sort timestamps, remove non-finite keys, clamp out-of-range values, and keep the last key at each duplicate time. Missing endpoints will be added. The clip duration and nonzero final pose are preserved. This may change playback; Undo remains available. Continue?",
            "Explicit timing repair", wxYES_NO|wxNO_DEFAULT|wxICON_WARNING,this)!=wxYES) return;
        try {
            auto entries=lipEditorEntries_;
            cslu::repairKotorKeyframes(entries,timelineLipDuration_);
            RememberLipEdit(); lipEditorEntries_=std::move(entries);
            lipEditorDirty_=true; RefreshLipGrid(); SyncEditorToTimeline();
        } catch(const std::exception& error) { ShowProblem(error.what()); }
    }

#ifdef __EMSCRIPTEN__
    void ExportBatchZip() {
        try {
            // Explicit user export, not a generated validation sidecar. The ZIP
            // contains only completed .lip outputs in their source hierarchy.
            const auto path = neobrowser::createDownloadPath("NeoLIP-results.zip");
            {
                wxFFileOutputStream stream(ToWx(path));
                if (!stream.IsOk()) throw std::runtime_error("Cannot prepare the batch download.");
                wxZipOutputStream zip(stream);
                for (const auto& item : lastBatch_.items) {
                    if (item.status != neolip::Status::Created && item.status != neolip::Status::Skipped) continue;
                    if (!fs::is_regular_file(item.output)) continue;
                    const auto relative = item.output.lexically_relative(batchPlan_.outputRoot);
                    wxFFileInputStream input(ToWx(item.output));
                    if (!input.IsOk() || !zip.PutNextEntry(ToWx(relative))) throw std::runtime_error("Cannot read a batch LIP for export.");
                    zip.Write(input); if (!zip.CloseEntry()) throw std::runtime_error("Cannot write the batch ZIP.");
                }
                if (!zip.Close()) throw std::runtime_error("Cannot finish the batch ZIP.");
            }
            wxui::publishBrowserFile(path, "NeoLIP-results.zip");
        } catch (const std::exception& e) { ShowProblem(e.what()); }
    }
#endif
};


class NeoLipGuiApp final : public wxApp {
public:
    bool OnInit() override {
        if (!wxApp::OnInit()) return false;
        SetAppName("NeoLIP");
        auto* frame = new MainFrame(); frame->Show();
        return true;
    }
};
} // namespace
wxIMPLEMENT_APP(NeoLipGuiApp);
