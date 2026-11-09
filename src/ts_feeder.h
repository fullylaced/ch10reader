// src/ts_feeder.h  (internal, FFmpeg builds only)
// Pulls transport-stream bytes out of one Chapter 10 video channel and hands
// them to FFmpeg through a custom AVIO read callback, recording the stream
// byte position where each Chapter 10 packet's bytes begin so any demuxed
// packet (`AVPacket::pos`) can be mapped back to the RTC that carried it.
//
// FrameClock then spreads the frames that came from the same Chapter 10
// packet across time by their PTS, so playback is smooth within a packet
// while staying anchored to the recording clock at packet boundaries.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "ch10/reader.h"
#include "ch10/session.h"
#include "ch10/video.h"

namespace ch10 {
namespace internal {

constexpr int kAvioBufSize = 64 * 1024;

inline void QuietFfmpeg() {
    static bool once = (av_log_set_level(AV_LOG_FATAL), true);
    (void)once;
}

struct TsFeeder {
    const Session*           session = nullptr;
    std::unique_ptr<Reader>  reader;
    std::vector<uint32_t>    pkts;           // channel packet indices, file order
    std::size_t              next_pkt = 0;
    std::vector<uint8_t>     buf;
    std::size_t              buf_pos = 0;
    int64_t                  stream_pos = 0;
    std::deque<std::pair<int64_t, uint64_t>> pos_rtc;
    uint64_t                 last_pkt_rtc = 0;

    bool Open(const Session& s, int channel) {
        session = &s;
        pkts = s.ChannelPackets(channel);
        reader = std::make_unique<Reader>(s.path());
        if (const ChannelStats* cs = s.Channel(channel)) srs = cs->video.scr_rtc_sync;
        return !pkts.empty() && reader->IsOpen();
    }

    bool srs = false;   // from the channel's CSDW (SCR/RTC synchronised)

    // Position on the packet at or just before `rtc`.
    void SeekTo(uint64_t rtc) {
        const auto& idx = session->packets();
        std::size_t at = 0;
        while (at + 1 < pkts.size() && idx[pkts[at + 1]].rtc <= rtc) ++at;
        next_pkt = at;
        last_pkt_rtc = idx[pkts[at]].rtc;
        buf.clear(); buf_pos = 0; stream_pos = 0; pos_rtc.clear();
    }

    bool Fill() {
        while (next_pkt < pkts.size()) {
            uint32_t i = pkts[next_pkt++];
            const PacketIndexEntry& e = session->packets()[i];
            Packet p;
            if (!reader->ReadAt(static_cast<std::streamoff>(e.offset), p)) continue;
            buf.clear();
            video::Extract(p.header.data_type, p.Body(), p.BodyLen(), buf);
            if (buf.empty()) continue;
            buf_pos = 0;
            pos_rtc.emplace_back(stream_pos, e.rtc);
            if (pos_rtc.size() > 4096) pos_rtc.pop_front();
            return true;
        }
        return false;
    }

    int Read(uint8_t* out, int size) {
        int total = 0;
        while (size > 0) {
            if (buf_pos >= buf.size() && !Fill()) break;
            int n = static_cast<int>(std::min<std::size_t>(buf.size() - buf_pos,
                                                           static_cast<std::size_t>(size)));
            std::memcpy(out, buf.data() + buf_pos, static_cast<std::size_t>(n));
            buf_pos += n; out += n; size -= n; total += n; stream_pos += n;
        }
        return total > 0 ? total : AVERROR_EOF;
    }

    static int ReadCb(void* opaque, uint8_t* out, int size) {
        return static_cast<TsFeeder*>(opaque)->Read(out, size);
    }

    uint64_t RtcForPos(int64_t pos) const {
        uint64_t rtc = last_pkt_rtc;
        for (const auto& pr : pos_rtc) { if (pr.first <= pos) rtc = pr.second; else break; }
        return rtc;
    }
};

// Demuxer + one decoder for the chosen media type.
struct TsDecoder {
    AVFormatContext* fmt = nullptr;
    AVIOContext*     avio = nullptr;
    AVCodecContext*  codec = nullptr;
    AVPacket*        pkt = nullptr;
    AVFrame*         frame = nullptr;
    int              stream = -1;
    bool             draining = false;
    bool             eof = false;
    AVRational       time_base = {1, 90000};

    void Teardown() {
        if (codec) avcodec_free_context(&codec);
        if (fmt)   avformat_close_input(&fmt);
        if (avio)  { av_freep(&avio->buffer); avio_context_free(&avio); }
        if (pkt)   av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        stream = -1; draining = false; eof = false;
    }

    bool Setup(TsFeeder& feeder, AVMediaType type, std::string& err, std::string& codec_name) {
        QuietFfmpeg();
        uint8_t* iobuf = static_cast<uint8_t*>(av_malloc(kAvioBufSize));
        avio = avio_alloc_context(iobuf, kAvioBufSize, 0, &feeder, &TsFeeder::ReadCb, nullptr, nullptr);
        fmt = avformat_alloc_context();
        fmt->pb = avio;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(&fmt, "", av_find_input_format("mpegts"), nullptr) < 0) {
            err = "FFmpeg could not open the transport stream"; return false;
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            err = "no stream information found"; return false;
        }
        const AVCodec* dec = nullptr;
        stream = av_find_best_stream(fmt, type, -1, -1, &dec, 0);
        if (stream < 0 || !dec) {
            err = type == AVMEDIA_TYPE_AUDIO ? "no audio stream in this channel"
                                             : "no decodable video stream in this channel";
            return false;
        }
        codec = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(codec, fmt->streams[stream]->codecpar);
        codec->thread_count = 0;
        if (avcodec_open2(codec, dec, nullptr) < 0) {
            err = std::string("cannot open decoder ") + dec->name; return false;
        }
        time_base = fmt->streams[stream]->time_base;
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        codec_name = dec->name;
        return true;
    }

    // Decode the next frame of our stream into `frame`; `pkt_rtc` receives the
    // RTC of the Chapter 10 packet the compressed data came from. False at end.
    bool Next(TsFeeder& feeder, uint64_t& pkt_rtc, std::unordered_map<int64_t, uint64_t>& pts_rtc,
              uint64_t& last_rtc) {
        if (!codec || eof) return false;
        for (;;) {
            int ret = avcodec_receive_frame(codec, frame);
            if (ret == 0) break;
            if (ret != AVERROR(EAGAIN) || draining) { eof = true; return false; }
            ret = av_read_frame(fmt, pkt);
            if (ret < 0) { avcodec_send_packet(codec, nullptr); draining = true; continue; }
            if (pkt->stream_index == stream) {
                // Frames a parser split out of one PES report pos = -1: they
                // belong to the same Chapter 10 packet as the previous frame.
                uint64_t rtc = pkt->pos >= 0 ? feeder.RtcForPos(pkt->pos) : last_rtc;
                last_rtc = rtc;
                if (pkt->pts != AV_NOPTS_VALUE) pts_rtc[pkt->pts] = rtc;
                if (pts_rtc.size() > 1024) pts_rtc.clear();
                avcodec_send_packet(codec, pkt);
            }
            av_packet_unref(pkt);
        }
        int64_t ts = frame->best_effort_timestamp;
        auto it = ts != AV_NOPTS_VALUE ? pts_rtc.find(ts) : pts_rtc.end();
        if (it != pts_rtc.end()) { pkt_rtc = it->second; pts_rtc.erase(it); }
        else pkt_rtc = last_rtc;
        return true;
    }
};

// Converts (packet RTC, frame PTS) pairs into a smooth per-frame RTC.
// Packet RTCs are only as fine as Chapter 10 packetisation (up to 100 ms of
// data each) while PTS is sample-accurate, so the clock follows PTS and
// learns the PTS->RTC offset as the median over the first frames after a
// (re)start. A PTS jump of more than two seconds (wrap, splice) relearns it.
struct FrameClock {
    AVRational          tb = {1, 90000};
    bool                srs = false;      // CSDW SRS: PCR/PTS locked to RTC -> exact
    std::vector<double> offsets;
    double              offset = 0;
    bool                locked = false;
    int64_t             last_pts = AV_NOPTS_VALUE;

    uint64_t Map(uint64_t pkt_rtc, int64_t pts) {
        if (pts == AV_NOPTS_VALUE) return pkt_rtc;
        double pts_rtc = static_cast<double>(pts) * av_q2d(tb) * 1e7;
        // SRS recorders: PTS is already on the RTC clock (sanity-checked
        // against the packet time in case the flag is set but wrong).
        if (srs && std::fabs(pts_rtc - static_cast<double>(pkt_rtc)) < 5e7)
            return static_cast<uint64_t>(pts_rtc);
        if (last_pts != AV_NOPTS_VALUE &&
            std::fabs(static_cast<double>(pts - last_pts) * av_q2d(tb)) > 2.0) {
            offsets.clear(); locked = false;
        }
        last_pts = pts;
        if (!locked) {
            offsets.push_back(static_cast<double>(pkt_rtc) - pts_rtc);
            std::vector<double> tmp = offsets;
            std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(tmp.size() / 2), tmp.end());
            offset = tmp[tmp.size() / 2];
            if (offsets.size() >= 32) locked = true;
        }
        double r = pts_rtc + offset;
        return r < 0 ? 0 : static_cast<uint64_t>(r);
    }
    void Reset() { offsets.clear(); locked = false; last_pts = AV_NOPTS_VALUE; }
};

} // namespace internal
} // namespace ch10
