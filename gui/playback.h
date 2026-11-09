// gui/playback.h
// Playback clock for the whole application. Owns the cursor (an RTC value on
// the recording's 10 MHz clock), the play/pause state and the speed, and
// advances the cursor from a wxTimer while playing. Every pane that wants to
// follow the cursor registers a listener; the video panels, timeline, packet
// list and 1553 list all move together because they share this one clock.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include <wx/wx.h>

class PlaybackController : public wxEvtHandler {
public:
    // Called whenever the cursor moves or play state changes.
    using Listener = std::function<void(uint64_t rtc, bool playing)>;

    PlaybackController();

    void SetRange(uint64_t rtc_min, uint64_t rtc_max);
    void AddListener(Listener fn) { listeners_.push_back(std::move(fn)); }

    void Play();                 // forward at |speed|
    void Rewind();               // backwards at |speed|
    void Pause();
    void TogglePlay();
    void Stop();                 // pause and return to the start
    void Seek(uint64_t rtc);     // absolute
    void StepSeconds(double s);  // relative, works while paused
    void JumpStart() { Seek(min_); }
    void JumpEnd()   { Seek(max_); }
    void SetSpeed(double magnitude);   // 0.1 ... 16
    void SpeedUp();                    // next step in Speeds()
    void SlowDown();
    static const std::vector<double>& Speeds();
    int  SpeedIndex() const;           // index into Speeds() closest to speed()

    // Thread-safe view for the audio thread: where the cursor is *right now*,
    // extrapolated from the last tick, and whether we are playing.
    struct Snapshot { uint64_t rtc; double speed; bool playing; };
    Snapshot Now() const;

    uint64_t cursor()  const { return cursor_; }
    bool     playing() const { return playing_; }
    double   speed()   const { return speed_ * (reverse_ ? -1 : 1); }
    uint64_t rtc_min() const { return min_; }
    uint64_t rtc_max() const { return max_; }
    double   Fraction() const;         // 0..1 position within the range

private:
    void OnTick(wxTimerEvent&);
    void Notify();
    uint64_t Clamp(int64_t rtc) const;

    wxTimer     timer_;
    wxStopWatch clock_;
    std::vector<Listener> listeners_;
    mutable std::mutex snap_mutex_;
    std::chrono::steady_clock::time_point tick_at_ = std::chrono::steady_clock::now();
    void UpdateSnapshot();
    uint64_t min_ = 0, max_ = 0, cursor_ = 0;
    bool     playing_ = false;
    bool     reverse_ = false;
    double   speed_   = 1.0;
};
