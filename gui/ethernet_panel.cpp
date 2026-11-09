// gui/ethernet_panel.cpp
#include "ethernet_panel.h"

#include <algorithm>

#include <wx/filename.h>
#include <wx/progdlg.h>

#include "ch10/reader.h"
#include "timeline_panel.h"

namespace {
wxString Utf8(const std::string& s) { return wxString::FromUTF8(s.c_str()); }
}

wxString HexDump(const uint8_t* b, std::size_t n, std::size_t max_bytes) {
    wxString t;
    std::size_t lim = std::min(n, max_bytes);
    for (std::size_t off = 0; off < lim; off += 16) {
        t += wxString::Format("%06zX  ", off);
        wxString ascii;
        for (std::size_t k = 0; k < 16; ++k) {
            if (off + k < lim) {
                t += wxString::Format("%02X ", b[off + k]);
                char c = static_cast<char>(b[off + k]);
                ascii += (c >= 32 && c < 127) ? wxString(c) : wxString(".");
            } else t += "   ";
            if (k == 7) t += " ";
        }
        t += " " + ascii + "\n";
    }
    if (n > lim) t += wxString::Format("... %zu more bytes\n", n - lim);
    return t;
}

// ===========================================================================
EthList::EthList(wxWindow* parent)
    : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                 wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL | wxLC_HRULES) {
    AppendColumn("#", wxLIST_FORMAT_RIGHT, 60);
    AppendColumn("Time", wxLIST_FORMAT_LEFT, 165);
    AppendColumn("Ch", wxLIST_FORMAT_RIGHT, 40);
    AppendColumn("Net", wxLIST_FORMAT_RIGHT, 40);
    AppendColumn("Len", wxLIST_FORMAT_RIGHT, 55);
    AppendColumn("Proto", wxLIST_FORMAT_LEFT, 60);
    AppendColumn("Source", wxLIST_FORMAT_LEFT, 170);
    AppendColumn("Destination", wxLIST_FORMAT_LEFT, 170);
    AppendColumn("Errors", wxLIST_FORMAT_LEFT, 60);
    AppendColumn("Payload", wxLIST_FORMAT_LEFT, 420);
    SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
}

void EthList::SetRows(std::vector<EthRow>&& rows, const ch10::Session* s) {
    rows_ = std::move(rows); session_ = s;
    SetItemCount(static_cast<long>(rows_.size()));
    Refresh();
}

wxString EthList::OnGetItemText(long item, long col) const {
    const EthRow* r = Row(item);
    if (!r || !session_) return wxEmptyString;
    switch (col) {
        case 0: return wxString::Format("%ld", item + 1);
        case 1: return Utf8(session_->TimeString(r->rtc));
        case 2: return wxString::Format("%d", r->channel);
        case 3: return wxString::Format("%d", r->netid);
        case 4: return wxString::Format("%d", r->length);
        case 5: return r->proto;
        case 6: return r->src;
        case 7: return r->dst;
        case 8: return r->errors;
        case 9: {
            wxString t;
            for (std::size_t i = 0; i < std::min<std::size_t>(r->payload.size(), 80); ++i) {
                char c = static_cast<char>(r->payload[i]); t += (c >= 32 && c < 127) ? wxString(c) : wxString(".");
            }
            return t;
        }
        default: return wxEmptyString;
    }
}

EthernetPanel::EthernetPanel(wxWindow* parent) : wxPanel(parent) {
    auto* box = new wxBoxSizer(wxVERTICAL);
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    bar->Add(new wxStaticText(this, wxID_ANY, "Channel:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    channel_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(280, -1));
    bar->Add(channel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    bar->Add(new wxStaticText(this, wxID_ANY, "Max:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    max_ = new wxSpinCtrl(this, wxID_ANY, "200000", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1000, 10000000, 200000);
    bar->Add(max_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    follow_ = new wxCheckBox(this, wxID_ANY, "Follow playback"); follow_->SetValue(true);
    bar->Add(follow_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    load_ = new wxButton(this, wxID_ANY, "Load frames"); bar->Add(load_, 0, wxRIGHT, 6);
    pcap_ = new wxButton(this, wxID_ANY, "Export PCAP..."); bar->Add(pcap_, 0);
    box->Add(bar, 0, wxALL, 6);
    list_ = new EthList(this);
    box->Add(list_, 3, wxEXPAND | wxLEFT | wxRIGHT, 6);
    detail_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 170), wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    detail_->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
    box->Add(detail_, 0, wxEXPAND | wxALL, 6);
    SetSizer(box);
    load_->Bind(wxEVT_BUTTON, &EthernetPanel::OnLoad, this);
    pcap_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ExportPcap(); });
    list_->Bind(wxEVT_LIST_ITEM_SELECTED, &EthernetPanel::OnSelected, this);
}

void EthernetPanel::SetSession(ch10::Session* s) {
    session_ = s;
    channel_->Clear();
    channel_->Append("All Ethernet channels");
    if (s) for (const auto& c : s->channels())
        if (c.data_type == ch10::kEthernetF0 || c.data_type == 0x69)
            channel_->Append(wxString::Format("Channel %d - %s%s%s (%llu frames)", c.channel_id,
                             c.data_type == 0x69 ? "ARINC-664" : "Ethernet", c.source.empty() ? "" : " - ",
                             Utf8(c.source), static_cast<unsigned long long>(c.messages)));
    channel_->SetSelection(0);
    bool have = s && channel_->GetCount() > 1;
    channel_->Enable(have); load_->Enable(have); pcap_->Enable(have);
    list_->SetRows({}, s); detail_->Clear(); last_follow_ = -1;
    if (have) { wxCommandEvent e; OnLoad(e); }
}

int EthernetPanel::SelectedChannel() const {
    int sel = channel_->GetSelection();
    if (sel <= 0 || !session_) return -1;
    int i = 0;
    for (const auto& c : session_->channels())
        if (c.data_type == ch10::kEthernetF0 || c.data_type == 0x69) { if (++i == sel) return c.channel_id; }
    return -1;
}

void EthernetPanel::OnLoad(wxCommandEvent&) {
    if (!session_) return;
    int ch = SelectedChannel();
    long max = max_->GetValue();
    std::vector<EthRow> rows;
    ch10::Reader rd(session_->path());
    ch10::Packet p;
    std::vector<ch10::eth::Frame> frames;
    ch10::eth::Csdw csdw;
    for (uint32_t i : session_->ByTime()) {
        const auto& e = session_->packets()[i];
        if (e.data_type != ch10::kEthernetF0 && e.data_type != 0x69) continue;
        if (ch >= 0 && e.channel != ch) continue;
        if (!rd.ReadAt(static_cast<std::streamoff>(e.offset), p)) continue;
        if (!ch10::eth::Decode(e.data_type, p.Body(), p.BodyLen(), p.header.flags, p.header.rtc, csdw, frames)) continue;
        for (const auto& f : frames) {
            if (static_cast<long>(rows.size()) >= max) break;
            ch10::eth::Parsed pa = f.arinc664 ? ch10::eth::ParseArinc664(f) : ch10::eth::Parse(f.data, f.length, f.payload_only);
            EthRow r; r.rtc = f.rtc; r.channel = e.channel; r.netid = f.netid; r.length = f.length;
            r.proto = Utf8(pa.Protocol()); r.src = Utf8(pa.Source()); r.dst = Utf8(pa.Destination());
            r.errors = Utf8(f.ErrorFlags());
            r.frame.assign(f.data, f.data + f.length);
            if (pa.payload && pa.payload_len) r.payload.assign(pa.payload, pa.payload + pa.payload_len);
            rows.push_back(std::move(r));
        }
        if (static_cast<long>(rows.size()) >= max) break;
    }
    std::size_t n = rows.size();
    list_->SetRows(std::move(rows), session_);
    detail_->Clear(); last_follow_ = -1;
    if (on_status) on_status(wxString::Format("%zu Ethernet frames loaded", n));
}

void EthernetPanel::FollowCursor(uint64_t rtc) {
    if (!session_ || !follow_->GetValue() || list_->rows().empty()) return;
    const auto& rows = list_->rows();
    auto it = std::upper_bound(rows.begin(), rows.end(), rtc, [](uint64_t t, const EthRow& r) { return t < r.rtc; });
    long row = static_cast<long>(it - rows.begin()) - 1;
    if (row < 0) row = 0;
    if (row == last_follow_) return;
    last_follow_ = row;
    list_->SetItemState(row, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    list_->EnsureVisible(row);
}

void EthernetPanel::OnSelected(wxListEvent& ev) {
    const EthRow* r = list_->Row(ev.GetIndex());
    if (!r || !session_) return;
    wxString t = wxString::Format("%s  channel %d  net %d  %d bytes  %s %s -> %s%s\n\n",
                                  Utf8(session_->TimeString(r->rtc)), r->channel, r->netid, r->length,
                                  r->proto, r->src, r->dst, r->errors.IsEmpty() ? wxString() : "  ERR[" + r->errors + "]");
    if (!r->payload.empty()) t += "Payload:\n" + HexDump(r->payload.data(), r->payload.size()) + "\n";
    t += "Frame:\n" + HexDump(r->frame.data(), r->frame.size(), 512);
    detail_->SetValue(t);
}

void EthernetPanel::ExportPcap() {
    if (!session_) return;
    int ch = SelectedChannel();
    wxFileName base(wxString(session_->path().wstring()));
    wxFileDialog dlg(this, "Export Ethernet frames to PCAP", base.GetPath(),
                     base.GetName() + (ch >= 0 ? wxString::Format("_ch%d", ch) : wxString("_eth")) + ".pcap",
                     "PCAP files (*.pcap)|*.pcap", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    wxProgressDialog pd("Exporting PCAP", "Writing...", 1000, this, wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT);
    uint64_t n = 0;
    bool ok = ch10::eth::WritePcap(*session_, ch, std::filesystem::path(dlg.GetPath().ToStdWstring()), n,
        [&pd](uint64_t d, uint64_t t) { return pd.Update(t ? static_cast<int>(std::min<uint64_t>(999, d * 1000 / t)) : 0); });
    pd.Update(1000);
    if (on_status) on_status(ok ? wxString::Format("%llu frames written to %s", static_cast<unsigned long long>(n), dlg.GetPath())
                               : wxString("PCAP export failed"));
}

// ===========================================================================
MsgList::MsgList(wxWindow* parent)
    : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL | wxLC_HRULES) {
    AppendColumn("#", wxLIST_FORMAT_RIGHT, 60);
    AppendColumn("Time", wxLIST_FORMAT_LEFT, 165);
    AppendColumn("Ch", wxLIST_FORMAT_RIGHT, 40);
    AppendColumn("Sub", wxLIST_FORMAT_RIGHT, 40);
    AppendColumn("Len", wxLIST_FORMAT_RIGHT, 55);
    AppendColumn("Text", wxLIST_FORMAT_LEFT, 800);
    SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
}

void MsgList::SetRows(std::vector<MsgRow>&& rows, const ch10::Session* s) {
    rows_ = std::move(rows); session_ = s;
    SetItemCount(static_cast<long>(rows_.size()));
    Refresh();
}

wxString MsgList::OnGetItemText(long item, long col) const {
    const MsgRow* r = Row(item);
    if (!r || !session_) return wxEmptyString;
    switch (col) {
        case 0: return wxString::Format("%ld", item + 1);
        case 1: return Utf8(session_->TimeString(r->rtc));
        case 2: return wxString::Format("%d", r->channel);
        case 3: return wxString::Format("%d", r->subchannel);
        case 4: return wxString::Format("%zu", r->msg.data.size());
        case 5: return Utf8(r->msg.Text());
        default: return wxEmptyString;
    }
}

MessagesPanel::MessagesPanel(wxWindow* parent) : wxPanel(parent) {
    auto* box = new wxBoxSizer(wxVERTICAL);
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    bar->Add(new wxStaticText(this, wxID_ANY, "Channel:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    channel_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(280, -1));
    bar->Add(channel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    follow_ = new wxCheckBox(this, wxID_ANY, "Follow playback"); follow_->SetValue(true);
    bar->Add(follow_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    load_ = new wxButton(this, wxID_ANY, "Load messages"); bar->Add(load_, 0);
    box->Add(bar, 0, wxALL, 6);
    list_ = new MsgList(this);
    box->Add(list_, 3, wxEXPAND | wxLEFT | wxRIGHT, 6);
    detail_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 150), wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    detail_->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
    box->Add(detail_, 0, wxEXPAND | wxALL, 6);
    SetSizer(box);
    load_->Bind(wxEVT_BUTTON, &MessagesPanel::OnLoad, this);
    list_->Bind(wxEVT_LIST_ITEM_SELECTED, &MessagesPanel::OnSelected, this);
}

void MessagesPanel::SetSession(ch10::Session* s) {
    session_ = s;
    channel_->Clear();
    channel_->Append("All message channels");
    if (s) for (const auto& c : s->channels())
        if (c.data_type == ch10::kMessageF0)
            channel_->Append(wxString::Format("Channel %d%s%s (%llu msgs)", c.channel_id, c.source.empty() ? "" : " - ",
                             Utf8(c.source), static_cast<unsigned long long>(c.messages)));
    channel_->SetSelection(0);
    bool have = s && channel_->GetCount() > 1;
    channel_->Enable(have); load_->Enable(have);
    list_->SetRows({}, s); detail_->Clear(); last_follow_ = -1;
    if (have) { wxCommandEvent e; OnLoad(e); }
}

int MessagesPanel::SelectedChannel() const {
    int sel = channel_->GetSelection();
    if (sel <= 0 || !session_) return -1;
    int i = 0;
    for (const auto& c : session_->channels())
        if (c.data_type == ch10::kMessageF0) { if (++i == sel) return c.channel_id; }
    return -1;
}

void MessagesPanel::OnLoad(wxCommandEvent&) {
    if (!session_) return;
    int ch = SelectedChannel();
    std::vector<MsgRow> rows;
    ch10::Reader rd(session_->path());
    ch10::Packet p;
    std::vector<ch10::Message> msgs;
    ch10::MessageCsdw csdw;
    for (uint32_t i : session_->ByTime()) {
        const auto& e = session_->packets()[i];
        if (e.data_type != ch10::kMessageF0) continue;
        if (ch >= 0 && e.channel != ch) continue;
        if (!rd.ReadAt(static_cast<std::streamoff>(e.offset), p)) continue;
        if (!ch10::DecodeMessageF0(p.Body(), p.BodyLen(), p.header.flags, p.header.rtc, csdw, msgs)) continue;
        for (auto& m : msgs) {
            MsgRow r; r.rtc = m.rtc; r.channel = e.channel; r.subchannel = m.subchannel;
            r.errors = m.data_error || m.format_error; r.msg = std::move(m);
            rows.push_back(std::move(r));
            if (rows.size() >= 500000) break;
        }
    }
    std::size_t n = rows.size();
    list_->SetRows(std::move(rows), session_);
    detail_->Clear(); last_follow_ = -1;
    if (on_status) on_status(wxString::Format("%zu messages loaded", n));
}

void MessagesPanel::FollowCursor(uint64_t rtc) {
    if (!session_ || !follow_->GetValue() || list_->rows().empty()) return;
    const auto& rows = list_->rows();
    auto it = std::upper_bound(rows.begin(), rows.end(), rtc, [](uint64_t t, const MsgRow& r) { return t < r.rtc; });
    long row = static_cast<long>(it - rows.begin()) - 1;
    if (row < 0) row = 0;
    if (row == last_follow_) return;
    last_follow_ = row;
    list_->SetItemState(row, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    list_->EnsureVisible(row);
}

void MessagesPanel::OnSelected(wxListEvent& ev) {
    const MsgRow* r = list_->Row(ev.GetIndex());
    if (!r || !session_) return;
    wxString t = wxString::Format("%s  channel %d  subchannel %d  %zu bytes%s\n\n%s\n\n",
                                  Utf8(session_->TimeString(r->rtc)), r->channel, r->subchannel, r->msg.data.size(),
                                  r->errors ? "  (error flags set)" : "", Utf8(r->msg.Text()));
    t += HexDump(r->msg.data.data(), r->msg.data.size());
    detail_->SetValue(t);
}
