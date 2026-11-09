// gui/theme.h
// Colour themes. wxWidgets 3.2 draws native controls in the OS colours, so a
// theme is applied by (1) recolouring every control we can, (2) giving the
// AUI dock and tab art our palette, and (3) letting the custom-drawn panels
// (timeline, video) read the palette when they paint. On Windows with
// wxWidgets 3.3+ the native dark mode is switched on as well so menus, title
// bar and scrollbars follow.

#pragma once

#include <vector>

#include <wx/wx.h>
#include <wx/aui/aui.h>

struct Theme {
    wxString name;
    bool     dark = true;
    wxColour window;        // frame / pane background
    wxColour panel;         // slightly lighter panel surface
    wxColour surface;       // list, text, input backgrounds
    wxColour text;
    wxColour dim;           // secondary text
    wxColour accent;        // cursor, highlights
    wxColour grid;          // separators, grid lines
    wxColour selection;
    wxColour caption;       // AUI pane caption
    wxColour caption_text;
    wxColour canvas;        // timeline / video background
    wxColour canvas_row;    // timeline row background
};

const std::vector<Theme>& Themes();
const Theme& CurrentTheme();
void SetCurrentTheme(const wxString& name);   // persisted with wxConfig
wxString LoadSavedThemeName();

// Recolour `root` and all descendants, plus AUI manager / notebook art.
void ApplyTheme(wxWindow* root, wxAuiManager* mgr, wxAuiNotebook* notebook);

// Flat tab renderer whose colours come from the theme (the stock art uses
// system colours for text, which is unreadable on a dark base).
class ThemedTabArt : public wxAuiGenericTabArt {
public:
    ThemedTabArt() = default;
    wxAuiTabArt* Clone() override { return new ThemedTabArt(*this); }
    void DrawBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect) override;
    void DrawTab(wxDC& dc, wxWindow* wnd, const wxAuiNotebookPage& page,
                 const wxRect& in_rect, int close_button_state,
                 wxRect* out_tab_rect, wxRect* out_button_rect, int* x_extent) override;
};
