// gui/ethernet_panel.h
// Ethernet viewer: frames of a channel with parsed MAC/IP/UDP/TCP headers,
// a hex + ASCII dump of the selected payload, follow-playback, PCAP export.
// Also hosts the Messages view (Message Data F0 packets as readable text).

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>

#include "ch10/ethernet.h"
#include "ch10/message_f0.h"
#include "ch10/session.h"

struct EthRow {
    uint64_t rtc = 0;
    int      channel = 0, netid = 0, length = 0;
    wxString proto, src, dst, info, errors;
    std::vector<uint8_t> payload;    // innermost payload
    std::vector<uint8_t> frame;      // whole frame (for the dump)
};

class EthList : public wxListCtrl {
public:
    EthList(wxWindow* parent);
    void SetRows(std::vector<EthRow>&& rows, const ch10::Session* s);
    const EthRow* Row(long i) const { return (i >= 0 && i < static_cast<long>(rows_.size())) ? &rows_[i] : nullptr; }
    const std::vector<EthRow>& rows() const { return rows_; }
protected:
    wxString OnGetItemText(long item, long column) const override;
private:
    std::vector<EthRow> rows_;
    const ch10::Session* session_ = nullptr;
};

class EthernetPanel : public wxPanel {
public:
    explicit EthernetPanel(wxWindow* parent);
    void SetSession(ch10::Session* s);
    void FollowCursor(uint64_t rtc);
    void ExportPcap();
    std::function<void(const wxString&)> on_status;
private:
    void OnLoad(wxCommandEvent&);
    void OnSelected(wxListEvent&);
    int  SelectedChannel() const;
    ch10::Session* session_ = nullptr;
    wxChoice* channel_ = nullptr;
    wxSpinCtrl* max_ = nullptr;
    wxCheckBox* follow_ = nullptr;
    wxButton* load_ = nullptr;
    wxButton* pcap_ = nullptr;
    EthList* list_ = nullptr;
    wxTextCtrl* detail_ = nullptr;
    long last_follow_ = -1;
};

struct MsgRow { uint64_t rtc = 0; int channel = 0, subchannel = 0; bool errors = false; ch10::Message msg; };

class MsgList : public wxListCtrl {
public:
    MsgList(wxWindow* parent);
    void SetRows(std::vector<MsgRow>&& rows, const ch10::Session* s);
    const MsgRow* Row(long i) const { return (i >= 0 && i < static_cast<long>(rows_.size())) ? &rows_[i] : nullptr; }
    const std::vector<MsgRow>& rows() const { return rows_; }
protected:
    wxString OnGetItemText(long item, long column) const override;
private:
    std::vector<MsgRow> rows_;
    const ch10::Session* session_ = nullptr;
};

class MessagesPanel : public wxPanel {
public:
    explicit MessagesPanel(wxWindow* parent);
    void SetSession(ch10::Session* s);
    void FollowCursor(uint64_t rtc);
    std::function<void(const wxString&)> on_status;
private:
    void OnLoad(wxCommandEvent&);
    void OnSelected(wxListEvent&);
    int  SelectedChannel() const;
    ch10::Session* session_ = nullptr;
    wxChoice* channel_ = nullptr;
    wxCheckBox* follow_ = nullptr;
    wxButton* load_ = nullptr;
    MsgList* list_ = nullptr;
    wxTextCtrl* detail_ = nullptr;
    long last_follow_ = -1;
};

// Hex + ASCII dump helper shared by panels.
wxString HexDump(const uint8_t* data, std::size_t len, std::size_t max_bytes = 2048);
