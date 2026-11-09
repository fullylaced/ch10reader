// gui/app.cpp — wxWidgets application entry point for ch10gui.
//
// wxIMPLEMENT_APP expands to main() on Linux and WinMain() on Windows, so
// this one file is the whole entry point on both platforms.

#include <wx/wx.h>
#include <wx/cmdline.h>
#include <wx/config.h>

#include "main_frame.h"
#include "theme.h"

class Ch10App : public wxApp {
public:
    bool OnInit() override {
        if (!wxApp::OnInit()) return false;   // runs the command-line parser
        SetAppName("ch10gui");
        SetVendorName("ch10reader");
        wxConfigBase::Set(wxConfigBase::Create());   // registry on Windows, ~/.config on Linux
        SetCurrentTheme(LoadSavedThemeName());
#if defined(__WXMSW__) && wxCHECK_VERSION(3, 3, 0)
        MSWEnableDarkMode(CurrentTheme().dark ? DarkMode_Always : DarkMode_Never);
#endif
        auto* frame = new MainFrame();
        frame->Show(true);
        // "ch10gui file.ch10" from a shell, shortcut, or file association.
        if (!file_.IsEmpty()) frame->OpenFile(file_);
        return true;
    }

    void OnInitCmdLine(wxCmdLineParser& parser) override {
        wxApp::OnInitCmdLine(parser);
        parser.AddParam("file", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL);
    }

    bool OnCmdLineParsed(wxCmdLineParser& parser) override {
        if (!wxApp::OnCmdLineParsed(parser)) return false;
        if (parser.GetParamCount() > 0) file_ = parser.GetParam(0);
        return true;
    }

private:
    wxString file_;
};

wxIMPLEMENT_APP(Ch10App);
