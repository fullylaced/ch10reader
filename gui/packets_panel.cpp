// gui/packets_panel.cpp
#include "packets_panel.h"

#include <algorithm>

#include "timeline_panel.h"   // TypeShortName

using ch10::PacketIndexEntry;

PacketList::PacketList(wxWindow* parent, wxWindowID id)
    : wxListCtrl(parent, id, wxDefaultPosition, wxDefaultSize,
                 wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL | wxLC_HRULES) {
    AppendColumn("#",         wxLIST_FORMAT_RIGHT, 70);
    AppendColumn("Offset",    wxLIST_FORMAT_RIGHT, 100);
    AppendColumn("Ch",        wxLIST_FORMAT_RIGHT, 45);
    AppendColumn("Type",      wxLIST_FORMAT_LEFT,  200);
    AppendColumn("Seq",       wxLIST_FORMAT_RIGHT, 45);
    AppendColumn("Time",      wxLIST_FORMAT_LEFT,  165);
    AppendColumn("Rel time",  wxLIST_FORMAT_LEFT,  110);
    AppendColumn("RTC",       wxLIST_FORMAT_RIGHT, 120);
    AppendColumn("Pkt len",   wxLIST_FORMAT_RIGHT, 75);
    AppendColumn("Data len",  wxLIST_FORMAT_RIGHT, 75);
    AppendColumn("Flags",     wxLIST_FORMAT_LEFT,  120);
    SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
}

void PacketList::SetSource(ch10::Session* s, std::vector<uint32_t> rows) {
    session_ = s;
    rows_ = std::move(rows);
    SetItemCount(static_cast<long>(rows_.size()));
    Refresh();
}

long PacketList::RowOfPacket(uint32_t index) const {
    auto it = std::lower_bound(rows_.begin(), rows_.end(), index);
    if (it == rows_.end() || *it != index) return -1;
    return static_cast<long>(it - rows_.begin());
}

wxString PacketList::OnGetItemText(long item, long col) const {
    if (!session_ || item < 0 || item >= Count()) return wxEmptyString;
    uint32_t i = rows_[item];
    const PacketIndexEntry& e = session_->packets()[i];
    switch (col) {
        case 0: return wxString::Format("%u", i + 1);
        case 1: return wxString::Format("0x%llX", static_cast<unsigned long long>(e.offset));
        case 2: return wxString::Format("%u", e.channel);
        case 3: return wxString::Format("0x%02X %s", e.data_type, TypeShortName(e.data_type));
        case 4: return wxString::Format("%u", e.seq);
        case 5: return wxString::FromUTF8(session_->TimeString(e.rtc).c_str());
        case 6: return wxString::FromUTF8(session_->RelativeString(e.rtc).c_str());
        case 7: return wxString::Format("%llu", static_cast<unsigned long long>(e.rtc));
        case 8: return wxString::Format("%u", e.packet_len);
        case 9: return wxString::Format("%u", e.data_len);
        case 10: {
            wxString f;
            if (e.flags & 0x80) f += "SEC ";
            if (e.flags & 0x40) f += "IPTS-SEC ";
            if (e.flags & 0x20) f += "RTCERR ";
            if (e.flags & 0x10) f += "OVF ";
            static const char* ck[] = {"", "CK8", "CK16", "CK32"};
            f += ck[e.flags & 3];
            return f;
        }
        default: return wxEmptyString;
    }
}

// ---------------------------------------------------------------------------
PacketsPanel::PacketsPanel(wxWindow* parent) : wxPanel(parent) {
    auto* box = new wxBoxSizer(wxVERTICAL);
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    bar->Add(new wxStaticText(this, wxID_ANY, "Channel:"), 0,
             wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    filter_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(280, -1));
    bar->Add(filter_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    follow_ = new wxCheckBox(this, wxID_ANY, "Follow playback");
    follow_->SetValue(true);
    bar->Add(follow_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    count_ = new wxStaticText(this, wxID_ANY, "");
    bar->Add(count_, 0, wxALIGN_CENTER_VERTICAL);
    box->Add(bar, 0, wxALL, 6);

    list_ = new PacketList(this, wxID_ANY);
    box->Add(list_, 3, wxEXPAND | wxLEFT | wxRIGHT, 6);

    hex_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 150),
                          wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    hex_->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
    box->Add(hex_, 0, wxEXPAND | wxALL, 6);
    SetSizer(box);

    filter_->Bind(wxEVT_CHOICE, &PacketsPanel::OnFilter, this);
    list_->Bind(wxEVT_LIST_ITEM_SELECTED, &PacketsPanel::OnSelected, this);
}

void PacketsPanel::SetSession(ch10::Session* s) {
    session_ = s;
    filter_->Clear();
    filter_->Append("All channels");
    if (s)
        for (const auto& c : s->channels())
            filter_->Append(wxString::Format("Channel %d - %s%s%s", c.channel_id,
                            TypeShortName(c.data_type),
                            c.source.empty() ? "" : " - ",
                            wxString::FromUTF8(c.source.c_str())));
    filter_->SetSelection(0);
    hex_->Clear();
    last_follow_row_ = -1;
    Rebuild();
}

void PacketsPanel::Rebuild() {
    std::vector<uint32_t> rows;
    if (session_) {
        int sel = filter_->GetSelection();
        if (sel <= 0) {
            rows.resize(session_->packets().size());
            for (uint32_t i = 0; i < rows.size(); ++i) rows[i] = i;
        } else {
            int ch = session_->channels()[sel - 1].channel_id;
            rows = session_->ChannelPackets(ch);
        }
    }
    count_->SetLabel(wxString::Format("%zu packets", rows.size()));
    list_->SetSource(session_, std::move(rows));
}

void PacketsPanel::OnFilter(wxCommandEvent&) { Rebuild(); }

void PacketsPanel::FollowCursor(uint64_t rtc) {
    if (!session_ || !follow_->GetValue() || list_->Count() == 0) return;
    int sel = filter_->GetSelection();
    int ch = sel <= 0 ? -1 : session_->channels()[sel - 1].channel_id;
    std::size_t idx = session_->FindByTime(rtc, ch);
    long row;
    if (idx >= session_->packets().size()) row = list_->Count() - 1;
    else {
        row = list_->RowOfPacket(static_cast<uint32_t>(idx));
        if (row < 0) return;
        if (row > 0 && session_->packets()[idx].rtc > rtc) --row;  // last <= cursor
    }
    if (row == last_follow_row_) return;
    last_follow_row_ = row;
    list_->SetItemState(row, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                        wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    list_->EnsureVisible(row);
}

void PacketsPanel::OnSelected(wxListEvent& ev) {
    if (!session_) return;
    uint32_t i = list_->PacketIndex(ev.GetIndex());
    ch10::Packet p;
    if (!session_->ReadPacket(i, p)) { hex_->SetValue("(could not read packet)"); return; }
    const auto& h = p.header;
    wxString t = wxString::Format(
        "Packet %u  channel %u  %s  seq %u  ver 0x%02X  flags 0x%02X\n"
        "offset 0x%llX  packet_len %u  data_len %u  RTC %llu  %s\n\n",
        i + 1, h.channel_id, TypeShortName(h.data_type), h.seq_num,
        h.data_type_ver, h.flags,
        static_cast<unsigned long long>(p.file_offset), h.packet_len, h.data_len,
        static_cast<unsigned long long>(h.rtc),
        wxString::FromUTF8(session_->TimeString(h.rtc).c_str()));
    const uint8_t* b = p.Body();
    std::size_t n = std::min<std::size_t>(p.BodyLen(), 512);
    for (std::size_t off = 0; off < n; off += 16) {
        t += wxString::Format("%06zX  ", off);
        wxString ascii;
        for (std::size_t k = 0; k < 16; ++k) {
            if (off + k < n) {
                t += wxString::Format("%02X ", b[off + k]);
                char c = static_cast<char>(b[off + k]);
                ascii += (c >= 32 && c < 127) ? wxString(c) : wxString(".");
            } else t += "   ";
            if (k == 7) t += " ";
        }
        t += " " + ascii + "\n";
    }
    if (p.BodyLen() > n) t += wxString::Format("... %zu more bytes\n", p.BodyLen() - n);
    hex_->SetValue(t);
}
