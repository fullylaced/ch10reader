// gui/tool_dialogs.cpp
#include "tool_dialogs.h"

#include <memory>
#include <sstream>

#include <wx/filename.h>

#include <fstream>

#include "ch10/userdata.h"
#include "theme.h"
#include "timeline_panel.h"   // TypeShortName

namespace {
wxString Utf8(const std::string& s) { return wxString::FromUTF8(s.c_str()); }
std::filesystem::path ToPath(const wxString& s) { return std::filesystem::path(s.ToStdWstring()); }
// wxFilePickerCtrl on GTK drops an initial path that does not exist yet, so
// set it on the picker and its text control explicitly; read back from the
// text control first.
void SetPickerPath(wxFilePickerCtrl* p, const wxString& path) {
    p->SetPath(path);
    if (p->GetTextCtrl()) p->GetTextCtrl()->SetValue(path);
}
wxString PickerPath(wxFilePickerCtrl* p) {
    if (p->GetTextCtrl() && !p->GetTextCtrl()->GetValue().Trim().IsEmpty())
        return p->GetTextCtrl()->GetValue().Trim();
    return p->GetPath();
}
std::set<int> ParseList(const wxString& s) {
    std::set<int> out;
    std::stringstream ss(std::string(s.mb_str()));
    std::string tok;
    while (std::getline(ss, tok, ',')) { long v; if (wxString(tok).Trim().Trim(false).ToLong(&v)) out.insert(static_cast<int>(v)); }
    return out;
}
wxString DefaultOut(const ch10::Session& s, const wxString& suffix) {
    wxFileName f(wxString(s.path().wstring()));
    return wxFileName(f.GetPath(), f.GetName() + suffix + ".ch10").GetFullPath();
}
}

// ===========================================================================
CutDialog::CutDialog(wxWindow* parent, const ch10::Session& s, bool have_range,
                     uint64_t rtc_in, uint64_t rtc_out, std::size_t user_annotations)
    : wxDialog(parent, wxID_ANY, "Cut / filter to a new file", wxDefaultPosition,
               wxSize(760, 620), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      session_(s) {
    auto* box = new wxBoxSizer(wxVERTICAL);

    // Time range
    auto* tbox = new wxStaticBoxSizer(wxHORIZONTAL, this, "Time range (seconds from start)");
    double f0 = have_range ? (rtc_in - s.rtc_min()) / 1e7 : 0.0;
    double f1 = have_range ? (rtc_out - s.rtc_min()) / 1e7 : s.duration_s();
    tbox->Add(new wxStaticText(this, wxID_ANY, "From"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    from_ = new wxTextCtrl(this, wxID_ANY, wxString::Format("%.3f", f0));
    tbox->Add(from_, 1, wxRIGHT, 16);
    tbox->Add(new wxStaticText(this, wxID_ANY, "To"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    to_ = new wxTextCtrl(this, wxID_ANY, wxString::Format("%.3f", f1));
    tbox->Add(to_, 1);
    box->Add(tbox, 0, wxEXPAND | wxALL, 10);

    // Channels
    auto* cbox = new wxStaticBoxSizer(wxVERTICAL, this, "Channels to keep (setup record and time are always kept)");
    wxArrayString names;
    for (const auto& c : s.channels()) {
        if (c.data_type == ch10::kCompGenF1_Tmats || c.data_type == ch10::kTimeF1) continue;
        names.Add(wxString::Format("%d  %s%s%s  (%llu packets)", c.channel_id,
                  TypeShortName(c.data_type), c.source.empty() ? "" : " - ", Utf8(c.source),
                  static_cast<unsigned long long>(c.packets)));
    }
    channels_ = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 140), names);
    for (unsigned i = 0; i < channels_->GetCount(); ++i) channels_->Check(i, true);
    cbox->Add(channels_, 1, wxEXPAND | wxALL, 4);
    box->Add(cbox, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // 1553 filter
    auto* mbox = new wxStaticBoxSizer(wxVERTICAL, this, "MIL-STD-1553 message filter");
    filter_1553_ = new wxCheckBox(this, wxID_ANY, "Keep only messages matching:");
    mbox->Add(filter_1553_, 0, wxALL, 4);
    auto* mgrid = new wxFlexGridSizer(4, 6, 8);
    mgrid->Add(new wxStaticText(this, wxID_ANY, "RT addresses (e.g. 5,7)"), 0, wxALIGN_CENTER_VERTICAL);
    rts_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(140, -1));
    mgrid->Add(rts_, 0);
    mgrid->Add(new wxStaticText(this, wxID_ANY, "Subaddresses"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    sas_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(140, -1));
    mgrid->Add(sas_, 0);
    mgrid->Add(new wxStaticText(this, wxID_ANY, "Direction"), 0, wxALIGN_CENTER_VERTICAL);
    tr_ = new wxChoice(this, wxID_ANY);
    tr_->Append("Any"); tr_->Append("Receive (BC->RT)"); tr_->Append("Transmit (RT->BC)");
    tr_->SetSelection(0);
    mgrid->Add(tr_, 0);
    errors_only_ = new wxCheckBox(this, wxID_ANY, "Errored messages only");
    mgrid->Add(errors_only_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    mbox->Add(mgrid, 0, wxALL, 4);
    box->Add(mbox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    embed_ = new wxCheckBox(this, wxID_ANY,
        wxString::Format("Embed the %zu unsaved annotation(s) as packets", user_annotations));
    embed_->SetValue(user_annotations > 0);
    embed_->Enable(user_annotations > 0);
    box->Add(embed_, 0, wxALL, 10);

    auto* obox = new wxBoxSizer(wxHORIZONTAL);
    obox->Add(new wxStaticText(this, wxID_ANY, "Output"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    out_ = new wxFilePickerCtrl(this, wxID_ANY, DefaultOut(s, "_cut"), "Output file",
                                "Chapter 10 files (*.ch10)|*.ch10", wxDefaultPosition, wxDefaultSize,
                                wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT | wxFLP_USE_TEXTCTRL);
    obox->Add(out_, 1);
    box->Add(obox, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    box->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(box);
    SetPickerPath(out_, DefaultOut(s, "_cut"));
    ApplyTheme(this, nullptr, nullptr);
}

ch10::FilterSpec CutDialog::Spec() const {
    ch10::FilterSpec spec;
    double f0 = 0, f1 = session_.duration_s();
    from_->GetValue().ToCDouble(&f0);
    to_->GetValue().ToCDouble(&f1);
    spec.rtc_from = session_.rtc_min() + static_cast<uint64_t>(std::max(0.0, f0) * 1e7);
    spec.rtc_to   = session_.rtc_min() + static_cast<uint64_t>(std::max(f0, f1) * 1e7);

    unsigned i = 0;
    bool all = true;
    for (const auto& c : session_.channels()) {
        if (c.data_type == ch10::kCompGenF1_Tmats || c.data_type == ch10::kTimeF1) continue;
        if (channels_->IsChecked(i)) spec.channels.insert(c.channel_id); else all = false;
        ++i;
    }
    if (all) spec.channels.clear();
    else if (spec.channels.empty()) spec.channels.insert(-1);   // keep none

    if (filter_1553_->GetValue()) {
        spec.m1553.enabled = true;
        spec.m1553.rts = ParseList(rts_->GetValue());
        spec.m1553.sas = ParseList(sas_->GetValue());
        spec.m1553.tr = tr_->GetSelection() == 0 ? -1 : (tr_->GetSelection() == 1 ? 0 : 1);
        spec.m1553.errors_only = errors_only_->GetValue();
    }
    return spec;
}

std::filesystem::path CutDialog::Output() const { return ToPath(PickerPath(out_)); }

// ===========================================================================
SplitDialog::SplitDialog(wxWindow* parent, const ch10::Session& s)
    : wxDialog(parent, wxID_ANY, "Split into parts", wxDefaultPosition, wxSize(560, 260)) {
    auto* box = new wxBoxSizer(wxVERTICAL);
    auto* grid = new wxFlexGridSizer(2, 10, 10);
    grid->AddGrowableCol(1, 1);
    by_time_ = new wxRadioButton(this, wxID_ANY, "By duration (seconds)", wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    seconds_ = new wxSpinCtrlDouble(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                    wxSP_ARROW_KEYS, 0.1, 1e7, std::max(1.0, s.duration_s() / 4));
    grid->Add(by_time_, 0, wxALIGN_CENTER_VERTICAL); grid->Add(seconds_, 0);
    by_size_ = new wxRadioButton(this, wxID_ANY, "By size (MB)");
    mb_ = new wxSpinCtrlDouble(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                               wxSP_ARROW_KEYS, 0.1, 1e6, std::max(1.0, s.file_size() / 1048576.0 / 4));
    grid->Add(by_size_, 0, wxALIGN_CENTER_VERTICAL); grid->Add(mb_, 0);
    grid->Add(new wxStaticText(this, wxID_ANY, "Output prefix"), 0, wxALIGN_CENTER_VERTICAL);
    wxFileName f(wxString(s.path().wstring()));
    out_ = new wxFilePickerCtrl(this, wxID_ANY, wxFileName(f.GetPath(), f.GetName()).GetFullPath(),
                                "Output prefix (parts get _partNN.ch10)", "*.*",
                                wxDefaultPosition, wxDefaultSize, wxFLP_SAVE | wxFLP_USE_TEXTCTRL);
    grid->Add(out_, 1, wxEXPAND);
    box->Add(grid, 0, wxEXPAND | wxALL, 12);
    box->Add(new wxStaticText(this, wxID_ANY, "Each part starts with the setup record and a time packet."),
             0, wxLEFT | wxRIGHT, 12);
    box->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(box);
    by_time_->SetValue(true);
    SetPickerPath(out_, wxFileName(f.GetPath(), f.GetName()).GetFullPath());
    ApplyTheme(this, nullptr, nullptr);
}

double   SplitDialog::Seconds()  const { return by_time_->GetValue() ? seconds_->GetValue() : 0.0; }
uint64_t SplitDialog::MaxBytes() const { return by_size_->GetValue() ? static_cast<uint64_t>(mb_->GetValue() * 1048576.0) : 0; }
std::filesystem::path SplitDialog::Prefix() const {
    wxFileName f(PickerPath(out_));
    f.ClearExt();
    return ToPath(f.GetFullPath());
}

// ===========================================================================
MergeDialog::MergeDialog(wxWindow* parent, const std::filesystem::path& first)
    : wxDialog(parent, wxID_ANY, "Merge recordings", wxDefaultPosition, wxSize(640, 420),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
    auto* box = new wxBoxSizer(wxVERTICAL);
    box->Add(new wxStaticText(this, wxID_ANY,
        "Files are merged in time order. If every file has a time packet, they are aligned on\n"
        "absolute (IRIG) time; otherwise they are placed one after another. The first file's\n"
        "channel IDs are kept; later files get an offset so IDs do not collide."),
        0, wxALL, 10);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    files_ = new wxListBox(this, wxID_ANY);
    files_->Append(wxString(first.wstring()));
    row->Add(files_, 1, wxEXPAND | wxRIGHT, 8);
    auto* btns = new wxBoxSizer(wxVERTICAL);
    auto* add = new wxButton(this, wxID_ANY, "Add files...");
    auto* rem = new wxButton(this, wxID_ANY, "Remove");
    auto* up  = new wxButton(this, wxID_UP, "Move up");
    auto* dn  = new wxButton(this, wxID_DOWN, "Move down");
    for (auto* b : {add, rem, up, dn}) btns->Add(b, 0, wxEXPAND | wxBOTTOM, 6);
    row->Add(btns, 0);
    box->Add(row, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    auto* orow = new wxBoxSizer(wxHORIZONTAL);
    auto_offsets_ = new wxCheckBox(this, wxID_ANY, "Choose channel offsets automatically");
    auto_offsets_->SetValue(true);
    orow->Add(auto_offsets_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    orow->Add(new wxStaticText(this, wxID_ANY, "or fixed step per file:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    offset_step_ = new wxSpinCtrl(this, wxID_ANY, "100", wxDefaultPosition, wxDefaultSize,
                                  wxSP_ARROW_KEYS, 0, 60000, 100);
    orow->Add(offset_step_, 0);
    box->Add(orow, 0, wxALL, 10);

    auto* obox = new wxBoxSizer(wxHORIZONTAL);
    obox->Add(new wxStaticText(this, wxID_ANY, "Output"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    wxFileName f(wxString(first.wstring()));
    out_ = new wxFilePickerCtrl(this, wxID_ANY, wxFileName(f.GetPath(), f.GetName() + "_merged.ch10").GetFullPath(),
                                "Output file", "Chapter 10 files (*.ch10)|*.ch10", wxDefaultPosition,
                                wxDefaultSize, wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT | wxFLP_USE_TEXTCTRL);
    obox->Add(out_, 1);
    box->Add(obox, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    box->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
    SetSizer(box);

    SetPickerPath(out_, wxFileName(f.GetPath(), f.GetName() + "_merged.ch10").GetFullPath());
    ApplyTheme(this, nullptr, nullptr);
    add->Bind(wxEVT_BUTTON, &MergeDialog::OnAdd, this);
    rem->Bind(wxEVT_BUTTON, &MergeDialog::OnRemove, this);
    up->Bind(wxEVT_BUTTON, &MergeDialog::OnMove, this);
    dn->Bind(wxEVT_BUTTON, &MergeDialog::OnMove, this);
}

void MergeDialog::OnAdd(wxCommandEvent&) {
    wxFileDialog dlg(this, "Add recordings", "", "",
                     "Chapter 10 files (*.ch10;*.c10)|*.ch10;*.c10|All files (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (dlg.ShowModal() != wxID_OK) return;
    wxArrayString paths;
    dlg.GetPaths(paths);
    for (const auto& p : paths) files_->Append(p);
}

void MergeDialog::OnRemove(wxCommandEvent&) {
    int i = files_->GetSelection();
    if (i != wxNOT_FOUND) files_->Delete(static_cast<unsigned>(i));
}

void MergeDialog::OnMove(wxCommandEvent& ev) {
    int i = files_->GetSelection();
    if (i == wxNOT_FOUND) return;
    int j = ev.GetId() == wxID_UP ? i - 1 : i + 1;
    if (j < 0 || j >= static_cast<int>(files_->GetCount())) return;
    wxString a = files_->GetString(static_cast<unsigned>(i));
    files_->SetString(static_cast<unsigned>(i), files_->GetString(static_cast<unsigned>(j)));
    files_->SetString(static_cast<unsigned>(j), a);
    files_->SetSelection(j);
}

std::vector<ch10::MergeInput> MergeDialog::Inputs() const {
    std::vector<ch10::MergeInput> in;
    for (unsigned i = 0; i < files_->GetCount(); ++i)
        in.push_back({ToPath(files_->GetString(i)), 0});
    if (auto_offsets_->GetValue()) {
        std::vector<std::unique_ptr<ch10::Session>> tmp;
        std::vector<ch10::Session*> ptrs;
        for (const auto& m : in) {
            tmp.push_back(std::make_unique<ch10::Session>());
            if (tmp.back()->Open(m.path)) ptrs.push_back(tmp.back().get());
        }
        if (ptrs.size() == in.size()) {
            auto offs = ch10::SuggestChannelOffsets(ptrs);
            for (std::size_t i = 0; i < in.size(); ++i) in[i].channel_offset = offs[i];
        }
    } else {
        for (std::size_t i = 1; i < in.size(); ++i)
            in[i].channel_offset = offset_step_->GetValue() * static_cast<int>(i);
    }
    return in;
}

std::filesystem::path MergeDialog::Output() const { return ToPath(PickerPath(out_)); }

// ===========================================================================
InjectDialog::InjectDialog(wxWindow* parent, const ch10::Session& s)
    : wxDialog(parent, wxID_ANY, "Add user data from a text file", wxDefaultPosition, wxSize(720, 520),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      session_(s) {
    auto* box = new wxBoxSizer(wxVERTICAL);
    box->Add(new wxStaticText(this, wxID_ANY,
        "Notes become annotation packets, text lines become Message Data (F0) packets, and 1553 records\n"
        "become real MIL-STD-1553 messages carrying hex words or ASCII text. New channels are described in\n"
        "the TMATS setup record. The result is written as a new recording."), 0, wxALL, 10);
    auto* frow = new wxBoxSizer(wxHORIZONTAL);
    frow->Add(new wxStaticText(this, wxID_ANY, "Data file"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    file_ = new wxTextCtrl(this, wxID_ANY, "");
    frow->Add(file_, 1, wxRIGHT, 6);
    auto* browse = new wxButton(this, wxID_ANY, "Browse...");
    auto* tmpl = new wxButton(this, wxID_ANY, "Write template...");
    frow->Add(browse, 0, wxRIGHT, 6); frow->Add(tmpl, 0);
    box->Add(frow, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    preview_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                              wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    preview_->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
    box->Add(preview_, 1, wxEXPAND | wxALL, 10);
    auto* obox = new wxBoxSizer(wxHORIZONTAL);
    obox->Add(new wxStaticText(this, wxID_ANY, "Output"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    out_ = new wxFilePickerCtrl(this, wxID_ANY, "", "Output file", "Chapter 10 files (*.ch10)|*.ch10",
                                wxDefaultPosition, wxDefaultSize, wxFLP_SAVE | wxFLP_OVERWRITE_PROMPT | wxFLP_USE_TEXTCTRL);
    obox->Add(out_, 1);
    box->Add(obox, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    auto* btns = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    box->Add(btns, 0, wxEXPAND | wxALL, 8);
    SetSizer(box);
    ok_ = static_cast<wxButton*>(FindWindow(wxID_OK));
    ok_->Enable(false);
    SetPickerPath(out_, DefaultOut(s, "_userdata"));
    browse->Bind(wxEVT_BUTTON, &InjectDialog::OnBrowse, this);
    tmpl->Bind(wxEVT_BUTTON, &InjectDialog::OnTemplate, this);
    file_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { Preview(); });
    ApplyTheme(this, nullptr, nullptr);
}

void InjectDialog::OnBrowse(wxCommandEvent&) {
    wxFileDialog dlg(this, "User data file", "", "", "Text files (*.txt;*.csv)|*.txt;*.csv|All files (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) file_->SetValue(dlg.GetPath());
}

void InjectDialog::OnTemplate(wxCommandEvent&) {
    wxFileName base(wxString(session_.path().wstring()));
    wxFileDialog dlg(this, "Write user data template", base.GetPath(), base.GetName() + "_userdata.txt",
                     "Text files (*.txt)|*.txt", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    std::ofstream f(ToPath(dlg.GetPath()), std::ios::binary);
    f << ch10::UserData::Template();
    f.close();
    file_->SetValue(dlg.GetPath());
}

void InjectDialog::Preview() {
    wxString path = file_->GetValue().Trim();
    if (path.IsEmpty() || !wxFileName::FileExists(path)) { preview_->SetValue(""); ok_->Enable(false); return; }
    ch10::UserDataParse up = ch10::UserData::ParseFile(std::string(path.ToUTF8()), session_);
    wxString t = wxString::Format("%d notes, %d text messages, %d 1553 messages parsed\n\n", up.notes, up.texts, up.messages1553);
    for (const auto& e : up.errors) t += "ERROR " + Utf8(e) + "\n";
    if (!up.errors.empty()) t += "\n";
    int shown = 0;
    for (const auto& r : up.records) {
        if (shown++ >= 40) { t += "...\n"; break; }
        const char* kind = r.kind == ch10::UserRecord::Note ? "note" : r.kind == ch10::UserRecord::Text ? "text" : "1553";
        t += wxString::Format("%s  %-5s ch%-5d %s\n", Utf8(session_.RelativeString(r.rtc)), kind,
                              r.channel, Utf8(r.kind == ch10::UserRecord::Ms1553
                                              ? (r.text.empty() ? std::to_string(r.words.size()) + " hex words" : "\"" + r.text + "\"")
                                              : r.text));
    }
    preview_->SetValue(t);
    ok_->Enable(!up.records.empty());
}

std::filesystem::path InjectDialog::InputFile() const { return ToPath(file_->GetValue().Trim()); }
std::filesystem::path InjectDialog::Output() const { return ToPath(PickerPath(out_)); }
