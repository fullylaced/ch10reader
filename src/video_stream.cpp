// src/video_stream.cpp
#include "ch10/video_stream.h"

#include <algorithm>
#include <cstring>

#include "ch10/reader.h"
#include "ch10/video.h"

#ifdef CH10_HAVE_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
}
#include "ts_feeder.h"
#endif

namespace ch10 {

namespace {
constexpr uint64_t kLookbackRtc  = 2ull * 10000000;   // 2 s before a seek target
constexpr uint64_t kForwardRtc   = 5ull * 10000000;   // decode forward up to 5 s
}

// ===========================================================================
#ifdef CH10_HAVE_FFMPEG

struct VideoStream::Impl {
    internal::TsFeeder  feeder;
    internal::TsDecoder dec;
    internal::FrameClock clock;
    SwsContext* sws = nullptr;
    std::unordered_map<int64_t, uint64_t> pts_rtc;
    uint64_t last_rtc = 0;

    void Teardown() {
        dec.Teardown();
        if (sws) { sws_freeContext(sws); sws = nullptr; }
        pts_rtc.clear();
        clock.Reset();
    }
};

bool VideoStream::Available() { return true; }

VideoStream::VideoStream() : impl_(new Impl) {
    static bool quiet = (av_log_set_level(AV_LOG_FATAL), true);
    (void)quiet;
}

VideoStream::~VideoStream() { impl_->Teardown(); }

bool VideoStream::Open(const Session& session, int channel) {
    impl_->Teardown();
    cache_.clear(); cache_bytes_ = 0;
    open_ = false;
    channel_ = channel;
    const ChannelStats* cs = session.Channel(channel);
    if (!cs || !session.IsVideoType(cs->data_type)) { error_ = "not a video channel"; return false; }
    if (!cs->video.transport) { error_ = "program-stream video is not supported for playback"; return false; }
    if (!impl_->feeder.Open(session, channel)) { error_ = "channel has no packets"; return false; }
    first_rtc_ = session.packets()[impl_->feeder.pkts.front()].rtc;
    last_rtc_  = session.packets()[impl_->feeder.pkts.back()].rtc;
    open_ = true;
    return true;
}

bool VideoStream::Restart(uint64_t start_rtc) {
    Impl& im = *impl_;
    im.Teardown();
    cache_.clear(); cache_bytes_ = 0;
    im.feeder.SeekTo(start_rtc);
    std::string err;
    if (!im.dec.Setup(im.feeder, AVMEDIA_TYPE_VIDEO, err, codec_name_)) {
        error_ = err; im.Teardown(); return false;
    }
    im.clock.tb = im.dec.time_base;
    im.clock.srs = im.feeder.srs;
    width_ = im.dec.codec->width; height_ = im.dec.codec->height;
    return true;
}

std::shared_ptr<const VideoFrame> VideoStream::DecodeNext() {
    Impl& im = *impl_;
    uint64_t pkt_rtc = 0;
    if (!im.dec.Next(im.feeder, pkt_rtc, im.pts_rtc, im.last_rtc)) return nullptr;
    AVFrame* fr = im.dec.frame;

    auto f = std::make_shared<VideoFrame>();
    f->rtc = im.clock.Map(pkt_rtc, fr->best_effort_timestamp);
    f->width = fr->width; f->height = fr->height;
    width_ = f->width; height_ = f->height;
    im.sws = sws_getCachedContext(im.sws, f->width, f->height,
                                  static_cast<AVPixelFormat>(fr->format),
                                  f->width, f->height, AV_PIX_FMT_RGB24,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
    f->rgb.resize(static_cast<std::size_t>(f->width) * f->height * 3);
    uint8_t* dst[4] = { f->rgb.data(), nullptr, nullptr, nullptr };
    int dst_stride[4] = { f->width * 3, 0, 0, 0 };
    sws_scale(im.sws, fr->data, fr->linesize, 0, f->height, dst, dst_stride);
    av_frame_unref(fr);
    return f;
}

#else  // ------------------------------------------------------------ stub

struct VideoStream::Impl {};
bool VideoStream::Available() { return false; }
VideoStream::VideoStream() : impl_(new Impl) {}
VideoStream::~VideoStream() {}
bool VideoStream::Open(const Session&, int channel) {
    channel_ = channel;
    error_ = "this build has no FFmpeg support; video playback is disabled";
    return false;
}
bool VideoStream::Restart(uint64_t) { return false; }
std::shared_ptr<const VideoFrame> VideoStream::DecodeNext() { return nullptr; }

#endif

// ===========================================================================
// Cache and seeking (shared by both builds)
// ===========================================================================
void VideoStream::PushCache(std::shared_ptr<const VideoFrame> f) {
    cache_bytes_ += f->Bytes();
    cache_.push_back(std::move(f));
    while (cache_.size() > 1 && cache_bytes_ > cache_budget_) {
        cache_bytes_ -= cache_.front()->Bytes();
        cache_.pop_front();
    }
}

std::shared_ptr<const VideoFrame> VideoStream::FromCache(uint64_t rtc) const {
    // Latest frame with frame.rtc <= rtc.
    std::shared_ptr<const VideoFrame> best;
    for (const auto& f : cache_) {
        if (f->rtc <= rtc) best = f; else break;
    }
    return best;
}

std::shared_ptr<const VideoFrame> VideoStream::SeekTo(uint64_t rtc) {
    if (!open_) return nullptr;

    const bool have_cache = !cache_.empty();
    const uint64_t cache_lo = have_cache ? cache_.front()->rtc : 0;
    const uint64_t cache_hi = have_cache ? cache_.back()->rtc : 0;

    // 1. Inside what we already decoded.
    if (have_cache && rtc >= cache_lo && rtc <= cache_hi)
        return FromCache(rtc);

    // 2. A little ahead: keep decoding forward.
    bool forward = have_cache && rtc > cache_hi && (rtc - cache_hi) < kForwardRtc
                   && impl_ && !cache_.empty();
    if (!forward) {
        // 3. Backwards or a jump: restart shortly before the target.
        uint64_t start = rtc > first_rtc_ + kLookbackRtc ? rtc - kLookbackRtc : first_rtc_;
        if (!Restart(start)) return nullptr;
    }

    std::shared_ptr<const VideoFrame> first_after;
    int budget = decode_budget_;
    while (cache_.empty() || cache_.back()->rtc < rtc) {
        if (budget-- <= 0) break;                   // stay responsive; catch up next call
        auto f = DecodeNext();
        if (!f) break;                              // end of channel
        if (f->rtc > rtc && !first_after) first_after = f;
        PushCache(std::move(f));
        if (cache_.back()->rtc > rtc) break;
    }
    auto shown = FromCache(rtc);
    if (!shown) shown = first_after ? first_after
                                    : (cache_.empty() ? nullptr : cache_.front());
    return shown;
}

} // namespace ch10
