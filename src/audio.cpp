// src/audio.cpp
#include "ch10/audio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <unordered_map>

#include "ch10/ms1553_f1.h"
#include "ch10/reader.h"

#ifdef CH10_HAVE_FFMPEG
#include "ts_feeder.h"
#endif

namespace ch10 {

namespace {

int16_t MuLawToPcm(uint8_t u) {
    u = ~u;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return static_cast<int16_t>((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

int16_t ALawToPcm(uint8_t a) {
    a ^= 0x55;
    int t = (a & 0x0F) << 4;
    int seg = (a & 0x70) >> 4;
    if (seg == 0) t += 8; else if (seg == 1) t += 0x108; else t = (t + 0x108) << (seg - 1);
    return static_cast<int16_t>((a & 0x80) ? t : -t);
}

} // namespace

// ===========================================================================
// Analog F1
// ===========================================================================
const char* AnalogInfo::ModeName() const {
    switch (mode) {
        case 0: return "packed";
        case 1: return "unpacked, LSB padded";
        case 3: return "unpacked, MSB padded";
        default: return "reserved";
    }
}

AnalogInfo DecodeAnalogCsdw(uint32_t w) {          // Figure 10-31
    AnalogInfo i;
    i.same    = (w >> 28) & 1;
    i.factor  = (w >> 24) & 0xF;
    i.totchan = (w >> 16) & 0xFF; if (i.totchan == 0) i.totchan = 256;
    i.subchan = (w >> 8) & 0xFF;
    i.bits    = (w >> 2) & 0x3F; if (i.bits == 0) i.bits = 64;
    i.mode    = w & 0x3;
    return i;
}

bool AnalogAudioSource::Open(const Session& s, int channel, int subchannel_index,
                             bool signed_samples, double rate_override) {
    session_ = &s;
    pkts_ = s.ChannelPackets(channel);
    sub_ = subchannel_index;
    signed_ = signed_samples;
    cache_.clear();
    const ChannelStats* cs = s.Channel(channel);
    if (!cs || cs->data_type != kAnalogF1 || pkts_.empty()) { error_ = "not an analog channel"; return false; }
    reader_ = std::make_unique<Reader>(s.path());
    if (!reader_->IsOpen()) { error_ = reader_->Error(); return false; }

    // Inspect the first packet for the CSDW and sample count.
    Packet p;
    if (!reader_->ReadAt(static_cast<std::streamoff>(s.packets()[pkts_.front()].offset), p) || p.BodyLen() < 4) {
        error_ = "cannot read first analog packet"; return false;
    }
    ainfo_ = DecodeAnalogCsdw(le32(p.Body()));
    if (ainfo_.mode == 0 && ainfo_.bits % 8 != 0) {
        error_ = "packed analog samples with a non-byte width are not supported"; return false;
    }
    if (sub_ >= ainfo_.totchan) sub_ = 0;
    int ncsdw = ainfo_.same ? 1 : ainfo_.totchan;
    int container = ainfo_.mode == 0 ? ainfo_.bits / 8 : (ainfo_.bits <= 8 ? 1 : ainfo_.bits <= 16 ? 2 : ainfo_.bits <= 32 ? 4 : 8);
    ainfo_.samples_per_packet = static_cast<int>((p.BodyLen() - 4 * ncsdw) / container / ainfo_.totchan);

    // Sample rate: TMATS R-x\ASR-n-m if we can find it, else measured.
    rate_ = rate_override;
    if (rate_ <= 0) {
        for (const auto& kv : s.tmats().attributes()) {
            // R-1\TK1-n : channel  ->  R-1\ASR-n-1
            if (kv.first.compare(0, 2, "R-") != 0) continue;
            std::size_t bs = kv.first.find('\\');
            if (bs == std::string::npos || kv.first.compare(bs + 1, 4, "TK1-") != 0) continue;
            if (std::atoi(kv.second.c_str()) != channel) continue;
            std::string n = kv.first.substr(bs + 5);
            std::string asr = s.tmats().Get(kv.first.substr(0, bs) + "\\ASR-" + n + "-" +
                                            std::to_string(sub_ + 1));
            if (asr.empty()) asr = s.tmats().Get(kv.first.substr(0, bs) + "\\ASR-" + n + "-1");
            if (!asr.empty()) rate_ = std::atof(asr.c_str());
            break;
        }
    }
    if (rate_ <= 0 && pkts_.size() >= 2) {
        std::size_t k = std::min<std::size_t>(pkts_.size() - 1, 50);
        uint64_t span = s.packets()[pkts_[k]].rtc - s.packets()[pkts_[0]].rtc;
        if (span > 0) rate_ = static_cast<double>(ainfo_.samples_per_packet) * k * 1e7 / span;
    }
    if (rate_ <= 0) { error_ = "cannot determine the analog sample rate"; return false; }

    first_rtc_ = s.packets()[pkts_.front()].rtc;
    last_rtc_  = s.packets()[pkts_.back()].rtc + static_cast<uint64_t>(ainfo_.samples_per_packet / rate_ * 1e7);
    char buf[160];
    std::snprintf(buf, sizeof buf, "%d-bit %s, %d subchannel%s, %.0f Hz (%s)",
                  ainfo_.bits, ainfo_.ModeName(), ainfo_.totchan, ainfo_.totchan == 1 ? "" : "s",
                  rate_, rate_override > 0 ? "set" : "from TMATS/measured");
    info_ = buf;
    name_ = "Analog channel " + std::to_string(channel) + (cs->source.empty() ? "" : " - " + cs->source);
    return true;
}

bool AnalogAudioSource::DecodePacket(uint32_t index) {
    for (auto& c : cache_) if (c.first == index) return true;
    Packet p;
    if (!reader_->ReadAt(static_cast<std::streamoff>(session_->packets()[index].offset), p)) return false;
    if (p.BodyLen() < 4) return false;
    AnalogInfo ai = DecodeAnalogCsdw(le32(p.Body()));
    int ncsdw = ai.same ? 1 : ai.totchan;
    const uint8_t* d = p.Body() + 4 * ncsdw;
    std::size_t len = p.BodyLen() - 4 * ncsdw;
    int bits = ai.bits;
    int container = ai.mode == 0 ? bits / 8 : (bits <= 8 ? 1 : bits <= 16 ? 2 : bits <= 32 ? 4 : 8);
    if (container == 0) return false;
    std::size_t total = len / container;
    std::vector<float> out;
    out.reserve(total / ai.totchan + 1);
    const double scale = 1.0 / std::pow(2.0, bits - 1);
    for (std::size_t i = static_cast<std::size_t>(sub_); i < total; i += ai.totchan) {
        const uint8_t* q = d + i * container;
        int64_t raw = 0;
        for (int b = container - 1; b >= 0; --b) raw = (raw << 8) | q[b];   // little-endian
        int64_t v;
        if (ai.mode == 1 && container * 8 > bits) raw >>= (container * 8 - bits);   // LSB padded
        if (signed_) {
            int64_t sign = int64_t(1) << (bits - 1);
            v = (raw & (sign - 1)) - (raw & sign);
        } else {
            v = raw - (int64_t(1) << (bits - 1));
        }
        out.push_back(static_cast<float>(v * scale));
    }
    if (cache_.size() >= 96) cache_.erase(cache_.begin(), cache_.begin() + 48);
    cache_.emplace_back(index, std::move(out));
    return true;
}

int AnalogAudioSource::Render(double rtc, double step, int frames, float* out) {
    std::fill(out, out + frames, 0.0f);
    if (pkts_.empty() || rate_ <= 0) return 0;
    const auto& idx = session_->packets();
    const double period = 1e7 / rate_;
    int got = 0;
    double t = rtc;
    const std::vector<float>* cur = nullptr;
    double cur_t0 = 0, cur_end = 0;
    for (int i = 0; i < frames; ++i, t += step) {
        if (t < 0) continue;
        if (!cur || t < cur_t0 || t >= cur_end) {
            cur = nullptr;
            auto it = std::upper_bound(pkts_.begin(), pkts_.end(), static_cast<uint64_t>(t),
                                       [&idx](uint64_t v, uint32_t k) { return v < idx[k].rtc; });
            if (it == pkts_.begin()) continue;
            --it;
            uint32_t pi = *it;
            if (!DecodePacket(pi)) continue;
            for (auto& c : cache_) if (c.first == pi) cur = &c.second;
            if (!cur) continue;
            cur_t0 = static_cast<double>(idx[pi].rtc);
            cur_end = cur_t0 + static_cast<double>(cur->size()) * period;
            if (t >= cur_end) { cur = nullptr; continue; }      // in a gap between packets
        }
        double pos = (t - cur_t0) / period;
        std::size_t k = static_cast<std::size_t>(pos);
        if (k >= cur->size()) continue;
        float a = (*cur)[k], b = k + 1 < cur->size() ? (*cur)[k + 1] : a;
        out[i] = a + (b - a) * static_cast<float>(pos - k);
        ++got;
    }
    return got;
}

// ===========================================================================
// 1553 data-word audio
// ===========================================================================
bool Ms1553AudioSource::Open(const Session& s, const Ms1553AudioSpec& spec,
                             const ProgressFn& progress) {
    msgs_.clear(); samples_.clear();
    const ChannelStats* cs = s.Channel(spec.channel);
    if (!cs || cs->data_type != kMilStd1553F1) { error_ = "not a 1553 channel"; return false; }
    Reader rd(s.path());
    if (!rd.IsOpen()) { error_ = rd.Error(); return false; }
    const auto& pk = s.ChannelPackets(spec.channel);
    Packet p;
    for (std::size_t n = 0; n < pk.size(); ++n) {
        if (progress && (n % 64 == 0) && !progress(n, pk.size())) { error_ = "cancelled"; return false; }
        if (!rd.ReadAt(static_cast<std::streamoff>(s.packets()[pk[n]].offset), p)) continue;
        ms1553::PacketContent pc;
        ms1553::DecodePacket(p.Body(), p.BodyLen(), p.header.flags, pc);
        for (const auto& m : pc.messages) {
            if (m.bsw.AnyError()) continue;
            const ms1553::CommandWord& c = m.cmd1;
            if (spec.rt >= 0 && c.Rt() != spec.rt && !(m.has_cmd2 && m.cmd2.Rt() == spec.rt)) continue;
            if (spec.sa >= 0 && c.Sa() != spec.sa && !(m.has_cmd2 && m.cmd2.Sa() == spec.sa)) continue;
            if (spec.tr == 0 && c.Transmit()) continue;
            if (spec.tr == 1 && !c.Transmit()) continue;
            int first = std::max(0, spec.first_word);
            int count = spec.word_count < 0 ? static_cast<int>(m.data.size()) - first
                                            : std::min<int>(spec.word_count, static_cast<int>(m.data.size()) - first);
            if (count <= 0) continue;
            Msg msg; msg.rtc = m.rtc; msg.offset = static_cast<uint32_t>(samples_.size());
            for (int w = first; w < first + count; ++w) {
                uint16_t v = m.data[static_cast<std::size_t>(w)];
                switch (spec.format) {
                    case Ms1553AudioSpec::S16:   samples_.push_back(static_cast<int16_t>(v)); break;
                    case Ms1553AudioSpec::U16:   samples_.push_back(static_cast<int16_t>(static_cast<int>(v) - 32768)); break;
                    case Ms1553AudioSpec::S8x2:  samples_.push_back(static_cast<int16_t>(static_cast<int8_t>(v >> 8)) * 256);
                                                 samples_.push_back(static_cast<int16_t>(static_cast<int8_t>(v & 0xFF)) * 256); break;
                    case Ms1553AudioSpec::U8x2:  samples_.push_back(static_cast<int16_t>(((v >> 8) - 128) * 256));
                                                 samples_.push_back(static_cast<int16_t>(((v & 0xFF) - 128) * 256)); break;
                    case Ms1553AudioSpec::MuLawx2: samples_.push_back(MuLawToPcm(v >> 8)); samples_.push_back(MuLawToPcm(v & 0xFF)); break;
                    case Ms1553AudioSpec::ALawx2:  samples_.push_back(ALawToPcm(v >> 8));  samples_.push_back(ALawToPcm(v & 0xFF)); break;
                }
            }
            msg.count = static_cast<uint32_t>(samples_.size() - msg.offset);
            msg.period = 0;
            msgs_.push_back(msg);
        }
    }
    if (msgs_.empty()) { error_ = "no matching 1553 messages"; return false; }
    std::stable_sort(msgs_.begin(), msgs_.end(), [](const Msg& a, const Msg& b) { return a.rtc < b.rtc; });

    // Sample rate: given, or the median samples-per-second between messages.
    rate_ = spec.sample_rate;
    if (rate_ <= 0 && msgs_.size() >= 2) {
        std::vector<double> rates;
        for (std::size_t i = 1; i < msgs_.size() && rates.size() < 2000; ++i) {
            uint64_t dt = msgs_[i].rtc - msgs_[i - 1].rtc;
            if (dt > 0) rates.push_back(msgs_[i - 1].count * 1e7 / dt);
        }
        if (!rates.empty()) { std::nth_element(rates.begin(), rates.begin() + rates.size() / 2, rates.end()); rate_ = rates[rates.size() / 2]; }
    }
    if (rate_ <= 0) rate_ = 8000;
    // Per-message period so samples span exactly to the next message.
    for (std::size_t i = 0; i < msgs_.size(); ++i) {
        double nominal = 1e7 / rate_;
        msgs_[i].period = nominal;
        if (i + 1 < msgs_.size()) {
            double span = static_cast<double>(msgs_[i + 1].rtc - msgs_[i].rtc);
            double per = span / msgs_[i].count;
            if (per > 0 && per < nominal * 2) msgs_[i].period = per;
        }
    }
    first_rtc_ = msgs_.front().rtc;
    last_rtc_  = msgs_.back().rtc + static_cast<uint64_t>(msgs_.back().count * msgs_.back().period);
    char buf[200];
    std::snprintf(buf, sizeof buf, "%zu messages, %zu samples, %.0f Hz (%s)", msgs_.size(),
                  samples_.size(), rate_, spec.sample_rate > 0 ? "set" : "from message cadence");
    info_ = buf;
    name_ = "1553 ch " + std::to_string(spec.channel) +
            (spec.rt >= 0 ? " RT" + std::to_string(spec.rt) : "") +
            (spec.sa >= 0 ? " SA" + std::to_string(spec.sa) : "") + " audio";
    return true;
}

int Ms1553AudioSource::Render(double rtc, double step, int frames, float* out) {
    std::fill(out, out + frames, 0.0f);
    if (msgs_.empty()) return 0;
    int got = 0;
    double t = rtc;
    for (int i = 0; i < frames; ++i, t += step) {
        auto it = std::upper_bound(msgs_.begin(), msgs_.end(), static_cast<uint64_t>(std::max(0.0, t)),
                                   [](uint64_t v, const Msg& m) { return v < m.rtc; });
        if (it == msgs_.begin()) continue;
        --it;
        double pos = (t - static_cast<double>(it->rtc)) / it->period;
        if (pos < 0 || pos >= it->count) continue;
        std::size_t k = it->offset + static_cast<std::size_t>(pos);
        float a = samples_[k] / 32768.0f;
        float b = (k + 1 < it->offset + it->count) ? samples_[k + 1] / 32768.0f : a;
        out[i] = a + (b - a) * static_cast<float>(pos - static_cast<std::size_t>(pos));
        ++got;
    }
    return got;
}

// ===========================================================================
// Audio inside the video transport stream
// ===========================================================================
#ifdef CH10_HAVE_FFMPEG
struct TsAudioSource::Impl {
    internal::TsFeeder   feeder;
    internal::TsDecoder  dec;
    internal::FrameClock clock;
    std::unordered_map<int64_t, uint64_t> pts_rtc;
    uint64_t last_rtc = 0;
    struct Chunk { double rtc; int rate; int64_t pts; std::vector<float> mono; };
    std::deque<Chunk> window;           // decoded audio in time order
    double window_secs = 0;
    bool started = false;

    void Teardown() { dec.Teardown(); pts_rtc.clear(); clock.Reset(); window.clear(); window_secs = 0; started = false; }

    bool Restart(uint64_t rtc, std::string& err, std::string& codec) {
        Teardown();
        feeder.SeekTo(rtc);
        if (!dec.Setup(feeder, AVMEDIA_TYPE_AUDIO, err, codec)) { Teardown(); return false; }
        clock.tb = dec.time_base;
        clock.srs = feeder.srs;
        started = true;
        return true;
    }

    // Convert one AVFrame to mono float.
    static void ToMono(const AVFrame* f, std::vector<float>& out) {
        int n = f->nb_samples;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        int ch = f->ch_layout.nb_channels;
#else
        int ch = f->channels;
#endif
        out.assign(static_cast<std::size_t>(n), 0.0f);
        auto fmt = static_cast<AVSampleFormat>(f->format);
        bool planar = av_sample_fmt_is_planar(fmt) != 0;
        AVSampleFormat base = av_get_packed_sample_fmt(fmt);
        for (int c = 0; c < ch; ++c) {
            const uint8_t* plane = planar ? f->data[c] : f->data[0];
            for (int i = 0; i < n; ++i) {
                std::size_t idx = planar ? static_cast<std::size_t>(i) : static_cast<std::size_t>(i) * ch + c;
                float v = 0;
                switch (base) {
                    case AV_SAMPLE_FMT_U8:  v = (reinterpret_cast<const uint8_t*>(plane)[idx] - 128) / 128.0f; break;
                    case AV_SAMPLE_FMT_S16: v = reinterpret_cast<const int16_t*>(plane)[idx] / 32768.0f; break;
                    case AV_SAMPLE_FMT_S32: v = reinterpret_cast<const int32_t*>(plane)[idx] / 2147483648.0f; break;
                    case AV_SAMPLE_FMT_FLT: v = reinterpret_cast<const float*>(plane)[idx]; break;
                    case AV_SAMPLE_FMT_DBL: v = static_cast<float>(reinterpret_cast<const double*>(plane)[idx]); break;
                    default: break;
                }
                out[static_cast<std::size_t>(i)] += v / ch;
            }
        }
    }

    bool DecodeOne() {
        uint64_t pkt_rtc = 0;
        if (!dec.Next(feeder, pkt_rtc, pts_rtc, last_rtc)) return false;
        Chunk c;
        bool was_locked = clock.locked;
        c.pts = dec.frame->best_effort_timestamp;
        c.rtc = static_cast<double>(clock.Map(pkt_rtc, c.pts));
        c.rate = dec.frame->sample_rate;
        if (!was_locked && !clock.srs)          // offset moved: keep the window consistent
            for (Chunk& w : window) if (w.pts != AV_NOPTS_VALUE)
                w.rtc = static_cast<double>(w.pts) * av_q2d(clock.tb) * 1e7 + clock.offset;
        ToMono(dec.frame, c.mono);
        av_frame_unref(dec.frame);
        window_secs += static_cast<double>(c.mono.size()) / c.rate;
        window.push_back(std::move(c));
        while (window_secs > 12.0 && window.size() > 1) {
            window_secs -= static_cast<double>(window.front().mono.size()) / window.front().rate;
            window.pop_front();
        }
        return true;
    }
    double WindowLo() const { return window.empty() ? 0 : window.front().rtc; }
    double WindowHi() const {
        if (window.empty()) return 0;
        const Chunk& c = window.back();
        return c.rtc + c.mono.size() * 1e7 / c.rate;
    }
};

bool TsAudioSource::Available() { return true; }
TsAudioSource::TsAudioSource() : impl_(new Impl) {}
TsAudioSource::~TsAudioSource() { impl_->Teardown(); }

bool TsAudioSource::Probe(const Session& s, int channel, std::string* codec, int* rate, int* channels) {
    Impl im;
    if (!im.feeder.Open(s, channel)) return false;
    im.feeder.SeekTo(s.packets()[im.feeder.pkts.front()].rtc);
    std::string err, name;
    bool ok = im.dec.Setup(im.feeder, AVMEDIA_TYPE_AUDIO, err, name);
    if (ok) {
        if (codec) *codec = name;
        if (rate) *rate = im.dec.codec->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        if (channels) *channels = im.dec.codec->ch_layout.nb_channels;
#else
        if (channels) *channels = im.dec.codec->channels;
#endif
    }
    im.Teardown();
    return ok;
}

bool TsAudioSource::Open(const Session& s, int channel) {
    impl_->Teardown();
    const ChannelStats* cs = s.Channel(channel);
    if (!cs || !s.IsVideoType(cs->data_type) || !cs->video.transport) { error_ = "not a transport-stream video channel"; return false; }
    if (!impl_->feeder.Open(s, channel)) { error_ = "channel has no packets"; return false; }
    first_rtc_ = s.packets()[impl_->feeder.pkts.front()].rtc;
    last_rtc_  = s.packets()[impl_->feeder.pkts.back()].rtc;
    std::string codec; int rate = 0, ch = 0;
    if (!Probe(s, channel, &codec, &rate, &ch)) { error_ = "no audio stream in this channel"; return false; }
    rate_ = rate;
    info_ = codec + ", " + std::to_string(rate) + " Hz, " + std::to_string(ch) + " ch";
    name_ = "Video channel " + std::to_string(channel) + " audio" + (cs->source.empty() ? "" : " - " + cs->source);
    return true;
}

int TsAudioSource::Render(double rtc, double step, int frames, float* out) {
    std::fill(out, out + frames, 0.0f);
    Impl& im = *impl_;
    if (im.feeder.pkts.empty()) return 0;
    double t_end = rtc + step * frames;
    double lo = std::min(rtc, t_end), hi = std::max(rtc, t_end);
    const double lookback = 1.0e7;   // restart 1 s before the target

    // Make sure the window covers [lo, hi].
    if (!im.started || lo < im.WindowLo() - 1e6 || lo > im.WindowHi() + 3e7) {
        std::string err, codec;
        uint64_t start = lo > first_rtc_ + lookback ? static_cast<uint64_t>(lo - lookback) : first_rtc_;
        if (!im.Restart(start, err, codec)) { error_ = err; return 0; }
    }
    int guard = 0;
    while (im.WindowHi() < hi + 5e5 && guard++ < 400) {
        if (!im.DecodeOne()) break;
    }

    // Sample the window.
    int got = 0;
    double t = rtc;
    std::size_t ci = 0;
    for (int i = 0; i < frames; ++i, t += step) {
        // find chunk containing t (window is in time order)
        if (im.window.empty()) break;
        while (ci + 1 < im.window.size() && im.window[ci + 1].rtc <= t) ++ci;
        while (ci > 0 && im.window[ci].rtc > t) --ci;
        const Impl::Chunk& c = im.window[ci];
        double pos = (t - c.rtc) * c.rate / 1e7;
        if (pos < 0 || pos >= static_cast<double>(c.mono.size())) continue;
        std::size_t k = static_cast<std::size_t>(pos);
        float a = c.mono[k], b = k + 1 < c.mono.size() ? c.mono[k + 1] : a;
        out[i] = a + (b - a) * static_cast<float>(pos - k);
        ++got;
    }
    return got;
}

#else
struct TsAudioSource::Impl {};
bool TsAudioSource::Available() { return false; }
TsAudioSource::TsAudioSource() : impl_(new Impl) {}
TsAudioSource::~TsAudioSource() {}
bool TsAudioSource::Probe(const Session&, int, std::string*, int*, int*) { return false; }
bool TsAudioSource::Open(const Session&, int) { error_ = "built without FFmpeg"; return false; }
int TsAudioSource::Render(double, double, int frames, float* out) { std::fill(out, out + frames, 0.0f); return 0; }
#endif

// ===========================================================================
// WAV export
// ===========================================================================
bool WriteWav(AudioSource& src, uint64_t rtc_from, uint64_t rtc_to, int rate,
              const std::filesystem::path& out, const ProgressFn& progress) {
    if (rtc_to <= rtc_from || rate <= 0) return false;
    std::ofstream f(out, std::ios::binary);
    if (!f) return false;
    uint64_t total = static_cast<uint64_t>((rtc_to - rtc_from) / 1e7 * rate);
    auto put16 = [&f](uint16_t v) { char b[2] = {char(v & 0xFF), char(v >> 8)}; f.write(b, 2); };
    auto put32 = [&f](uint32_t v) { char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char(v >> 24)}; f.write(b, 4); };
    uint32_t data_bytes = static_cast<uint32_t>(total * 2);
    f.write("RIFF", 4); put32(36 + data_bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); put32(16); put16(1); put16(1); put32(static_cast<uint32_t>(rate));
    put32(static_cast<uint32_t>(rate * 2)); put16(2); put16(16);
    f.write("data", 4); put32(data_bytes);

    const int block = 4096;
    std::vector<float> buf(block);
    std::vector<int16_t> pcm(block);
    const double step = 1e7 / rate;
    double t = static_cast<double>(rtc_from);
    for (uint64_t done = 0; done < total; ) {
        int n = static_cast<int>(std::min<uint64_t>(block, total - done));
        src.Render(t, step, n, buf.data());
        for (int i = 0; i < n; ++i)
            pcm[static_cast<std::size_t>(i)] = static_cast<int16_t>(std::clamp(buf[static_cast<std::size_t>(i)], -1.0f, 1.0f) * 32767.0f);
        f.write(reinterpret_cast<const char*>(pcm.data()), n * 2);
        t += step * n; done += static_cast<uint64_t>(n);
        if (progress && !progress(done, total)) return false;
    }
    return f.good();
}

} // namespace ch10
