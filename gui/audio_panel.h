// gui/audio_panel.h
// The Audio pane: sources found in the recording (analog channels, audio
// streams inside video channels) plus 1553 data-word sources the user
// defines, with per-source enable/volume, master volume, mute, a level meter
// and a scrolling waveform of what is being played. Export to WAV.

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <wx/wx.h>
#include <wx/checklst.h>

#include "audio_engine.h"
#include "ch10/session.h"

// Dialog to define a 1553 data-word audio source.
class Ms1553AudioDialog : public wxDialog {
public:
    Ms1553AudioDialog(wxWindow* parent, const ch10::Session& s);
    ch10::Ms1553AudioSpec Spec() const;
private:
    const ch10::Session& session_;
    wxChoice* channel_ = nullptr;
    wxTextCtrl* rt_ = nullptr;
    wxTextCtrl* sa_ = nullptr;
    wxChoice* tr_ = nullptr;
    wxTextCtrl* first_word_ = nullptr;
    wxTextCtrl* word_count_ = nullptr;
    wxChoice* format_ = nullptr;
    wxTextCtrl* rate_ = nullptr;
};

class WaveformPanel : public wxPanel {
public:
    WaveformPanel(wxWindow* parent, AudioEngine* engine);
private:
    void OnPaint(wxPaintEvent&);
    AudioEngine* engine_;
    std::vector<float> buf_;
};

class AudioPanel : public wxPanel {
public:
    AudioPanel(wxWindow* parent, AudioEngine* engine);

    // Discover sources in the session (analog channels, TS audio) and load them.
    void SetSession(ch10::Session* s);
    void RefreshList();

    std::function<uint64_t()> cursor;
    std::function<bool(uint64_t&, uint64_t&)> range;
    std::function<void(const wxString&)> on_status;

private:
    void OnCheck(wxCommandEvent&);
    void OnSelect(wxCommandEvent&);
    void OnGain(wxCommandEvent&);
    void OnMaster(wxCommandEvent&);
    void OnMute(wxCommandEvent&);
    void OnAdd1553(wxCommandEvent&);
    void OnExport(wxCommandEvent&);
    void OnTimer(wxTimerEvent&);

    AudioEngine*   engine_;
    ch10::Session* session_ = nullptr;
    wxCheckListBox* list_ = nullptr;
    wxStaticText*  info_ = nullptr;
    wxSlider*      gain_ = nullptr;
    wxSlider*      master_ = nullptr;
    wxCheckBox*    mute_ = nullptr;
    wxButton*      add1553_ = nullptr;
    wxButton*      export_ = nullptr;
    wxGauge*       meter_ = nullptr;
    WaveformPanel* wave_ = nullptr;
    wxStaticText*  status_ = nullptr;
    wxTimer        timer_;
};
