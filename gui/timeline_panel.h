// gui/timeline_panel.h
// Data-flow timeline: one row per channel (brightness = bytes per time slice),
// the playback cursor, the In/Out selection range, and markers for
// annotations (marker strip under the axis) and anomalies (ticks in the
// channel row they belong to). Click/drag to seek; hover a marker for its
// text; click a marker to select it.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <wx/wx.h>

#include "ch10/annotation.h"
#include "ch10/session.h"

wxColour TypeColour(uint8_t data_type);
wxString TypeShortName(uint8_t data_type);
wxColour SeverityColour(ch10::Severity s);

class TimelinePanel : public wxPanel {
public:
    explicit TimelinePanel(wxWindow* parent);

    void SetSession(const ch10::Session* s);   // nullptr clears
    void SetCursor(uint64_t rtc);
    void SetRange(uint64_t rtc_in, uint64_t rtc_out, bool valid);
    void MarkersChanged() { RebuildMarkers(); Refresh(false); }

    std::function<void(uint64_t rtc)> on_seek;
    // A marker was clicked: the annotation and whether it is an anomaly.
    std::function<void(const ch10::Annotation&, bool anomaly)> on_marker;

private:
    struct Marker {
        const ch10::Annotation* a = nullptr;
        bool anomaly = false;
        wxRect hit;                  // screen rect for hover/click
    };

    void OnPaint(wxPaintEvent&);
    void OnMouse(wxMouseEvent&);
    void OnSize(wxSizeEvent&) { RebuildMarkers(); Refresh(); }
    void RebuildMarkers();

    wxRect PlotRect() const;
    uint64_t XToRtc(int x) const;
    int      RtcToX(uint64_t rtc) const;
    int      RowHeight() const;
    int      RowY(int channel) const;    // -1 if channel has no row

    const ch10::Session* session_ = nullptr;
    uint64_t cursor_ = 0;
    uint64_t in_ = 0, out_ = 0;
    bool     range_valid_ = false;
    bool     dragging_ = false;
    std::vector<Marker> markers_;
    int      hover_ = -1;

    static constexpr int kLabelWidth  = 150;
    static constexpr int kAxisHeight  = 22;
    static constexpr int kMarkerStrip = 18;
};
