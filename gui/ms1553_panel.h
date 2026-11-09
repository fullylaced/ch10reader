// gui/ms1553_panel.h
// MIL-STD-1553 viewer: channel picker, virtual message table, full decode of
// the selected message, CSV export, and "follow playback" which highlights
// the latest message at the cursor as the recording plays.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>

#include "ch10/session.h"
#include "ch10/wordmap.h"

class MessageList : public wxListCtrl {
public:
    MessageList(wxWindow* parent, wxWindowID id);
    void SetRows(std::vector<ch10::Message1553Row>&& rows);
    const ch10::Message1553Row* Row(long i) const {
        return (i >= 0 && i < static_cast<long>(rows_.size())) ? &rows_[i] : nullptr;
    }
    const std::vector<ch10::Message1553Row>& rows() const { return rows_; }
    long Count() const { return static_cast<long>(rows_.size()); }

protected:
    wxString OnGetItemText(long item, long column) const override;
    wxItemAttr* OnGetItemAttr(long item) const override;

private:
    std::vector<ch10::Message1553Row> rows_;
    mutable wxItemAttr error_attr_;
};

class Ms1553Panel : public wxPanel {
public:
    explicit Ms1553Panel(wxWindow* parent);

    void SetSession(ch10::Session* s);      // nullptr clears
    void FollowCursor(uint64_t rtc);
    void ExportCsv();                       // also reachable from the menu
    void LoadWordMap();                     // pick a CSV (path remembered)
    bool LoadWordMapFile(const wxString& path);

    std::function<void(const wxString&)> on_status;

private:
    void OnLoad(wxCommandEvent&);
    void OnExport(wxCommandEvent&) { ExportCsv(); }
    void OnSelected(wxListEvent&);
    int  SelectedChannel() const;           // -1 = all

    ch10::Session* session_ = nullptr;
    wxChoice*      channel_ = nullptr;
    wxSpinCtrl*    max_     = nullptr;
    wxCheckBox*    time_    = nullptr;
    wxCheckBox*    follow_  = nullptr;
    wxButton*      load_    = nullptr;
    wxButton*      export_  = nullptr;
    wxButton*      wordmap_btn_ = nullptr;
    ch10::WordMap  wordmap_;
    wxString       wordmap_path_;
    wxStaticText*  live_    = nullptr;
    MessageList*   list_    = nullptr;
    wxTextCtrl*    detail_  = nullptr;
    long           last_follow_row_ = -1;
};
