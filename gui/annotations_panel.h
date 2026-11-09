// gui/annotations_panel.h
// Lists annotations (from the file, recording events, and ones added in this
// session) together with automatically detected anomalies. Add / edit /
// delete user annotations, promote an anomaly to a saved annotation, jump to
// any entry, and hand the user annotations to the file-copy operation.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <wx/wx.h>
#include <wx/listctrl.h>

#include "ch10/annotation.h"
#include "ch10/session.h"

// Modal editor for one annotation. Times are entered in seconds from the
// start of the recording.
class AnnotationDialog : public wxDialog {
public:
    AnnotationDialog(wxWindow* parent, const ch10::Session& s, ch10::Annotation initial);
    ch10::Annotation Result() const;
private:
    const ch10::Session& session_;
    wxTextCtrl* text_ = nullptr;
    wxChoice*   severity_ = nullptr;
    wxChoice*   channel_ = nullptr;
    wxTextCtrl* start_ = nullptr;
    wxTextCtrl* end_ = nullptr;
    ch10::Annotation initial_;
};

class AnnotationList : public wxListCtrl {
public:
    struct Row { const ch10::Annotation* a; bool anomaly; std::size_t index; };
    AnnotationList(wxWindow* parent, wxWindowID id);
    void SetRows(std::vector<Row>&& rows, const ch10::Session* s);
    const Row* RowAt(long i) const { return (i >= 0 && i < static_cast<long>(rows_.size())) ? &rows_[i] : nullptr; }
    long Count() const { return static_cast<long>(rows_.size()); }
protected:
    wxString OnGetItemText(long item, long column) const override;
    wxItemAttr* OnGetItemAttr(long item) const override;
private:
    std::vector<Row> rows_;
    const ch10::Session* session_ = nullptr;
    mutable wxItemAttr anomaly_attr_, user_attr_;
};

class AnnotationsPanel : public wxPanel {
public:
    explicit AnnotationsPanel(wxWindow* parent);

    void SetSession(ch10::Session* s);   // nullptr clears
    void Rebuild();                      // after annotations changed elsewhere
    void AddAtCursor();                  // same as the button

    std::function<uint64_t()>                 cursor;          // current playback time
    std::function<bool(uint64_t&, uint64_t&)> range;           // In/Out if set
    std::function<void(uint64_t)>             on_seek;
    std::function<void()>                     on_changed;      // user annotations edited
    std::function<void()>                     on_save_request; // "Save into file copy..."
    std::function<void(const wxString&)>      on_status;

private:
    void OnAdd(wxCommandEvent&);
    void OnAddRange(wxCommandEvent&);
    void OnEdit(wxCommandEvent&);
    void OnDelete(wxCommandEvent&);
    void OnPromote(wxCommandEvent&);
    void OnSave(wxCommandEvent&) { if (on_save_request) on_save_request(); }
    void OnFilter(wxCommandEvent&) { Rebuild(); }
    void OnActivated(wxListEvent&);
    void OnSelected(wxListEvent&);
    const AnnotationList::Row* Selected() const;
    void UpdateButtons();

    ch10::Session*  session_ = nullptr;
    wxChoice*       filter_  = nullptr;
    wxButton*       add_ = nullptr;
    wxButton*       add_range_ = nullptr;
    wxButton*       edit_ = nullptr;
    wxButton*       del_ = nullptr;
    wxButton*       promote_ = nullptr;
    wxButton*       save_ = nullptr;
    wxStaticText*   count_ = nullptr;
    AnnotationList* list_ = nullptr;
};
