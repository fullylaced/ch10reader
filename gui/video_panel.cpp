// gui/video_panel.cpp
#include "video_panel.h"

#include <algorithm>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

VideoPanel::VideoPanel(wxWindow* parent, const ch10::Session& session, int channel)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
              wxFULL_REPAINT_ON_RESIZE),
      session_(session), channel_(channel) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(320, 200));
    Bind(wxEVT_PAINT, &VideoPanel::OnPaint, this);

    const ch10::ChannelStats* cs = session.Channel(channel);
    title_ = wxString::Format("Channel %d", channel);
    if (cs && !cs->source.empty())
        title_ += " - " + wxString::FromUTF8(cs->source.c_str());
    if (cs) title_ += wxString::Format("  [%s]", cs->video.FormatName());

    if (!stream_.Open(session, channel))
        status_ = wxString::FromUTF8(stream_.error().c_str());
}

void VideoPanel::UpdateTo(uint64_t rtc) {
    cursor_ = rtc;
    if (!stream_.IsOpen()) { Refresh(false); return; }

    auto f = stream_.SeekTo(rtc);
    if (f != frame_) {
        frame_ = f;
        if (frame_) {
            // wxImage takes the RGB buffer without copying (static_data=true);
            // the shared_ptr keeps it alive for as long as the bitmap needs it.
            wxImage img(frame_->width, frame_->height,
                        const_cast<unsigned char*>(frame_->rgb.data()), true);
            bitmap_ = wxBitmap(img);
        } else {
            bitmap_ = wxBitmap();
        }
        status_ = frame_ ? wxString() : "No video at this time";
    }
    Refresh(false);
}

void VideoPanel::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(wxColour(12, 12, 14)));
    dc.Clear();
    wxSize sz = GetClientSize();

    const int kBar = 22;                       // top title bar + bottom info bar
    const int avail_h = std::max(1, sz.y - 2 * kBar);
    if (bitmap_.IsOk()) {
        double sx = static_cast<double>(sz.x) / bitmap_.GetWidth();
        double sy = static_cast<double>(avail_h) / bitmap_.GetHeight();
        double s  = std::max(0.01, std::min(sx, sy));
        int w = static_cast<int>(bitmap_.GetWidth() * s);
        int h = static_cast<int>(bitmap_.GetHeight() * s);
        int x = (sz.x - w) / 2, y = kBar + (avail_h - h) / 2;
        wxGraphicsContext* gc = wxGraphicsContext::Create(dc);
        if (gc) {
            gc->SetInterpolationQuality(wxINTERPOLATION_GOOD);
            gc->DrawBitmap(bitmap_, x, y, w, h);
            delete gc;
        } else {
            dc.SetUserScale(s, s);
            dc.DrawBitmap(bitmap_, static_cast<int>(x / s), static_cast<int>(y / s));
            dc.SetUserScale(1, 1);
        }
    }

    // Title bar (top) and frame info bar (bottom)
    dc.SetBrush(wxBrush(wxColour(0, 0, 0)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, sz.x, kBar);
    dc.DrawRectangle(0, sz.y - kBar, sz.x, kBar);
    dc.SetFont(wxFont(wxFontInfo(9)));
    dc.SetTextForeground(wxColour(220, 224, 232));
    dc.DrawLabel(title_, wxRect(6, 0, sz.x - 12, kBar), wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);

    if (frame_) {
        wxString left = wxString::Format("%dx%d  %s", frame_->width, frame_->height,
                                         wxString::FromUTF8(stream_.codec_name().c_str()));
        wxString right = "frame " +
            wxString::FromUTF8(session_.RelativeString(frame_->rtc).c_str()) + "   " +
            wxString::FromUTF8(session_.TimeString(frame_->rtc).c_str());
        wxRect bar(6, sz.y - kBar, sz.x - 12, kBar);
        dc.DrawLabel(left, bar, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
        dc.SetTextForeground(wxColour(160, 200, 255));
        dc.DrawLabel(right, bar, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
    }

    if (!status_.IsEmpty() || !bitmap_.IsOk()) {
        dc.SetTextForeground(wxColour(150, 154, 164));
        wxString msg = status_.IsEmpty() ? "No frame" : status_;
        dc.DrawLabel(msg, wxRect(0, kBar, sz.x, avail_h), wxALIGN_CENTER);
    }
}
