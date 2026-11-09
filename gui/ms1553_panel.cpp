// gui/ms1553_panel.cpp
#include "ms1553_panel.h"

#include <algorithm>
#include <filesystem>

#include <wx/config.h>
#include <wx/filename.h>
#include <wx/progdlg.h>

using ch10::Message1553Row;

namespace {
wxString Utf8(const std::string& s) { return wxString::FromUTF8(s.c_str()); }
wxString Hex4(uint16_t v) { return wxString::Format("%04X", v); }
}

// ===========================================================================
MessageList::MessageList(wxWindow* parent, wxWindowID id)
    : wxListCtrl(parent, id, wxDefaultPosition, wxDefaultSize,
                 wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL | wxLC_HRULES) {
    AppendColumn("#",        wxLIST_FORMAT_RIGHT, 60);
    AppendColumn("Time",     wxLIST_FORMAT_LEFT,  165);
    AppendColumn("Ch",       wxLIST_FORMAT_RIGHT, 40);
    AppendColumn("Bus",      wxLIST_FORMAT_LEFT,  40);
    AppendColumn("Type",     wxLIST_FORMAT_LEFT,  105);
    AppendColumn("Command",  wxLIST_FORMAT_LEFT,  150);
    AppendColumn("Cmd 2 (RT-RT)", wxLIST_FORMAT_LEFT, 150);
    AppendColumn("Status",   wxLIST_FORMAT_LEFT,  90);
    AppendColumn("Errors",   wxLIST_FORMAT_LEFT,  70);
    AppendColumn("Data",     wxLIST_FORMAT_LEFT,  420);
    SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
    error_attr_.SetTextColour(wxColour(200, 40, 40));
}

void MessageList::SetRows(std::vector<Message1553Row>&& rows) {
    rows_ = std::move(rows);
    SetItemCount(static_cast<long>(rows_.size()));
    Refresh();
}

wxItemAttr* MessageList::OnGetItemAttr(long item) const {
    const auto* r = Row(item);
    return (r && !r->errors.empty()) ? &error_attr_ : nullptr;
}

wxString MessageList::OnGetItemText(long item, long col) const {
    const auto* r = Row(item);
    if (!r) return wxEmptyString;
    switch (col) {
        case 0: return wxString::Format("%llu", static_cast<unsigned long long>(r->index));
        case 1: return Utf8(r->time);
        case 2: return wxString::Format("%d", r->channel);
        case 3: return wxString(r->bus);
        case 4: return Utf8(r->type);
        case 5: return Utf8(r->cmd1_text);
        case 6: return r->has_cmd2 ? Utf8(r->cmd2_text) : wxString();
        case 7: {
            wxString s;
            if (r->has_status1) s += Hex4(r->status1);
            if (r->has_status2) s += " " + Hex4(r->status2);
            return s;
        }
        case 8: return Utf8(r->errors);
        case 9: return Utf8(r->data_hex);
        default: return wxEmptyString;
    }
}

// ===========================================================================
Ms1553Panel::Ms1553Panel(wxWindow* parent) : wxPanel(parent) {
    auto* box = new wxBoxSizer(wxVERTICAL);
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    bar->Add(new wxStaticText(this, wxID_ANY, "Channel:"), 0,
             wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    channel_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(300, -1));
    bar->Add(channel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    bar->Add(new wxStaticText(this, wxID_ANY, "Max:"), 0,
             wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    max_ = new wxSpinCtrl(this, wxID_ANY, "200000", wxDefaultPosition, wxDefaultSize,
                          wxSP_ARROW_KEYS, 1000, 10000000, 200000);
    bar->Add(max_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    time_ = new wxCheckBox(this, wxID_ANY, "IRIG time");
    time_->SetValue(true);
    bar->Add(time_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    follow_ = new wxCheckBox(this, wxID_ANY, "Follow playback");
    follow_->SetValue(true);
    bar->Add(follow_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    load_ = new wxButton(this, wxID_ANY, "Load messages");
    bar->Add(load_, 0, wxRIGHT, 6);
    export_ = new wxButton(this, wxID_ANY, "Export CSV...");
    bar->Add(export_, 0, wxRIGHT, 6);
    wordmap_btn_ = new wxButton(this, wxID_ANY, "Word map...");
    wordmap_btn_->SetToolTip("Load a CSV naming and scaling data words (see Tools > Write word map template)");
    bar->Add(wordmap_btn_, 0, wxRIGHT, 16);
    live_ = new wxStaticText(this, wxID_ANY, "");
    bar->Add(live_, 0, wxALIGN_CENTER_VERTICAL);
    box->Add(bar, 0, wxALL, 6);

    list_ = new MessageList(this, wxID_ANY);
    box->Add(list_, 3, wxEXPAND | wxLEFT | wxRIGHT, 6);

    detail_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 190),
                             wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    detail_->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
    box->Add(detail_, 0, wxEXPAND | wxALL, 6);
    SetSizer(box);

    load_->Bind(wxEVT_BUTTON, &Ms1553Panel::OnLoad, this);
    wordmap_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { LoadWordMap(); });
    if (wxConfigBase* cfg = wxConfigBase::Get(false)) {
        wxString saved;
        if (cfg->Read("WordMap", &saved) && !saved.IsEmpty() && wxFileName::FileExists(saved))
            LoadWordMapFile(saved);
    }
    export_->Bind(wxEVT_BUTTON, &Ms1553Panel::OnExport, this);
    list_->Bind(wxEVT_LIST_ITEM_SELECTED, &Ms1553Panel::OnSelected, this);
}

void Ms1553Panel::SetSession(ch10::Session* s) {
    session_ = s;
    channel_->Clear();
    channel_->Append("All 1553 channels");
    if (s)
        for (int id : s->ChannelsOfType(ch10::kMilStd1553F1)) {
            const auto* c = s->Channel(id);
            wxString label = wxString::Format("Channel %d", id);
            if (c && !c->source.empty()) label += " - " + Utf8(c->source);
            if (c) label += wxString::Format(" (%llu msgs)",
                                             static_cast<unsigned long long>(c->messages));
            channel_->Append(label);
        }
    channel_->SetSelection(channel_->GetCount() > 1 ? 1 : 0);
    bool have = s && channel_->GetCount() > 1;
    channel_->Enable(have); load_->Enable(have); export_->Enable(have);
    list_->SetRows({});
    detail_->Clear();
    live_->SetLabel("");
    last_follow_row_ = -1;

    // Auto-load a first batch so playback has something to follow.
    if (have) { wxCommandEvent ev; OnLoad(ev); }
}

int Ms1553Panel::SelectedChannel() const {
    int sel = channel_->GetSelection();
    if (sel <= 0 || !session_) return -1;
    auto ids = session_->ChannelsOfType(ch10::kMilStd1553F1);
    return (sel - 1 < static_cast<int>(ids.size())) ? ids[sel - 1] : -1;
}

void Ms1553Panel::OnLoad(wxCommandEvent&) {
    if (!session_) return;
    long max = max_->GetValue();
    wxProgressDialog dlg("Loading 1553 messages", "Decoding...", 1000, this,
                         wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
    std::vector<Message1553Row> rows;
    session_->Load1553(SelectedChannel(), max, time_->GetValue(), rows,
        [&dlg](uint64_t done, uint64_t total) {
            return dlg.Update(total ? static_cast<int>(std::min<uint64_t>(999, done * 1000 / total)) : 0);
        });
    dlg.Update(1000);
    std::size_t n = rows.size();
    list_->SetRows(std::move(rows));
    detail_->Clear();
    last_follow_row_ = -1;
    if (on_status)
        on_status(wxString::Format("%zu 1553 messages loaded%s", n,
                  static_cast<long>(n) >= max ? " (display limit reached; CSV export writes all)" : ""));
}

void Ms1553Panel::FollowCursor(uint64_t rtc) {
    if (!session_ || !follow_->GetValue() || list_->Count() == 0) return;
    const auto& rows = list_->rows();
    auto it = std::upper_bound(rows.begin(), rows.end(), rtc,
                               [](uint64_t t, const Message1553Row& r) { return t < r.rtc; });
    long row = static_cast<long>(it - rows.begin()) - 1;   // last message <= cursor
    if (row < 0) row = 0;
    live_->SetLabel(wxString::Format("at cursor: message %ld of %ld", row + 1, list_->Count()));
    if (row == last_follow_row_) return;
    last_follow_row_ = row;
    list_->SetItemState(row, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                        wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    list_->EnsureVisible(row);
}

void Ms1553Panel::OnSelected(wxListEvent& ev) {
    const auto* r = list_->Row(ev.GetIndex());
    if (!r) return;
    wxString t;
    t += wxString::Format("Message #%llu   channel %d   bus %c   %s\n",
                          static_cast<unsigned long long>(r->index), r->channel,
                          r->bus, Utf8(r->time));
    t += wxString::Format("Type: %s   Gap1 %.1f us   Gap2 %.1f us\n",
                          Utf8(r->type), r->gap1 / 10.0, r->gap2 / 10.0);
    t += wxString::Format("Command 1: 0x%04X  %s\n", r->cmd1, Utf8(r->cmd1_text));
    if (r->has_cmd2)
        t += wxString::Format("Command 2: 0x%04X  %s\n", r->cmd2, Utf8(r->cmd2_text));
    if (r->has_status1)
        t += wxString::Format("Status 1:  0x%04X  RT%02d %s\n", r->status1,
                              (r->status1 >> 11) & 0x1F, Utf8(r->status1_flags));
    if (r->has_status2)
        t += wxString::Format("Status 2:  0x%04X  RT%02d %s\n", r->status2,
                              (r->status2 >> 11) & 0x1F, Utf8(r->status2_flags));
    if (!r->errors.empty()) t += "Errors:    " + Utf8(r->errors) + "\n";
    t += wxString::Format("Data (%zu words):", r->data.size());
    for (std::size_t i = 0; i < r->data.size(); ++i) {
        if (i % 8 == 0) t += wxString::Format("\n  [%02zu] ", i);
        t += Hex4(r->data[i]) + " ";
    }
    if (!r->data.empty()) {
        std::string ascii = ch10::WordsToAscii(r->data);
        long printable = std::count_if(ascii.begin(), ascii.end(), [](char c) { return c != '.'; });
        if (printable * 10 >= static_cast<long>(ascii.size()) * 7)
            t += "\nASCII:     \"" + Utf8(ascii) + "\"";
        auto decoded = wordmap_.Decode(r->channel, r->rt, r->sa, r->tr == 'T', r->data);
        if (!decoded.empty()) {
            t += "\nDecoded:";
            for (const auto& d : decoded)
                t += wxString::Format("\n  [%02d] %-18s = %s %s", d.word, Utf8(d.name), Utf8(d.value), Utf8(d.units));
        }
    }
    detail_->SetValue(t);
}

bool Ms1553Panel::LoadWordMapFile(const wxString& path) {
    std::string err;
    ch10::WordMap wm;
    if (!wm.LoadCsv(std::string(path.ToUTF8()), &err)) {
        wxMessageBox(Utf8(err), "Word map", wxOK | wxICON_WARNING, this);
        return false;
    }
    wordmap_ = wm; wordmap_path_ = path;
    wordmap_btn_->SetLabel(wxString::Format("Word map (%zu)", wordmap_.size()));
    if (wxConfigBase* cfg = wxConfigBase::Get(false)) { cfg->Write("WordMap", path); cfg->Flush(); }
    if (on_status) on_status(wxString::Format("Word map loaded: %zu entries from %s", wordmap_.size(), path));
    return true;
}

void Ms1553Panel::LoadWordMap() {
    wxFileDialog dlg(this, "Load 1553 word map", wxFileName(wordmap_path_).GetPath(), "",
                     "CSV files (*.csv)|*.csv|All files (*.*)|*.*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) LoadWordMapFile(dlg.GetPath());
}

void Ms1553Panel::ExportCsv() {
    if (!session_) return;
    int channel = SelectedChannel();
    wxFileName base(wxString(session_->path().wstring()));
    wxString suggested = base.GetName() +
        (channel >= 0 ? wxString::Format("_ch%d", channel) : wxString("_all")) + "_1553.csv";
    wxFileDialog dlg(this, "Export 1553 messages", base.GetPath(), suggested,
                     "CSV files (*.csv)|*.csv|All files (*.*)|*.*",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    wxProgressDialog pd("Exporting CSV", "Writing...", 1000, this,
                        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT |
                        wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME);
    bool ok = session_->Export1553Csv(std::filesystem::path(dlg.GetPath().ToStdWstring()),
                                      channel, time_->GetValue(),
        [&pd](uint64_t done, uint64_t total) {
            return pd.Update(total ? static_cast<int>(std::min<uint64_t>(999, done * 1000 / total)) : 0);
        });
    pd.Update(1000);
    if (ok) { if (on_status) on_status("1553 CSV written to " + dlg.GetPath()); }
    else if (session_->error() != "cancelled")
        wxMessageBox(Utf8(session_->error()), "Export CSV", wxOK | wxICON_ERROR, this);
}
