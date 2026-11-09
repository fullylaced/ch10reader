// gui/playback.cpp
#include "playback.h"

#include <cmath>

namespace {
constexpr int kTickMs = 40;   // 25 updates/s is plenty for video + lists
const std::vector<double> kSpeedSteps = {0.1, 0.25, 0.5, 0.75, 1, 1.5, 2, 4, 8, 16};
}

const std::vector<double>& PlaybackController::Speeds() { return kSpeedSteps; }

int PlaybackController::SpeedIndex() const {
    int best = 0; double bd = 1e9;
    for (std::size_t i = 0; i < kSpeedSteps.size(); ++i) {
        double d = std::fabs(kSpeedSteps[i] - speed_);
        if (d < bd) { bd = d; best = static_cast<int>(i); }
    }
    return best;
}

void PlaybackController::SpeedUp() {
    int i = SpeedIndex();
    if (i + 1 < static_cast<int>(kSpeedSteps.size())) SetSpeed(kSpeedSteps[static_cast<std::size_t>(i + 1)]);
}

void PlaybackController::SlowDown() {
    int i = SpeedIndex();
    if (i > 0) SetSpeed(kSpeedSteps[static_cast<std::size_t>(i - 1)]);
}

PlaybackController::Snapshot PlaybackController::Now() const {
    std::lock_guard<std::mutex> lock(snap_mutex_);
    Snapshot s{cursor_, speed(), playing_};
    if (playing_) {
        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tick_at_).count();
        int64_t est = static_cast<int64_t>(cursor_) +
                      static_cast<int64_t>(ms * 10000.0 * s.speed);
        s.rtc = Clamp(est);
    }
    return s;
}

void PlaybackController::UpdateSnapshot() {
    std::lock_guard<std::mutex> lock(snap_mutex_);
    tick_at_ = std::chrono::steady_clock::now();
}

PlaybackController::PlaybackController() : timer_(this) {
    Bind(wxEVT_TIMER, &PlaybackController::OnTick, this);
}

void PlaybackController::SetRange(uint64_t rtc_min, uint64_t rtc_max) {
    Pause();
    min_ = rtc_min;
    max_ = rtc_max < rtc_min ? rtc_min : rtc_max;
    cursor_ = min_;
    Notify();
}

uint64_t PlaybackController::Clamp(int64_t rtc) const {
    if (rtc < static_cast<int64_t>(min_)) return min_;
    if (rtc > static_cast<int64_t>(max_)) return max_;
    return static_cast<uint64_t>(rtc);
}

double PlaybackController::Fraction() const {
    return max_ > min_ ? static_cast<double>(cursor_ - min_) / (max_ - min_) : 0.0;
}

void PlaybackController::Play() {
    reverse_ = false;
    if (cursor_ >= max_) cursor_ = min_;      // restart from the top
    playing_ = true;
    clock_.Start(0);
    timer_.Start(kTickMs);
    Notify();
}

void PlaybackController::Rewind() {
    reverse_ = true;
    playing_ = true;
    clock_.Start(0);
    timer_.Start(kTickMs);
    Notify();
}

void PlaybackController::Pause() {
    timer_.Stop();
    playing_ = false;
    Notify();
}

void PlaybackController::TogglePlay() {
    if (playing_) Pause(); else Play();
}

void PlaybackController::Stop() {
    timer_.Stop();
    playing_ = false;
    cursor_  = min_;
    Notify();
}

void PlaybackController::Seek(uint64_t rtc) {
    cursor_ = Clamp(static_cast<int64_t>(rtc));
    clock_.Start(0);
    Notify();
}

void PlaybackController::StepSeconds(double s) {
    int64_t delta = static_cast<int64_t>(s * 1e7);
    Seek(Clamp(static_cast<int64_t>(cursor_) + delta));
}

void PlaybackController::SetSpeed(double magnitude) {
    speed_ = magnitude > 0 ? magnitude : 1.0;
    Notify();
}

void PlaybackController::OnTick(wxTimerEvent&) {
    long elapsed_ms = clock_.Time();
    clock_.Start(0);
    // 10,000 RTC ticks per millisecond of recording time.
    int64_t delta = static_cast<int64_t>(elapsed_ms * 10000.0 * speed_);
    if (reverse_) delta = -delta;
    int64_t next = static_cast<int64_t>(cursor_) + delta;
    cursor_ = Clamp(next);
    bool hit_edge = (!reverse_ && cursor_ >= max_) || (reverse_ && cursor_ <= min_);
    if (hit_edge) {
        timer_.Stop();
        playing_ = false;
    }
    Notify();
}

void PlaybackController::Notify() {
    UpdateSnapshot();
    for (auto& fn : listeners_) fn(cursor_, playing_);
}
