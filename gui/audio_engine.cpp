// gui/audio_engine.cpp
#include "audio_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "miniaudio.h"

struct AudioEngine::Impl {
    ma_device device{};
    bool inited = false;
};

namespace {
constexpr std::size_t kVizFrames = 48000 * 3;   // 3 s at 48 kHz
constexpr double kResyncRtc = 800000;           // 80 ms drift -> hard resync

void DeviceCallback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
    static_cast<AudioEngine*>(dev->pUserData)->Render(static_cast<float*>(out), frames);
}
} // namespace

AudioEngine::AudioEngine() : impl_(new Impl) {
    viz_.assign(kVizFrames, 0.0f);
}

AudioEngine::~AudioEngine() { Stop(); }

bool AudioEngine::Start(const PlaybackController* controller) {
    Stop();
    controller_ = controller;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = 48000;
    cfg.dataCallback      = DeviceCallback;
    cfg.pUserData         = this;
    cfg.periodSizeInMilliseconds = 20;
    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
        error_ = "no audio output device available";
        return false;
    }
    impl_->inited = true;
    rate_ = static_cast<int>(impl_->device.sampleRate);
    backend_ = ma_get_backend_name(impl_->device.pContext->backend);
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        error_ = "cannot start the audio device";
        ma_device_uninit(&impl_->device);
        impl_->inited = false;
        return false;
    }
    running_ = true;
    return true;
}

void AudioEngine::Stop() {
    if (impl_->inited) {
        ma_device_uninit(&impl_->device);   // joins the audio thread
        impl_->inited = false;
    }
    running_ = false;
}

std::size_t AudioEngine::Add(std::shared_ptr<ch10::AudioSource> src, const std::string& kind, int channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot s; s.source = std::move(src); s.kind = kind; s.channel = channel;
    slots_.push_back(std::move(s));
    return slots_.size() - 1;
}

void AudioEngine::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.clear();
    have_pos_ = false;
}

std::vector<AudioEngine::Slot> AudioEngine::Slots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_;
}

void AudioEngine::SetEnabled(std::size_t i, bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (i < slots_.size()) slots_[i].enabled = on;
}

void AudioEngine::SetGain(std::size_t i, float g) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (i < slots_.size()) slots_[i].gain = g;
}

void AudioEngine::RecentOutput(std::vector<float>& out, std::size_t max_frames) const {
    std::lock_guard<std::mutex> lock(viz_mutex_);
    std::size_t n = std::min(max_frames, viz_.size());
    out.resize(n);
    std::size_t start = (viz_head_ + viz_.size() - n) % viz_.size();
    for (std::size_t i = 0; i < n; ++i) out[i] = viz_[(start + i) % viz_.size()];
}

// Runs on the audio thread.
void AudioEngine::Render(float* out, unsigned frames) {
    std::memset(out, 0, sizeof(float) * frames * 2);
    if (!controller_) return;
    PlaybackController::Snapshot snap = controller_->Now();

    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;                 // UI is editing sources: skip

    // Follow the playback cursor; snap when we have drifted or on seek.
    double target = static_cast<double>(snap.rtc);
    if (!snap.playing || !have_pos_ || std::fabs(audio_rtc_ - target) > kResyncRtc) {
        audio_rtc_ = target;
        have_pos_ = true;
    }
    const double step = snap.speed * 1e7 / rate_;
    float peak = 0;

    if (snap.playing && !muted_ && !slots_.empty()) {
        scratch_.resize(frames);
        for (Slot& s : slots_) {
            if (!s.enabled || !s.source) continue;
            s.source->Render(audio_rtc_, step, static_cast<int>(frames), scratch_.data());
            float g = s.gain * master_, sp = 0;
            for (unsigned i = 0; i < frames; ++i) {
                float v = scratch_[i] * g;
                sp = std::max(sp, std::fabs(v));
                out[2 * i] += v; out[2 * i + 1] += v;
            }
            s.peak = sp;
        }
        for (unsigned i = 0; i < frames * 2; ++i) {
            out[i] = std::clamp(out[i], -1.0f, 1.0f);
            peak = std::max(peak, std::fabs(out[i]));
        }
    } else {
        for (Slot& s : slots_) s.peak = 0;
    }
    if (snap.playing) audio_rtc_ += step * frames;
    out_peak_ = peak;

    std::lock_guard<std::mutex> vlock(viz_mutex_);
    for (unsigned i = 0; i < frames; ++i) {
        viz_[viz_head_] = out[2 * i];
        viz_head_ = (viz_head_ + 1) % viz_.size();
    }
}
