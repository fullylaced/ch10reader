// gui/tool_dialogs.h
// Dialogs for the file operations: Cut/Filter, Split, Merge.

#pragma once

#include <filesystem>
#include <vector>

#include <wx/wx.h>
#include <wx/checklst.h>
#include <wx/filepicker.h>
#include <wx/spinctrl.h>

#include "ch10/ops.h"
#include "ch10/session.h"

class CutDialog : public wxDialog {
public:
    CutDialog(wxWindow* parent, const ch10::Session& s, bool have_range,
              uint64_t rtc_in, uint64_t rtc_out, std::size_t user_annotations);
    ch10::FilterSpec Spec() const;
    std::filesystem::path Output() const;
    bool EmbedAnnotations() const { return embed_ && embed_->GetValue(); }
private:
    const ch10::Session& session_;
    wxTextCtrl* from_ = nullptr;
    wxTextCtrl* to_ = nullptr;
    wxCheckListBox* channels_ = nullptr;
    wxCheckBox* filter_1553_ = nullptr;
    wxTextCtrl* rts_ = nullptr;
    wxTextCtrl* sas_ = nullptr;
    wxChoice*   tr_ = nullptr;
    wxCheckBox* errors_only_ = nullptr;
    wxCheckBox* embed_ = nullptr;
    wxFilePickerCtrl* out_ = nullptr;
};

class SplitDialog : public wxDialog {
public:
    SplitDialog(wxWindow* parent, const ch10::Session& s);
    double   Seconds() const;      // 0 if splitting by size
    uint64_t MaxBytes() const;     // 0 if splitting by time
    std::filesystem::path Prefix() const;
private:
    wxRadioButton* by_time_ = nullptr;
    wxRadioButton* by_size_ = nullptr;
    wxSpinCtrlDouble* seconds_ = nullptr;
    wxSpinCtrlDouble* mb_ = nullptr;
    wxFilePickerCtrl* out_ = nullptr;
};

class InjectDialog : public wxDialog {
public:
    InjectDialog(wxWindow* parent, const ch10::Session& s);
    std::filesystem::path InputFile() const;
    std::filesystem::path Output() const;
private:
    void OnBrowse(wxCommandEvent&);
    void OnTemplate(wxCommandEvent&);
    void Preview();
    const ch10::Session& session_;
    wxTextCtrl* file_ = nullptr;
    wxTextCtrl* preview_ = nullptr;
    wxFilePickerCtrl* out_ = nullptr;
    wxButton* ok_ = nullptr;
};

class MergeDialog : public wxDialog {
public:
    MergeDialog(wxWindow* parent, const std::filesystem::path& first);
    std::vector<ch10::MergeInput> Inputs() const;   // offsets filled in
    std::filesystem::path Output() const;
private:
    void OnAdd(wxCommandEvent&);
    void OnRemove(wxCommandEvent&);
    void OnMove(wxCommandEvent&);
    wxListBox* files_ = nullptr;
    wxCheckBox* auto_offsets_ = nullptr;
    wxSpinCtrl* offset_step_ = nullptr;
    wxFilePickerCtrl* out_ = nullptr;
};
