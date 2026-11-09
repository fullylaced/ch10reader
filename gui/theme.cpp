// gui/theme.cpp
#include "theme.h"

#include <wx/config.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>

namespace {

std::vector<Theme> BuildThemes() {
    std::vector<Theme> v;
    Theme dark;
    dark.name = "Dark";  dark.dark = true;
    dark.window   = wxColour(30, 32, 38);
    dark.panel    = wxColour(38, 41, 48);
    dark.surface  = wxColour(24, 26, 31);
    dark.text     = wxColour(222, 226, 234);
    dark.dim      = wxColour(140, 146, 158);
    dark.accent   = wxColour(88, 166, 255);
    dark.grid     = wxColour(58, 62, 72);
    dark.selection= wxColour(50, 90, 140);
    dark.caption  = wxColour(44, 48, 58);
    dark.caption_text = wxColour(200, 205, 215);
    dark.canvas   = wxColour(22, 24, 29);
    dark.canvas_row = wxColour(36, 39, 46);
    v.push_back(dark);

    Theme midnight = dark;
    midnight.name = "Midnight";
    midnight.window   = wxColour(14, 20, 34);
    midnight.panel    = wxColour(20, 28, 46);
    midnight.surface  = wxColour(10, 15, 27);
    midnight.text     = wxColour(214, 224, 240);
    midnight.dim      = wxColour(120, 136, 168);
    midnight.accent   = wxColour(110, 200, 255);
    midnight.grid     = wxColour(38, 52, 82);
    midnight.selection= wxColour(30, 80, 150);
    midnight.caption  = wxColour(24, 36, 62);
    midnight.canvas   = wxColour(8, 12, 22);
    midnight.canvas_row = wxColour(18, 26, 44);
    v.push_back(midnight);

    Theme light;
    light.name = "Light";  light.dark = false;
    light.window   = wxColour(240, 241, 244);
    light.panel    = wxColour(248, 249, 251);
    light.surface  = wxColour(255, 255, 255);
    light.text     = wxColour(28, 30, 36);
    light.dim      = wxColour(110, 116, 128);
    light.accent   = wxColour(30, 110, 220);
    light.grid     = wxColour(210, 214, 222);
    light.selection= wxColour(190, 215, 250);
    light.caption  = wxColour(222, 226, 234);
    light.caption_text = wxColour(40, 44, 52);
    light.canvas   = wxColour(250, 250, 252);
    light.canvas_row = wxColour(232, 235, 242);
    v.push_back(light);
    return v;
}

const Theme* g_current = nullptr;

void Recolour(wxWindow* w, const Theme& t) {
    if (auto* lc = dynamic_cast<wxListCtrl*>(w)) {
        lc->SetBackgroundColour(t.surface);
        lc->SetTextColour(t.text);
    } else if (auto* tc = dynamic_cast<wxTextCtrl*>(w)) {
        tc->SetBackgroundColour(t.surface);
        tc->SetForegroundColour(t.text);
    } else if (dynamic_cast<wxButton*>(w) || dynamic_cast<wxChoice*>(w) ||
               dynamic_cast<wxSpinCtrl*>(w) || dynamic_cast<wxSlider*>(w)) {
        w->SetBackgroundColour(t.panel);
        w->SetForegroundColour(t.text);
    } else if (dynamic_cast<wxCheckBox*>(w) || dynamic_cast<wxStaticText*>(w)) {
        w->SetBackgroundColour(t.window);
        w->SetForegroundColour(t.text);
    } else if (dynamic_cast<wxToolBar*>(w) || dynamic_cast<wxStatusBar*>(w)) {
        w->SetBackgroundColour(t.window);
        w->SetForegroundColour(t.text);
    } else if (dynamic_cast<wxAuiNotebook*>(w)) {
        w->SetBackgroundColour(t.window);
    } else if (dynamic_cast<wxPanel*>(w) || dynamic_cast<wxScrolledWindow*>(w)) {
        w->SetBackgroundColour(t.window);
        w->SetForegroundColour(t.text);
    }
    for (wxWindow* c : w->GetChildren()) Recolour(c, t);
}

} // namespace

const std::vector<Theme>& Themes() {
    static std::vector<Theme> themes = BuildThemes();
    return themes;
}

const Theme& CurrentTheme() {
    if (!g_current) g_current = &Themes().front();
    return *g_current;
}

wxString LoadSavedThemeName() {
    wxString name;
    if (wxConfigBase* cfg = wxConfigBase::Get(true))
        cfg->Read("Theme", &name, "Dark");
    return name.IsEmpty() ? "Dark" : name;
}

void SetCurrentTheme(const wxString& name) {
    for (const Theme& t : Themes())
        if (t.name == name) g_current = &t;
    if (!g_current) g_current = &Themes().front();
    if (wxConfigBase* cfg = wxConfigBase::Get(true)) {
        cfg->Write("Theme", CurrentTheme().name);
        cfg->Flush();
    }
}

void ApplyTheme(wxWindow* root, wxAuiManager* mgr, wxAuiNotebook* notebook) {
    const Theme& t = CurrentTheme();
    root->SetBackgroundColour(t.window);
    root->SetForegroundColour(t.text);
    Recolour(root, t);

    if (mgr) {
        wxAuiDockArt* art = mgr->GetArtProvider();
        art->SetColour(wxAUI_DOCKART_BACKGROUND_COLOUR, t.window);
        art->SetColour(wxAUI_DOCKART_SASH_COLOUR, t.window);
        art->SetColour(wxAUI_DOCKART_BORDER_COLOUR, t.grid);
        art->SetColour(wxAUI_DOCKART_GRIPPER_COLOUR, t.grid);
        art->SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_COLOUR, t.caption);
        art->SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_GRADIENT_COLOUR, t.caption);
        art->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_COLOUR, t.caption);
        art->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_GRADIENT_COLOUR, t.caption);
        art->SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_TEXT_COLOUR, t.caption_text);
        art->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_TEXT_COLOUR, t.caption_text);
        art->SetMetric(wxAUI_DOCKART_GRADIENT_TYPE, wxAUI_GRADIENT_NONE);
        mgr->Update();
    }
    if (notebook) {
        auto* tab = new ThemedTabArt();
        tab->SetColour(t.window);
        tab->SetActiveColour(t.panel);
        notebook->SetArtProvider(tab);
    }
    root->Refresh();
}

// ---------------------------------------------------------------------------
void ThemedTabArt::DrawBackground(wxDC& dc, wxWindow*, const wxRect& rect) {
    const Theme& t = CurrentTheme();
    dc.SetBrush(wxBrush(t.window));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(rect);
    dc.SetPen(wxPen(t.grid));
    dc.DrawLine(rect.x, rect.GetBottom(), rect.GetRight() + 1, rect.GetBottom());
}

void ThemedTabArt::DrawTab(wxDC& dc, wxWindow* wnd, const wxAuiNotebookPage& page,
                           const wxRect& in_rect, int /*close_button_state*/,
                           wxRect* out_tab_rect, wxRect* out_button_rect, int* x_extent) {
    const Theme& t = CurrentTheme();
    dc.SetFont(page.active ? m_selectedFont : m_normalFont);
    wxSize ts = dc.GetTextExtent(page.caption);
    int pad = wnd->FromDIP(14);
    int tab_w = ts.x + 2 * pad;
    int tab_h = in_rect.height;
    wxRect r(in_rect.x, in_rect.y, tab_w, tab_h);

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(page.active ? t.panel : t.window));
    dc.DrawRectangle(r);
    if (page.active) {
        dc.SetBrush(wxBrush(t.accent));
        dc.DrawRectangle(r.x, r.GetBottom() - 2, r.width, 3);
    }
    dc.SetTextForeground(page.active ? t.text : t.dim);
    dc.DrawText(page.caption, r.x + pad, r.y + (tab_h - ts.y) / 2);

    *out_tab_rect = r;
    if (out_button_rect) *out_button_rect = wxRect();
    *x_extent = tab_w;
}
