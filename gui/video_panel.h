// gui/video_panel.h
// Shows one video channel. UpdateTo(rtc) asks the VideoStream for the frame
// at that recording time and repaints; the frame is drawn letterboxed with
// an overlay showing channel, codec and the frame's own time stamp.

#pragma once

#include <cstdint>
#include <memory>

#include <wx/wx.h>

#include "ch10/session.h"
#include "ch10/video_stream.h"

class VideoPanel : public wxPanel {
public:
    VideoPanel(wxWindow* parent, const ch10::Session& session, int channel);

    bool ok() const { return stream_.IsOpen(); }
    int  channel() const { return channel_; }

    // Move to `rtc`; decodes as needed. Cheap when the frame does not change.
    void UpdateTo(uint64_t rtc);

private:
    void OnPaint(wxPaintEvent&);

    const ch10::Session& session_;
    int                  channel_;
    wxString             title_;
    ch10::VideoStream    stream_;
    std::shared_ptr<const ch10::VideoFrame> frame_;
    wxBitmap             bitmap_;
    uint64_t             cursor_ = 0;
    wxString             status_;
};
