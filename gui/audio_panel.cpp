// gui/audio_panel.cpp
#include "audio_panel.h"

#include <algorithm>
#include <cmath>

#include <wx/dcbuffer.h>
#include <wx/filename.h>
#include <wx/progdlg.h>

#include "theme.h"
#include "timeline_panel.h"

namespace {
wxString Utf8(const std::string& s) { return wxString::FromUTF8(s.c_str()); }
}

// ===========================================================================
Ms1553AudioDialog::Ms1553AudioDialog(wxWindow* parent, const ch10::Session& s)
    : wxDialog(parent, wxID_ANY, "1553 data-word audio source", wxDefaultPosition, wxSize(520, 420)),
      session_(s) {
    auto* box = new wxBoxSizer(wxVERTICAL);
    box->Add(new wxStaticText(this, wxID_ANY,
        "Interpret the data words of matching messages as PCM audio samples.\n"
        "Leave RT/SA blank for any; the sample rate is derived from the message\n"
        "cadence unless you set it."), 0, wxALL, 12);
    auto* grid = new wxFlexGridSizer(2, 8, 12);
    grid->AddGrowableCol(1, 1);
    auto row = [&](const char* label, wxWindow* w) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(w, 1, wxEXPAND);
    };
    channel_ = new wxChoice(this, wxID_ANY);
    for (int id : s.ChannelsOfType(ch10::kMilStd1553F1)) {
        const auto* c = s.Channel(id);
        channel_->Append(wxString::Format("Channel %d%s%s", id, c && !c->source.empty() ? " - " : "",
                                          c ? Utf8(c->source) : wxString()));
    }
    channel_->SetSelection(0);
    row("1553 channel", channel_);
    rt_ = new wxTextCtrl(this, wxID_ANY, ""); row("RT address", rt_);
    sa_ = new wxTextCtrl(this, wxID_ANY, ""); row("Subaddress", sa_);
    tr_ = new wxChoice(this, wxID_ANY); tr_->Append("Any"); tr_->Append("Receive"); tr_->Append("Transmit"); tr_->SetSelection(0);
    row("Direction", tr_);
    first_word_ = new wxTextCtrl(this, wxID_ANY, "0"); row("First data word", first_word_);
    word_count_ = new wxTextCtrl(this, wxID_ANY, ""); row("Word count (blank = rest)", word_count_);
    format_ = new wxChoice(this, wxID_ANY);
    for (auto f : {"16-bit signed", "16-bit unsigned", "2 x 8-bit signed", "2 x 8-bit unsigned", "2 x G.711 mu-law", "2 x G.711 A-law"})
        format_->Append(f);
    format_->SetSelection(0);
    row("Sample format", format_);
    rate_ = new wxTextCtrl(this, wxID_ANY, ""); row("Sample rate (Hz, blank = auto)", rate_);
    box->Add(grid, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);
    box->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(box);
    ApplyTheme(this, nullptr, nullptr);
}

ch10::Ms1553AudioSpec Ms1553AudioDialog::Spec() const {
    ch10::Ms1553AudioSpec spec;
    auto ids = session_.ChannelsOfType(ch10::kMilStd1553F1);
    int sel = channel_->GetSelection();
    spec.channel = (sel >= 0 && sel < static_cast<int>(ids.size())) ? ids[static_cast<std::size_t>(sel)] : -1;
    long v;
    if (rt_->GetValue().ToLong(&v)) spec.rt = static_cast<int>(v);
    if (sa_->GetValue().ToLong(&v)) spec.sa = static_cast<int>(v);
    spec.tr = tr_->GetSelection() == 0 ? -1 : (tr_->GetSelection() == 1 ? 0 : 1);
    if (first_word_->GetValue().ToLong(&v)) spec.first_word = static_cast<int>(v);
    if (word_count_->GetValue().ToLong(&v)) spec.word_count = static_cast<int>(v);
    spec.format = static_cast<ch10::Ms1553AudioSpec::Format>(format_->GetSelection());
    double r; if (rate_->GetValue().ToCDouble(&r)) spec.sample_rate = r;
    return spec;
}

// ===========================================================================
WaveformPanel::WaveformPanel(wxWindow* parent, AudioEngine* engine)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 70), wxFULL_REPAINT_ON_RESIZE),
      engine_(engine) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &WaveformPanel::OnPaint, this);
}

void WaveformPanel::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    const Theme& th = CurrentTheme();
    dc.SetBackground(wxBrush(th.canvas));
    dc.Clear();
    wxSize sz = GetClientSize();
    int mid = sz.y / 2;
    dc.SetPen(wxPen(th.grid));
    dc.DrawLine(0, mid, sz.x, mid);
    engine_->RecentOutput(buf_, static_cast<std::size_t>(engine_->output_rate()) * 2);   // last 2 s
    if (buf_.empty()) return;
    dc.SetPen(wxPen(th.accent));
    std::size_t per_px = std::max<std::size_t>(1, buf_.size() / static_cast<std::size_t>(std::max(1, sz.x)));
    for (int x = 0; x < sz.x; ++x) {
        std::size_t a = static_cast<std::size_t>(x) * per_px, b = std::min(buf_.size(), a + per_px);
        if (a >= buf_.size()) break;
        float lo = 1, hi = -1;
        for (std::size_t i = a; i < b; ++i) { lo = std::min(lo, buf_[i]); hi = std::max(hi, buf_[i]); }
        int y0 = mid - static_cast<int>(hi * (mid - 2)), y1 = mid - static_cast<int>(lo * (mid - 2));
        dc.DrawLine(x, y0, x, std::max(y1, y0 + 1));
    }
}

// ===========================================================================
AudioPanel::AudioPanel(wxWindow* parent, AudioEngine* engine)
    : wxPanel(parent), engine_(engine), timer_(this) {
    auto* box = new wxBoxSizer(wxVERTICAL);

    auto* top = new wxBoxSizer(wxHORIZONTAL);
    list_ = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 64));
    top->Add(list_, 1, wxEXPAND | wxRIGHT, 8);
    auto* btns = new wxBoxSizer(wxVERTICAL);
    add1553_ = new wxButton(this, wxID_ANY, "Add 1553 audio...");
    export_  = new wxButton(this, wxID_ANY, "Export WAV...");
    btns->Add(add1553_, 0, wxEXPAND | wxBOTTOM, 4);
    btns->Add(export_, 0, wxEXPAND);
    top->Add(btns, 0);
    box->Add(top, 0, wxEXPAND | wxALL, 6);

    info_ = new wxStaticText(this, wxID_ANY, "");
    box->Add(info_, 0, wxLEFT | wxRIGHT, 8);

    auto* ctl = new wxFlexGridSizer(4, 4, 8);
    ctl->AddGrowableCol(1, 1);
    ctl->AddGrowableCol(3, 1);
    ctl->Add(new wxStaticText(this, wxID_ANY, "Source volume"), 0, wxALIGN_CENTER_VERTICAL);
    gain_ = new wxSlider(this, wxID_ANY, 80, 0, 100);
    ctl->Add(gain_, 1, wxEXPAND);
    ctl->Add(new wxStaticText(this, wxID_ANY, "Master"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    master_ = new wxSlider(this, wxID_ANY, 80, 0, 100);
    ctl->Add(master_, 1, wxEXPAND);
    ctl->Add(new wxStaticText(this, wxID_ANY, "Level"), 0, wxALIGN_CENTER_VERTICAL);
    meter_ = new wxGauge(this, wxID_ANY, 100);
    ctl->Add(meter_, 1, wxEXPAND);
    mute_ = new wxCheckBox(this, wxID_ANY, "Mute");
    ctl->Add(mute_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    status_ = new wxStaticText(this, wxID_ANY, "");
    ctl->Add(status_, 0, wxALIGN_CENTER_VERTICAL);
    box->Add(ctl, 0, wxEXPAND | wxALL, 6);

    wave_ = new WaveformPanel(this, engine);
    box->Add(wave_, 1, wxEXPAND | wxALL, 6);
    SetSizer(box);

    list_->Bind(wxEVT_CHECKLISTBOX, &AudioPanel::OnCheck, this);
    list_->Bind(wxEVT_LISTBOX, &AudioPanel::OnSelect, this);
    gain_->Bind(wxEVT_SLIDER, &AudioPanel::OnGain, this);
    master_->Bind(wxEVT_SLIDER, &AudioPanel::OnMaster, this);
    mute_->Bind(wxEVT_CHECKBOX, &AudioPanel::OnMute, this);
    add1553_->Bind(wxEVT_BUTTON, &AudioPanel::OnAdd1553, this);
    export_->Bind(wxEVT_BUTTON, &AudioPanel::OnExport, this);
    Bind(wxEVT_TIMER, &AudioPanel::OnTimer, this);
    timer_.Start(50);
    RefreshList();
}

void AudioPanel::SetSession(ch10::Session* s) {
    session_ = s;
    engine_->Clear();
    if (s) {
        for (const auto& c : s->channels()) {
            if (c.data_type == ch10::kAnalogF1) {
                auto a = std::make_shared<ch10::AnalogAudioSource>();
                if (a->Open(*s, c.channel_id)) engine_->Add(a, "analog", c.channel_id);
            } else if (s->IsVideoType(c.data_type) && c.video.transport && ch10::TsAudioSource::Available()) {
                if (ch10::TsAudioSource::Probe(*s, c.channel_id, nullptr, nullptr, nullptr)) {
                    auto t = std::make_shared<ch10::TsAudioSource>();
                    if (t->Open(*s, c.channel_id)) engine_->Add(t, "video", c.channel_id);
                }
            }
        }
    }
    RefreshList();
}

void AudioPanel::RefreshList() {
    list_->Clear();
    auto slots = engine_->Slots();
    for (const auto& sl : slots) {
        list_->Append(Utf8(sl.source->name()));
        list_->Check(list_->GetCount() - 1, sl.enabled);
    }
    bool have = session_ != nullptr;
    add1553_->Enable(have && !session_->ChannelsOfType(ch10::kMilStd1553F1).empty());
    export_->Enable(have && !slots.empty());
    gain_->Enable(!slots.empty());
    if (!slots.empty()) { list_->SetSelection(0); wxCommandEvent e; OnSelect(e); }
    else info_->SetLabel(have ? "No audio sources found (analog channels or audio in video). Add a 1553 source if the bus carries PCM."
                              : "");
    status_->SetLabel(engine_->running() ? wxString::Format("%s, %d Hz", engine_->backend(), engine_->output_rate())
                                         : "Audio device: " + Utf8(engine_->error()));
}

void AudioPanel::OnCheck(wxCommandEvent& ev) {
    engine_->SetEnabled(static_cast<std::size_t>(ev.GetInt()), list_->IsChecked(static_cast<unsigned>(ev.GetInt())));
}

void AudioPanel::OnSelect(wxCommandEvent&) {
    int i = list_->GetSelection();
    auto slots = engine_->Slots();
    if (i < 0 || i >= static_cast<int>(slots.size())) return;
    const auto& sl = slots[static_cast<std::size_t>(i)];
    info_->SetLabel(Utf8(sl.source->info()));
    gain_->SetValue(static_cast<int>(sl.gain * 100));
}

void AudioPanel::OnGain(wxCommandEvent&) {
    int i = list_->GetSelection();
    if (i >= 0) engine_->SetGain(static_cast<std::size_t>(i), gain_->GetValue() / 100.0f);
}

void AudioPanel::OnMaster(wxCommandEvent&) { engine_->SetMaster(master_->GetValue() / 100.0f); }
void AudioPanel::OnMute(wxCommandEvent&) { engine_->SetMuted(mute_->GetValue()); }

void AudioPanel::OnAdd1553(wxCommandEvent&) {
    if (!session_) return;
    Ms1553AudioDialog dlg(this, *session_);
    if (dlg.ShowModal() != wxID_OK) return;
    wxProgressDialog pd("Reading 1553 audio", "Scanning messages...", 1000, this,
                        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT);
    auto src = std::make_shared<ch10::Ms1553AudioSource>();
    bool ok = src->Open(*session_, dlg.Spec(), [&pd](uint64_t d, uint64_t t) {
        return pd.Update(t ? static_cast<int>(std::min<uint64_t>(999, d * 1000 / t)) : 0); });
    pd.Update(1000);
    if (!ok) {
        if (src->error() != "cancelled")
            wxMessageBox(Utf8(src->error()), "1553 audio", wxOK | wxICON_WARNING, this);
        return;
    }
    engine_->Add(src, "1553", dlg.Spec().channel);
    RefreshList();
    list_->SetSelection(static_cast<int>(list_->GetCount()) - 1);
    wxCommandEvent e; OnSelect(e);
}

void AudioPanel::OnExport(wxCommandEvent&) {
    int i = list_->GetSelection();
    auto slots = engine_->Slots();
    if (!session_ || i < 0 || i >= static_cast<int>(slots.size())) return;
    auto src = slots[static_cast<std::size_t>(i)].source;
    uint64_t from = src->first_rtc(), to = src->last_rtc();
    if (range) { uint64_t a, b; if (range(a, b)) { from = a; to = b; } }
    wxFileName base(wxString(session_->path().wstring()));
    wxFileDialog dlg(this, "Export audio as WAV", base.GetPath(),
                     base.GetName() + wxString::Format("_ch%d.wav", slots[static_cast<std::size_t>(i)].channel),
                     "WAV files (*.wav)|*.wav", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    wxProgressDialog pd("Exporting WAV", "Rendering...", 1000, this,
                        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
    int rate = static_cast<int>(src->sample_rate() + 0.5);
    bool ok = ch10::WriteWav(*src, from, to, rate, std::filesystem::path(dlg.GetPath().ToStdWstring()),
                             [&pd](uint64_t d, uint64_t t) {
                                 return pd.Update(t ? static_cast<int>(std::min<uint64_t>(999, d * 1000 / t)) : 0); });
    pd.Update(1000);
    if (on_status) on_status(ok ? "WAV written to " + dlg.GetPath() : wxString("WAV export failed"));
}

void AudioPanel::OnTimer(wxTimerEvent&) {
    float p = engine_->output_peak();
    meter_->SetValue(static_cast<int>(std::clamp(p, 0.0f, 1.0f) * 100));
    wave_->Refresh(false);
}
