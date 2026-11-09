// gui/timeline_panel.cpp
#include "timeline_panel.h"

#include <algorithm>
#include <cmath>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include "theme.h"

using ch10::kActivityBins;

wxColour TypeColour(uint8_t t) {
    switch (t) {
        case 0x00: return wxColour(255, 210, 90);    // user-defined (annotations)
        case 0x01: return wxColour(150, 150, 160);   // TMATS
        case 0x02: case 0x03: return wxColour(170, 130, 200);
        case 0x09: return wxColour(90, 200, 160);    // PCM
        case 0x11: return wxColour(120, 220, 120);   // Time
        case 0x19: case 0x1A: return wxColour(255, 170, 60);   // 1553
        case 0x21: return wxColour(80, 200, 220);    // Analog
        case 0x29: return wxColour(200, 220, 90);    // Discrete
        case 0x38: return wxColour(240, 130, 160);   // ARINC
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44:
                   return wxColour(90, 150, 255);    // Video
        case 0x68: case 0x69: return wxColour(200, 120, 255); // Ethernet
        default:   return wxColour(180, 180, 200);
    }
}

wxString TypeShortName(uint8_t t) {
    switch (t) {
        case 0x00: return "User/Notes";
        case 0x01: return "TMATS";
        case 0x02: return "Events";
        case 0x03: return "Index";
        case 0x09: return "PCM";
        case 0x11: return "Time";
        case 0x19: return "1553";
        case 0x1A: return "16PP194";
        case 0x21: return "Analog";
        case 0x29: return "Discrete";
        case 0x30: return "Message";
        case 0x38: return "ARINC-429";
        case 0x40: return "Video F0";
        case 0x41: return "Video F1";
        case 0x42: return "Video F2";
        case 0x50: return "UART";
        case 0x68: return "Ethernet";
        default:   return wxString::Format("0x%02X", t);
    }
}

wxColour SeverityColour(ch10::Severity s) {
    switch (s) {
        case ch10::Severity::Note:    return wxColour(100, 180, 255);
        case ch10::Severity::Warning: return wxColour(255, 190, 60);
        case ch10::Severity::Anomaly: return wxColour(255, 80, 80);
        case ch10::Severity::Marker:  return wxColour(140, 230, 140);
    }
    return wxColour(200, 200, 200);
}

TimelinePanel::TimelinePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
              wxFULL_REPAINT_ON_RESIZE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(-1, 140));
    Bind(wxEVT_PAINT, &TimelinePanel::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &TimelinePanel::OnMouse, this);
    Bind(wxEVT_LEFT_UP, &TimelinePanel::OnMouse, this);
    Bind(wxEVT_MOTION, &TimelinePanel::OnMouse, this);
    Bind(wxEVT_LEAVE_WINDOW, &TimelinePanel::OnMouse, this);
    Bind(wxEVT_SIZE, &TimelinePanel::OnSize, this);
}

void TimelinePanel::SetSession(const ch10::Session* s) {
    session_ = s;
    cursor_ = s ? s->rtc_min() : 0;
    range_valid_ = false;
    int rows = s ? static_cast<int>(s->activity().size()) : 0;
    SetMinSize(wxSize(-1, kAxisHeight + kMarkerStrip + std::clamp(rows, 3, 16) * 16 + 8));
    RebuildMarkers();
    Refresh();
}

void TimelinePanel::SetCursor(uint64_t rtc) {
    if (rtc == cursor_) return;
    cursor_ = rtc;
    Refresh(false);
}

void TimelinePanel::SetRange(uint64_t in, uint64_t out, bool valid) {
    in_ = std::min(in, out); out_ = std::max(in, out); range_valid_ = valid;
    Refresh(false);
}

wxRect TimelinePanel::PlotRect() const {
    wxSize sz = GetClientSize();
    int top = kAxisHeight + kMarkerStrip;
    return wxRect(kLabelWidth, top, std::max(1, sz.x - kLabelWidth - 8),
                  std::max(1, sz.y - top - 4));
}

int TimelinePanel::RowHeight() const {
    if (!session_ || session_->activity().empty()) return 20;
    return std::max(6, std::min(20, PlotRect().height /
                                  static_cast<int>(session_->activity().size())));
}

int TimelinePanel::RowY(int channel) const {
    if (!session_) return -1;
    const auto& rows = session_->activity();
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].channel_id == channel) return PlotRect().y + static_cast<int>(i) * RowHeight();
    return -1;
}

uint64_t TimelinePanel::XToRtc(int x) const {
    if (!session_) return 0;
    wxRect r = PlotRect();
    double f = std::clamp((x - r.x) / static_cast<double>(r.width), 0.0, 1.0);
    return session_->rtc_min() +
           static_cast<uint64_t>(f * (session_->rtc_max() - session_->rtc_min()));
}

int TimelinePanel::RtcToX(uint64_t rtc) const {
    wxRect r = PlotRect();
    uint64_t span = session_->rtc_max() - session_->rtc_min();
    double f = span ? (static_cast<double>(rtc) - session_->rtc_min()) / span : 0;
    return r.x + static_cast<int>(std::clamp(f, 0.0, 1.0) * r.width);
}

void TimelinePanel::RebuildMarkers() {
    markers_.clear();
    hover_ = -1;
    if (!session_) return;
    wxRect plot = PlotRect();
    const int strip_y = kAxisHeight;
    for (const auto& a : session_->annotations()) {
        Marker m; m.a = &a;
        int x0 = RtcToX(a.rtc_start), x1 = RtcToX(a.IsRange() ? a.rtc_end : a.rtc_start);
        m.hit = wxRect(std::min(x0, x1) - 5, strip_y, std::max(10, x1 - x0 + 10), kMarkerStrip);
        markers_.push_back(m);
    }
    int rh = RowHeight();
    for (const auto& a : session_->anomalies()) {
        Marker m; m.a = &a; m.anomaly = true;
        int x = RtcToX(a.rtc_start);
        int y = RowY(a.channel);
        m.hit = y >= 0 ? wxRect(x - 4, y, 9, rh) : wxRect(x - 4, strip_y, 9, kMarkerStrip);
        markers_.push_back(m);
    }
    (void)plot;
}

void TimelinePanel::OnMouse(wxMouseEvent& ev) {
    if (!session_) return;
    if (ev.Moving() || ev.Dragging()) {
        int h = -1;
        for (int i = static_cast<int>(markers_.size()) - 1; i >= 0; --i)
            if (markers_[i].hit.Contains(ev.GetPosition())) { h = i; break; }
        if (h != hover_) {
            hover_ = h;
            if (h >= 0) {
                const auto& a = *markers_[h].a;
                wxString tip = wxString::Format("%s  %s\n%s",
                    wxString::FromUTF8(session_->RelativeString(a.rtc_start).c_str()),
                    markers_[h].anomaly ? "anomaly" : ch10::SeverityName(a.severity),
                    wxString::FromUTF8(a.text.c_str()));
                SetToolTip(tip);
            } else {
                UnsetToolTip();
            }
            Refresh(false);
        }
    }
    if (ev.LeftDown()) {
        if (hover_ >= 0 && on_marker) {
            on_marker(*markers_[hover_].a, markers_[hover_].anomaly);
            if (on_seek) on_seek(markers_[hover_].a->rtc_start);
            return;
        }
        dragging_ = true; CaptureMouse();
    }
    if (ev.LeftUp() || ev.Leaving()) {
        if (dragging_ && HasCapture()) ReleaseMouse();
        dragging_ = false;
        if (ev.Leaving() && hover_ >= 0) { hover_ = -1; UnsetToolTip(); Refresh(false); }
    }
    if ((ev.LeftDown() || (ev.Dragging() && dragging_)) && on_seek)
        on_seek(XToRtc(ev.GetX()));
}

void TimelinePanel::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    const Theme& th = CurrentTheme();
    dc.SetBackground(wxBrush(th.canvas));
    dc.Clear();

    wxRect plot = PlotRect();
    dc.SetFont(wxFont(wxFontInfo(9)));

    if (!session_ || session_->activity().empty()) {
        dc.SetTextForeground(th.dim);
        dc.DrawLabel("Open a recording to see the data flow per channel",
                     GetClientRect(), wxALIGN_CENTER);
        return;
    }

    const auto& rows = session_->activity();
    const int n = static_cast<int>(rows.size());
    const int row_h = RowHeight();

    // --- time axis
    dc.SetPen(wxPen(th.grid));
    dc.DrawLine(plot.x, kAxisHeight - 1, plot.GetRight(), kAxisHeight - 1);
    double dur = session_->duration_s();
    double step = 1;
    const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 30, 60, 120, 300, 600, 1800, 3600};
    for (double s : steps) { step = s; if (dur / s <= plot.width / 90.0) break; }
    dc.SetTextForeground(th.text);
    for (double t = 0; t <= dur + 1e-9; t += step) {
        int x = plot.x + static_cast<int>(t / (dur > 0 ? dur : 1) * plot.width);
        dc.SetPen(wxPen(th.grid));
        dc.DrawLine(x, kAxisHeight - 6, x, plot.GetBottom());
        wxString label = step < 1 ? wxString::Format("%.1fs", t)
                                  : wxString::Format("%d:%02d", int(t) / 60, int(t) % 60);
        dc.DrawText(label, x + 3, 3);
    }

    // --- marker strip background
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(th.canvas_row.ChangeLightness(th.dark ? 90 : 105)));
    dc.DrawRectangle(plot.x, kAxisHeight, plot.width, kMarkerStrip - 2);
    dc.SetTextForeground(th.dim);
    dc.DrawText("notes", 12, kAxisHeight + 1);

    // --- one heat row per channel
    for (int i = 0; i < n; ++i) {
        const auto& row = rows[i];
        int y = plot.y + i * row_h;
        if (y + row_h > plot.GetBottom() + 1) break;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(th.panel));
        dc.DrawRectangle(0, y, kLabelWidth - 4, row_h);
        wxColour c = TypeColour(row.data_type);
        dc.SetBrush(wxBrush(c));
        dc.DrawRectangle(4, y + 2, 4, row_h - 4);
        dc.SetTextForeground(th.text);
        wxString label = wxString::Format("%d  %s", row.channel_id, TypeShortName(row.data_type));
        if (row_h >= 12)
            dc.DrawLabel(label, wxRect(12, y, kLabelWidth - 16, row_h),
                         wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);

        dc.SetBrush(wxBrush(th.canvas_row));
        dc.DrawRectangle(plot.x, y, plot.width, row_h);
        if (row.max_bytes == 0) continue;
        const int cols = plot.width;
        for (int px = 0; px < cols; ++px) {
            int b0 = px * kActivityBins / cols;
            int b1 = std::max(b0 + 1, (px + 1) * kActivityBins / cols);
            uint64_t sum = 0;
            for (int b = b0; b < b1 && b < kActivityBins; ++b) sum += row.bytes[b];
            if (!sum) continue;
            double v = std::log1p(static_cast<double>(sum)) /
                       std::log1p(static_cast<double>(row.max_bytes) * (b1 - b0));
            v = std::clamp(0.3 + 0.7 * v, 0.0, 1.0);
            wxColour cc = th.dark
                ? wxColour(static_cast<unsigned char>(c.Red() * v),
                           static_cast<unsigned char>(c.Green() * v),
                           static_cast<unsigned char>(c.Blue() * v))
                : c.ChangeLightness(static_cast<int>(130 - 60 * v));
            dc.SetPen(wxPen(cc));
            dc.DrawLine(plot.x + px, y + 1, plot.x + px, y + row_h - 1);
        }
    }

    // --- In/Out range and translucent overlays
    if (wxGraphicsContext* gc = wxGraphicsContext::Create(dc)) {
        if (range_valid_) {
            int x0 = RtcToX(in_), x1 = RtcToX(out_);
            gc->SetBrush(wxBrush(wxColour(th.accent.Red(), th.accent.Green(), th.accent.Blue(), 40)));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRectangle(x0, kAxisHeight, std::max(1, x1 - x0), plot.GetBottom() - kAxisHeight);
            gc->SetPen(wxPen(th.accent, 1, wxPENSTYLE_SHORT_DASH));
            gc->StrokeLine(x0, kAxisHeight - 6, x0, plot.GetBottom());
            gc->StrokeLine(x1, kAxisHeight - 6, x1, plot.GetBottom());
            gc->SetFont(wxFont(wxFontInfo(8).Bold()), th.accent);
            gc->DrawText("IN", x0 + 2, kAxisHeight - 20);
            gc->DrawText("OUT", x1 - 22, kAxisHeight - 20);
        }
        // annotation markers
        for (std::size_t i = 0; i < markers_.size(); ++i) {
            const Marker& m = markers_[i];
            const auto& a = *m.a;
            wxColour col = m.anomaly ? SeverityColour(ch10::Severity::Anomaly)
                                     : SeverityColour(a.severity);
            bool hot = static_cast<int>(i) == hover_;
            if (m.anomaly) {
                int x = RtcToX(a.rtc_start);
                int y = RowY(a.channel);
                if (y < 0) y = kAxisHeight;
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->SetBrush(wxBrush(wxColour(col.Red(), col.Green(), col.Blue(), hot ? 255 : 200)));
                wxGraphicsPath tri = gc->CreatePath();
                tri.MoveToPoint(x - 4, y); tri.AddLineToPoint(x + 4, y);
                tri.AddLineToPoint(x, y + 6); tri.CloseSubpath();
                gc->FillPath(tri);
                continue;
            }
            int x0 = RtcToX(a.rtc_start);
            if (a.IsRange()) {
                int x1 = RtcToX(a.rtc_end);
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->SetBrush(wxBrush(wxColour(col.Red(), col.Green(), col.Blue(), hot ? 90 : 45)));
                gc->DrawRectangle(x0, kAxisHeight, std::max(2, x1 - x0), plot.GetBottom() - kAxisHeight);
                gc->SetBrush(wxBrush(col));
                gc->DrawRectangle(x0, kAxisHeight + 3, std::max(2, x1 - x0), kMarkerStrip - 8);
            } else {
                gc->SetPen(wxPen(wxColour(col.Red(), col.Green(), col.Blue(), hot ? 220 : 110), 1));
                gc->StrokeLine(x0, kAxisHeight, x0, plot.GetBottom());
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->SetBrush(wxBrush(col));
                wxGraphicsPath d = gc->CreatePath();
                int cy = kAxisHeight + kMarkerStrip / 2 - 1;
                d.MoveToPoint(x0, cy - 6); d.AddLineToPoint(x0 + 6, cy);
                d.AddLineToPoint(x0, cy + 6); d.AddLineToPoint(x0 - 6, cy); d.CloseSubpath();
                gc->FillPath(d);
            }
        }
        delete gc;
    }

    // --- cursor
    int cx = RtcToX(cursor_);
    dc.SetPen(wxPen(th.dark ? *wxWHITE : *wxBLACK, 2));
    dc.DrawLine(cx, kAxisHeight - 8, cx, plot.GetBottom());
    dc.SetTextForeground(th.dark ? *wxWHITE : *wxBLACK);
    wxString ts = wxString::FromUTF8(session_->RelativeString(cursor_).c_str());
    wxSize tsz = dc.GetTextExtent(ts);
    dc.DrawText(ts, std::min(cx + 6, plot.GetRight() - tsz.x), plot.GetBottom() - tsz.y - 2);
}
