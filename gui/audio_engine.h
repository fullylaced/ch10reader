// gui/audio_engine.h
// Audio output for playback. Owns the miniaudio device; the device callback
// asks the PlaybackController where the cursor is right now and renders every
// enabled source at that time and speed, so audio follows play / rewind /
// slow-motion / fast-forward exactly like the video does (pitch follows
// speed, like a tape). Keeps recent output for the waveform display.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ch10/audio.h"
#include "playback.h"

class AudioEngine {
public:
    struct Slot {
        std::shared_ptr<ch10::AudioSource> source;
        std::string kind;            // "analog", "1553", "video"
        int   channel = -1;
        bool  enabled = true;
        float gain = 0.8f;
        float peak = 0;              // last rendered peak, for meters
    };

    AudioEngine();
    ~AudioEngine();

    bool Start(const PlaybackController* controller);   // opens the device
    void Stop();
    bool running() const { return running_; }
    const std::string& error() const { return error_; }
    int  output_rate() const { return rate_; }
    std::string backend() const { return backend_; }

    // Source management (thread-safe).
    std::size_t Add(std::shared_ptr<ch10::AudioSource> src, const std::string& kind, int channel);
    void Clear();
    std::vector<Slot> Slots() const;                     // copy for the UI
    void SetEnabled(std::size_t i, bool on);
    void SetGain(std::size_t i, float gain);
    void SetMaster(float gain) { master_ = gain; }
    void SetMuted(bool m) { muted_ = m; }
    bool muted() const { return muted_; }
    float master() const { return master_; }

    // Recent mono output (oldest first), for drawing; and the output peak.
    void RecentOutput(std::vector<float>& out, std::size_t max_frames) const;
    float output_peak() const { return out_peak_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    void Render(float* stereo_out, unsigned frames);   // audio thread entry (called by the device callback)
private:

    const PlaybackController* controller_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    std::atomic<float> master_{0.8f};
    std::atomic<bool>  muted_{false};
    std::atomic<float> out_peak_{0};
    bool running_ = false;
    int  rate_ = 48000;
    std::string error_, backend_;

    // audio-thread state
    double audio_rtc_ = 0;
    bool   have_pos_ = false;
    std::vector<float> scratch_;
    // visualisation ring
    mutable std::mutex viz_mutex_;
    std::vector<float> viz_;
    std::size_t viz_head_ = 0;
};
