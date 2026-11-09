// gui/packets_panel.h
// Every packet in the file: a virtual list over Session::packets() with a
// channel filter, a hex dump of the selected packet's body, and an optional
// "follow playback" mode that keeps the packet at the cursor in view.

#pragma once

#include <cstdint>
#include <vector>

#include <wx/wx.h>
#include <wx/listctrl.h>

#include "ch10/session.h"

class PacketList : public wxListCtrl {
public:
    PacketList(wxWindow* parent, wxWindowID id);
    void SetSource(ch10::Session* s, std::vector<uint32_t> rows);
    uint32_t PacketIndex(long row) const { return rows_[row]; }
    long RowOfPacket(uint32_t index) const;   // -1 if filtered out
    long Count() const { return static_cast<long>(rows_.size()); }

protected:
    wxString OnGetItemText(long item, long column) const override;

private:
    ch10::Session*        session_ = nullptr;
    std::vector<uint32_t> rows_;        // packet indices, file order
};

class PacketsPanel : public wxPanel {
public:
    explicit PacketsPanel(wxWindow* parent);

    void SetSession(ch10::Session* s);        // nullptr clears
    void FollowCursor(uint64_t rtc);          // no-op unless "follow" is on

private:
    void OnFilter(wxCommandEvent&);
    void OnSelected(wxListEvent&);
    void Rebuild();

    ch10::Session* session_ = nullptr;
    wxChoice*      filter_  = nullptr;
    wxCheckBox*    follow_  = nullptr;
    wxStaticText*  count_   = nullptr;
    PacketList*    list_    = nullptr;
    wxTextCtrl*    hex_     = nullptr;
    long           last_follow_row_ = -1;
};
