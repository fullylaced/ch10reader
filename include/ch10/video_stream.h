// ch10/video_stream.h
// Decodes one Chapter 10 video channel (Video F0/F1 transport stream) into
// RGB frames addressed by the recording's RTC clock.
//
// The transport-stream bytes are pulled directly out of the Chapter 10
// packets (no temporary file) and handed to FFmpeg through a custom AVIO
// read callback. Every MPEG packet the demuxer returns is tagged with the
// RTC of the Chapter 10 packet it came from, so each decoded frame gets a
// time on the same clock as the 1553 messages and every other channel.
//
// SeekTo(rtc) is the only operation a player needs: it decodes forward, or
// restarts from a little before the target when asked to go backwards or
// jump, and keeps a bounded cache of recent frames so scrubbing backwards
// through the last few seconds needs no re-decoding.
//
// Built without FFmpeg (CH10_HAVE_FFMPEG undefined) the class compiles to a
// stub whose Open() fails with an explanatory message.

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ch10/session.h"

namespace ch10 {

struct VideoFrame {
    int                  width  = 0;
    int                  height = 0;
    uint64_t             rtc    = 0;     // 10 MHz, same clock as packets
    std::vector<uint8_t> rgb;            // width*height*3, row-major RGB24
    std::size_t Bytes() const { return rgb.size(); }
};

class VideoStream {
public:
    VideoStream();
    ~VideoStream();
    VideoStream(const VideoStream&) = delete;
    VideoStream& operator=(const VideoStream&) = delete;

    // Bind to a video channel of an open session.
    bool Open(const Session& session, int channel);
    bool IsOpen() const { return open_; }
    const std::string& error() const { return error_; }

    int      channel()   const { return channel_; }
    uint64_t first_rtc() const { return first_rtc_; }   // RTC of first packet
    uint64_t last_rtc()  const { return last_rtc_; }
    std::string codec_name() const { return codec_name_; }
    int width()  const { return width_; }
    int height() const { return height_; }

    // Bring the stream to `rtc`. Returns the frame that should be shown at
    // that time (the latest frame with frame.rtc <= rtc, or the first frame
    // after it when nothing precedes it), or nullptr if none is available.
    std::shared_ptr<const VideoFrame> SeekTo(uint64_t rtc);

    // Bytes the frame cache may hold (default 96 MB).
    void SetCacheBudget(std::size_t bytes) { cache_budget_ = bytes; }
    // Max frames decoded per SeekTo call (default 40): at high playback speeds
    // the picture lags briefly instead of stalling the UI.
    void SetDecodeBudget(int frames) { decode_budget_ = frames; }

    static bool Available();          // false in a build without FFmpeg

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool        open_       = false;
    std::string error_;
    int         channel_    = -1;
    uint64_t    first_rtc_  = 0;
    uint64_t    last_rtc_   = 0;
    std::string codec_name_;
    int         width_ = 0, height_ = 0;
    std::size_t cache_budget_ = 96u << 20;
    int         decode_budget_ = 40;

    // Decoded frames in presentation order (rtc ascending), bounded by bytes.
    std::deque<std::shared_ptr<const VideoFrame>> cache_;
    std::size_t cache_bytes_ = 0;

    bool Restart(uint64_t start_rtc);
    std::shared_ptr<const VideoFrame> DecodeNext();   // nullptr at end
    void PushCache(std::shared_ptr<const VideoFrame> f);
    std::shared_ptr<const VideoFrame> FromCache(uint64_t rtc) const;
};

} // namespace ch10
