// gui/annotations_panel.cpp
#include "annotations_panel.h"

#include <algorithm>

#include "theme.h"
#include "timeline_panel.h"   // SeverityColour

using ch10::Annotation;
using ch10::Severity;

namespace {
wxString Utf8(const std::string& s) { return wxString::FromUTF8(s.c_str()); }
double RelSeconds(const ch10::Session& s, uint64_t rtc) {
    return (static_cast<double>(rtc) - static_cast<double>(s.rtc_min())) / 1e7;
}
const char* SourceName(ch10::AnnotationSource src) {
    switch (src) {
        case ch10::AnnotationSource::User:  return "this session";
        case ch10::AnnotationSource::File:  return "file";
        case ch10::AnnotationSource::Event: return "recording event";
        case ch10::AnnotationSource::Auto:  return "detected";
    }
    return "";
}
} // namespace

// ===========================================================================
AnnotationDialog::AnnotationDialog(wxWindow* parent, const ch10::Session& s, Annotation initial)
    : wxDialog(parent, wxID_ANY, "Annotation", wxDefaultPosition, wxSize(520, 380),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      session_(s), initial_(initial) {
    auto* box = new wxBoxSizer(wxVERTICAL);
    auto* grid = new wxFlexGridSizer(2, 8, 10);
    grid->AddGrowableCol(1, 1);

    grid->Add(new wxStaticText(this, wxID_ANY, "Start (s)"), 0, wxALIGN_CENTER_VERTICAL);
    start_ = new wxTextCtrl(this, wxID_ANY, wxString::Format("%.3f", RelSeconds(s, initial.rtc_start)));
    grid->Add(start_, 1, wxEXPAND);
    grid->Add(new wxStaticText(this, wxID_ANY, "End (s, blank = point)"), 0, wxALIGN_CENTER_VERTICAL);
    end_ = new wxTextCtrl(this, wxID_ANY, initial.IsRange()
                          ? wxString::Format("%.3f", RelSeconds(s, initial.rtc_end)) : wxString());
    grid->Add(end_, 1, wxEXPAND);
    grid->Add(new wxStaticText(this, wxID_ANY, "Severity"), 0, wxALIGN_CENTER_VERTICAL);
    severity_ = new wxChoice(this, wxID_ANY);
    for (int i = 0; i < 4; ++i) severity_->Append(ch10::SeverityName(static_cast<Severity>(i)));
    severity_->SetSelection(static_cast<int>(initial.severity));
    grid->Add(severity_, 0);
    grid->Add(new wxStaticText(this, wxID_ANY, "Channel"), 0, wxALIGN_CENTER_VERTICAL);
    channel_ = new wxChoice(this, wxID_ANY);
    channel_->Append("Whole recording");
    int sel = 0, i = 1;
    for (const auto& c : s.channels()) {
        channel_->Append(wxString::Format("Channel %d - %s%s%s", c.channel_id,
                         TypeShortName(c.data_type), c.source.empty() ? "" : " - ", Utf8(c.source)));
        if (c.channel_id == initial.channel) sel = i;
        ++i;
    }
    channel_->SetSelection(sel);
    grid->Add(channel_, 0);
    box->Add(grid, 0, wxEXPAND | wxALL, 12);

    box->Add(new wxStaticText(this, wxID_ANY, "Text"), 0, wxLEFT | wxRIGHT, 12);
    text_ = new wxTextCtrl(this, wxID_ANY, Utf8(initial.text), wxDefaultPosition,
                           wxDefaultSize, wxTE_MULTILINE);
    box->Add(text_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    box->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(box);
    ApplyTheme(this, nullptr, nullptr);
    text_->SetFocus();
}

Annotation AnnotationDialog::Result() const {
    Annotation a = initial_;
    double s0 = 0, s1 = 0;
    start_->GetValue().ToCDouble(&s0);
    bool has_end = !end_->GetValue().Trim().IsEmpty() && end_->GetValue().ToCDouble(&s1);
    a.rtc_start = session_.rtc_min() + static_cast<uint64_t>(std::max(0.0, s0) * 1e7);
    a.rtc_end   = has_end ? session_.rtc_min() + static_cast<uint64_t>(std::max(s0, s1) * 1e7)
                          : a.rtc_start;
    a.severity = static_cast<Severity>(severity_->GetSelection());
    int cs = channel_->GetSelection();
    a.channel = cs <= 0 ? -1 : session_.channels()[cs - 1].channel_id;
    a.text = text_->GetValue().ToUTF8().data();
    a.source = ch10::AnnotationSource::User;
    return a;
}

// ===========================================================================
AnnotationList::AnnotationList(wxWindow* parent, wxWindowID id)
    : wxListCtrl(parent, id, wxDefaultPosition, wxDefaultSize,
                 wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL | wxLC_HRULES) {
    AppendColumn("Time",     wxLIST_FORMAT_LEFT, 110);
    AppendColumn("Absolute", wxLIST_FORMAT_LEFT, 165);
    AppendColumn("Length",   wxLIST_FORMAT_LEFT, 80);
    AppendColumn("Kind",     wxLIST_FORMAT_LEFT, 90);
    AppendColumn("Ch",       wxLIST_FORMAT_RIGHT, 50);
    AppendColumn("Source",   wxLIST_FORMAT_LEFT, 120);
    AppendColumn("Text",     wxLIST_FORMAT_LEFT, 600);
    anomaly_attr_.SetTextColour(SeverityColour(Severity::Anomaly));
    user_attr_.SetFont(GetFont().Bold());
}

void AnnotationList::SetRows(std::vector<Row>&& rows, const ch10::Session* s) {
    rows_ = std::move(rows);
    session_ = s;
    SetItemCount(static_cast<long>(rows_.size()));
    Refresh();
}

wxItemAttr* AnnotationList::OnGetItemAttr(long item) const {
    const Row* r = RowAt(item);
    if (!r) return nullptr;
    if (r->anomaly) return &anomaly_attr_;
    if (r->a->source == ch10::AnnotationSource::User) return &user_attr_;
    return nullptr;
}

wxString AnnotationList::OnGetItemText(long item, long col) const {
    const Row* r = RowAt(item);
    if (!r || !session_) return wxEmptyString;
    const Annotation& a = *r->a;
    switch (col) {
        case 0: return Utf8(session_->RelativeString(a.rtc_start));
        case 1: return Utf8(session_->TimeString(a.rtc_start));
        case 2: return a.IsRange() ? wxString::Format("%.3f s", (a.rtc_end - a.rtc_start) / 1e7) : wxString("point");
        case 3: return r->anomaly ? "anomaly" : ch10::SeverityName(a.severity);
        case 4: return a.channel < 0 ? wxString("all") : wxString::Format("%d", a.channel);
        case 5: return SourceName(a.source);
        case 6: return Utf8(a.text);
        default: return wxEmptyString;
    }
}

// ===========================================================================
AnnotationsPanel::AnnotationsPanel(wxWindow* parent) : wxPanel(parent) {
    auto* box = new wxBoxSizer(wxVERTICAL);
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    bar->Add(new wxStaticText(this, wxID_ANY, "Show:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    filter_ = new wxChoice(this, wxID_ANY);
    filter_->Append("Everything");
    filter_->Append("Annotations only");
    filter_->Append("Anomalies only");
    filter_->SetSelection(0);
    bar->Add(filter_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    add_ = new wxButton(this, wxID_ANY, "Add at cursor");
    add_range_ = new wxButton(this, wxID_ANY, "Add In-Out range");
    edit_ = new wxButton(this, wxID_ANY, "Edit");
    del_ = new wxButton(this, wxID_ANY, "Delete");
    promote_ = new wxButton(this, wxID_ANY, "Keep anomaly");
    save_ = new wxButton(this, wxID_ANY, "Save to file copy...");
    for (auto* b : {add_, add_range_, edit_, del_, promote_, save_}) bar->Add(b, 0, wxRIGHT, 6);
    count_ = new wxStaticText(this, wxID_ANY, "");
    bar->Add(count_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    box->Add(bar, 0, wxALL, 6);

    list_ = new AnnotationList(this, wxID_ANY);
    box->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    SetSizer(box);

    add_->Bind(wxEVT_BUTTON, &AnnotationsPanel::OnAdd, this);
    add_range_->Bind(wxEVT_BUTTON, &AnnotationsPanel::OnAddRange, this);
    edit_->Bind(wxEVT_BUTTON, &AnnotationsPanel::OnEdit, this);
    del_->Bind(wxEVT_BUTTON, &AnnotationsPanel::OnDelete, this);
    promote_->Bind(wxEVT_BUTTON, &AnnotationsPanel::OnPromote, this);
    save_->Bind(wxEVT_BUTTON, &AnnotationsPanel::OnSave, this);
    filter_->Bind(wxEVT_CHOICE, &AnnotationsPanel::OnFilter, this);
    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, &AnnotationsPanel::OnActivated, this);
    list_->Bind(wxEVT_LIST_ITEM_SELECTED, &AnnotationsPanel::OnSelected, this);
    list_->Bind(wxEVT_LIST_ITEM_DESELECTED, &AnnotationsPanel::OnSelected, this);
    UpdateButtons();
}

void AnnotationsPanel::SetSession(ch10::Session* s) {
    session_ = s;
    Rebuild();
}

void AnnotationsPanel::Rebuild() {
    std::vector<AnnotationList::Row> rows;
    if (session_) {
        int f = filter_->GetSelection();
        if (f != 2)
            for (std::size_t i = 0; i < session_->annotations().size(); ++i)
                rows.push_back({&session_->annotations()[i], false, i});
        if (f != 1)
            for (std::size_t i = 0; i < session_->anomalies().size(); ++i)
                rows.push_back({&session_->anomalies()[i], true, i});
        std::stable_sort(rows.begin(), rows.end(),
                         [](const AnnotationList::Row& a, const AnnotationList::Row& b) {
                             return a.a->rtc_start < b.a->rtc_start;
                         });
        std::size_t users = session_->UserAnnotations().size();
        count_->SetLabel(wxString::Format("%zu annotations (%zu unsaved), %llu anomalies",
                         session_->annotations().size(), users,
                         static_cast<unsigned long long>(session_->anomaly_total())));
    } else {
        count_->SetLabel("");
    }
    list_->SetRows(std::move(rows), session_);
    UpdateButtons();
}

const AnnotationList::Row* AnnotationsPanel::Selected() const {
    long i = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return list_->RowAt(i);
}

void AnnotationsPanel::UpdateButtons() {
    bool have = session_ != nullptr;
    const auto* r = Selected();
    add_->Enable(have);
    add_range_->Enable(have && range && [this] { uint64_t a, b; return range(a, b); }());
    edit_->Enable(have && r && !r->anomaly && r->a->source == ch10::AnnotationSource::User);
    del_->Enable(edit_->IsEnabled());
    promote_->Enable(have && r && r->anomaly);
    save_->Enable(have && !session_->UserAnnotations().empty());
}

void AnnotationsPanel::AddAtCursor() {
    if (!session_) return;
    Annotation a;
    a.rtc_start = a.rtc_end = cursor ? cursor() : session_->rtc_min();
    AnnotationDialog dlg(this, *session_, a);
    if (dlg.ShowModal() != wxID_OK) return;
    session_->AddAnnotation(dlg.Result());
    Rebuild();
    if (on_changed) on_changed();
}

void AnnotationsPanel::OnAdd(wxCommandEvent&) { AddAtCursor(); }

void AnnotationsPanel::OnAddRange(wxCommandEvent&) {
    if (!session_ || !range) return;
    Annotation a;
    if (!range(a.rtc_start, a.rtc_end)) return;
    AnnotationDialog dlg(this, *session_, a);
    if (dlg.ShowModal() != wxID_OK) return;
    session_->AddAnnotation(dlg.Result());
    Rebuild();
    if (on_changed) on_changed();
}

void AnnotationsPanel::OnEdit(wxCommandEvent&) {
    const auto* r = Selected();
    if (!session_ || !r || r->anomaly || r->a->source != ch10::AnnotationSource::User) return;
    AnnotationDialog dlg(this, *session_, *r->a);
    if (dlg.ShowModal() != wxID_OK) return;
    Annotation edited = dlg.Result();
    session_->RemoveAnnotation(r->index);
    session_->AddAnnotation(edited);
    Rebuild();
    if (on_changed) on_changed();
}

void AnnotationsPanel::OnDelete(wxCommandEvent&) {
    const auto* r = Selected();
    if (!session_ || !r || r->anomaly) return;
    if (!session_->RemoveAnnotation(r->index)) {
        wxMessageBox("Only annotations added in this session can be deleted here. Annotations "
                     "stored in the file are packets; use Cut/Filter to drop that channel.",
                     "Delete annotation", wxOK | wxICON_INFORMATION, this);
        return;
    }
    Rebuild();
    if (on_changed) on_changed();
}

void AnnotationsPanel::OnPromote(wxCommandEvent&) {
    const auto* r = Selected();
    if (!session_ || !r || !r->anomaly) return;
    Annotation a = *r->a;
    a.source = ch10::AnnotationSource::User;
    a.severity = Severity::Anomaly;
    AnnotationDialog dlg(this, *session_, a);
    if (dlg.ShowModal() != wxID_OK) return;
    session_->AddAnnotation(dlg.Result());
    Rebuild();
    if (on_changed) on_changed();
}

void AnnotationsPanel::OnActivated(wxListEvent& ev) {
    const auto* r = list_->RowAt(ev.GetIndex());
    if (r && on_seek) on_seek(r->a->rtc_start);
}

void AnnotationsPanel::OnSelected(wxListEvent&) { UpdateButtons(); }
