// gui/main_frame.h
// Main window of ch10gui — a dockable (wxAUI) workspace:
//
//   toolbar:  Open | path | exports
//   top:      Recording pane — file, size, TMATS program, recording start /
//             end / duration, time source, channel counts
//   center:   tabs — Packets | MIL-STD-1553 | TMATS | Channels
//   right:    Video pane — one live view per video channel
//   bottom:   Playback pane — transport controls + data-flow timeline
//
// A single PlaybackController drives every pane, so pressing Play moves the
// timeline cursor, advances the video frames, and walks the packet and 1553
// tables in step. Panes can be dragged, resized, floated or closed; the View
// menu restores them.

#pragma once

#include <memory>
#include <vector>

#include <wx/wx.h>
#include <wx/aui/aui.h>
#include <wx/listctrl.h>
#include <wx/scrolwin.h>

#include "ch10/ops.h"
#include "ch10/session.h"
#include "annotations_panel.h"
#include "audio_engine.h"
#include "audio_panel.h"
#include "ethernet_panel.h"
#include "ms1553_panel.h"
#include "packets_panel.h"
#include "playback.h"
#include "timeline_panel.h"
#include "video_panel.h"

class MainFrame : public wxFrame {
public:
    MainFrame();
    ~MainFrame() override;

    void OpenFile(const wxString& path);

private:
    enum {
        ID_OPEN_PATH = wxID_HIGHEST + 1,
        ID_EXPORT_TMATS, ID_EXPORT_1553, ID_EXTRACT_VIDEO,
        ID_PLAY, ID_STOP, ID_REWIND, ID_FORWARD, ID_STEP_BACK, ID_STEP_FWD,
        ID_JUMP_START, ID_JUMP_END, ID_SPEED, ID_SLIDER,
        ID_VIEW_RECORDING, ID_VIEW_VIDEO, ID_VIEW_PLAYBACK, ID_VIEW_AUDIO, ID_VIEW_RESET,
        ID_SPEED_UP, ID_SPEED_DOWN,
        ID_SET_IN, ID_SET_OUT, ID_CLEAR_RANGE, ID_ADD_ANNOTATION,
        ID_TOOL_CUT, ID_TOOL_SPLIT, ID_TOOL_MERGE, ID_TOOL_SAVE_ANNOTATIONS,
        ID_TOOL_INJECT, ID_TOOL_PCAP, ID_TOOL_WORDMAP, ID_TOOL_WORDMAP_TEMPLATE,
        ID_THEME_FIRST, ID_THEME_LAST = ID_THEME_FIRST + 15,
    };

    // construction
    void BuildMenu();
    void BuildToolbar();
    wxPanel* BuildRecordingPane();
    wxWindow* BuildCenter();
    wxPanel* BuildTmatsPage(wxWindow* parent);
    wxPanel* BuildChannelsPage(wxWindow* parent);
    wxScrolledWindow* BuildVideoPane();
    wxPanel* BuildPlaybackPane();

    // events
    void OnMenuOpen(wxCommandEvent&);
    void OnOpenPath(wxCommandEvent&);
    void OnExportTmats(wxCommandEvent&);
    void OnExport1553(wxCommandEvent&) { ms1553_->ExportCsv(); }
    void OnExtractVideo(wxCommandEvent&);
    void OnPlay(wxCommandEvent&)      { controller_.TogglePlay(); }
    void OnStop(wxCommandEvent&)      { controller_.Stop(); }
    void OnRewind(wxCommandEvent&)    { controller_.Rewind(); }
    void OnForward(wxCommandEvent&)   { controller_.Play(); }
    void OnStepBack(wxCommandEvent&)  { controller_.StepSeconds(-1.0); }
    void OnStepFwd(wxCommandEvent&)   { controller_.StepSeconds(+1.0); }
    void OnJumpStart(wxCommandEvent&) { controller_.JumpStart(); }
    void OnJumpEnd(wxCommandEvent&)   { controller_.JumpEnd(); }
    void OnSpeed(wxCommandEvent&);
    void OnCharHook(wxKeyEvent&);
    void OnSpeedUp(wxCommandEvent&)   { controller_.SpeedUp(); }
    void OnSpeedDown(wxCommandEvent&) { controller_.SlowDown(); }
    void OnSlider(wxCommandEvent&);
    void OnViewPane(wxCommandEvent&);
    void OnTheme(wxCommandEvent&);
    void OnSetIn(wxCommandEvent&);
    void OnSetOut(wxCommandEvent&);
    void OnClearRange(wxCommandEvent&);
    void OnAddAnnotation(wxCommandEvent&) { annotations_->AddAtCursor(); }
    void OnToolCut(wxCommandEvent&);
    void OnToolSplit(wxCommandEvent&);
    void OnToolMerge(wxCommandEvent&);
    void OnToolSaveAnnotations(wxCommandEvent&);
    void OnToolInject(wxCommandEvent&);
    void OnToolWordMapTemplate(wxCommandEvent&);
    void OnAbout(wxCommandEvent&);
    void OnExit(wxCommandEvent&) { Close(true); }

    // playback listener
    void OnCursor(uint64_t rtc, bool playing);

    // refresh after open
    void RefreshAll();
    void RefreshRecordingPane();
    void RefreshChannelsPage();
    void RefreshVideoPane();
    void ClearAll();
    void ApplyCurrentTheme();
    void UpdateRangeDisplay();
    // Run a file operation with a progress dialog; reports and offers to open.
    void ReportOperation(const wxString& title, bool ok, const ch10::OpResult& r);

    ch10::Session      session_;
    PlaybackController controller_;
    AudioEngine        audio_;
    AudioPanel*        audio_panel_ = nullptr;
    wxAuiManager       mgr_;
    wxString           default_perspective_;

    // toolbar
    wxTextCtrl*   path_ctrl_ = nullptr;

    // recording pane
    wxPanel*      recording_pane_ = nullptr;
    wxStaticText* rec_values_[10] = {};

    // center
    wxAuiNotebook* notebook_  = nullptr;
    PacketsPanel*  packets_   = nullptr;
    Ms1553Panel*   ms1553_    = nullptr;
    AnnotationsPanel* annotations_ = nullptr;
    EthernetPanel* ethernet_ = nullptr;
    MessagesPanel* messages_ = nullptr;
    wxTextCtrl*    tmats_text_ = nullptr;
    wxListCtrl*    channel_list_ = nullptr;

    // video
    wxScrolledWindow*        video_pane_ = nullptr;
    wxSizer*                 video_sizer_ = nullptr;
    std::vector<VideoPanel*> video_panels_;
    wxStaticText*            video_hint_ = nullptr;

    // playback pane
    wxPanel*       playback_pane_ = nullptr;
    TimelinePanel* timeline_ = nullptr;
    wxButton*      play_btn_ = nullptr;
    wxChoice*      speed_    = nullptr;
    wxSlider*      slider_   = nullptr;
    wxStaticText*  time_abs_ = nullptr;
    wxStaticText*  time_rel_ = nullptr;
    wxStaticText*  range_label_ = nullptr;
    bool           updating_slider_ = false;

    // In/Out selection
    bool     in_set_ = false, out_set_ = false;
    uint64_t in_rtc_ = 0, out_rtc_ = 0;
    bool RangeValid() const { return in_set_ && out_set_ && out_rtc_ > in_rtc_; }
};
