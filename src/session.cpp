// src/session.cpp
#include "ch10/session.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>

#include "ch10/reader.h"

namespace ch10 {

namespace {

std::string HexWords(const std::vector<uint16_t>& w) {
    std::string s;
    s.reserve(w.size() * 5);
    char buf[8];
    for (std::size_t i = 0; i < w.size(); ++i) {
        std::snprintf(buf, sizeof buf, "%04X", w[i]);
        if (i) s += ' ';
        s += buf;
    }
    return s;
}

// Report progress roughly every megabyte so callbacks stay cheap.
struct Throttle {
    uint64_t last = 0;
    bool Report(const ProgressFn& fn, uint64_t done, uint64_t total) {
        if (!fn) return true;
        if (done - last < (1u << 20) && done != total) return true;
        last = done;
        return fn(done, total);
    }
};

} // namespace

// ---------------------------------------------------------------------------
Message1553Row MakeRow(uint64_t index, int channel, uint8_t packet_flags,
                       const ms1553::Message& m, const TimeCorrelator& time,
                       bool correlate_time) {
    Message1553Row r;
    r.index   = index;
    r.channel = channel;
    r.rtc     = m.rtc;

    char buf[64];
    if ((packet_flags & 0x40) == 0) {                     // RTC-based IPTS
        if (correlate_time) {
            r.time = time.RtcToString(m.rtc);
        } else {
            std::snprintf(buf, sizeof buf, "RTC %.6f s", m.rtc / 1e7);
            r.time = buf;
        }
    } else {                                              // secondary time
        uint64_t ns = SecondaryTimeToNs(m.ipts_bytes, (packet_flags >> 2) & 3);
        std::snprintf(buf, sizeof buf, "%.6f s", ns / 1e9);
        r.time = buf;
    }

    r.bus       = m.bsw.BusB() ? 'B' : 'A';
    r.type      = m.TypeName();
    r.cmd1      = m.cmd1.raw;
    r.cmd1_text = m.cmd1.ToString();
    r.rt        = m.cmd1.Rt();
    r.tr        = m.cmd1.Transmit() ? 'T' : 'R';
    r.sa        = m.cmd1.Sa();
    r.wc_mc     = m.cmd1.WcMc();
    if (m.has_cmd2) {
        r.has_cmd2  = true;
        r.cmd2      = m.cmd2.raw;
        r.cmd2_text = m.cmd2.ToString();
    }
    if (m.has_status1) {
        r.has_status1   = true;
        r.status1       = m.status1.raw;
        r.status1_flags = m.status1.Flags();
    }
    if (m.has_status2) {
        r.has_status2   = true;
        r.status2       = m.status2.raw;
        r.status2_flags = m.status2.Flags();
    }
    r.errors = m.bsw.ErrorFlags();
    r.gap1   = m.gap1;
    r.gap2   = m.gap2;
    r.data   = (m.data.empty() && m.bsw.AnyError()) ? m.words : m.data;
    r.data_hex = HexWords(r.data);
    return r;
}

// ---------------------------------------------------------------------------
void Session::Close() {
    open_ = false;
    path_.clear();
    error_.clear();
    file_size_ = packet_count_ = resync_count_ = 0;
    tmats_ = Tmats();
    time_  = TimeCorrelator();
    channels_.clear();
    index_.clear();
    by_time_.clear();
    by_channel_.clear();
    activity_.clear();
    rtc_min_ = rtc_max_ = 0;
    time_packets_ = 0;
    reader_.reset();
    annotations_.clear();
    anomalies_.clear();
    anomaly_total_ = 0;
}

void Session::NoteAnomaly(uint64_t rtc, int channel, uint64_t packet_index,
                          const std::string& text) {
    ++anomaly_total_;
    if (anomalies_.size() >= 20000) return;
    Annotation a;
    a.rtc_start = a.rtc_end = rtc;
    a.channel = channel;
    a.severity = Severity::Anomaly;
    a.source = AnnotationSource::Auto;
    a.text = text;
    a.packet_index = packet_index;
    anomalies_.push_back(std::move(a));
}

void Session::ScanEventPacket(const Packet& p, uint64_t index) {
    // Recording Event, Format 2 (10.6.7.3): CSDW bit 31 = IPDH present,
    // bits 11-0 = entry count; entries are IPTS(8) [+IPDH(8)] + 4-byte entry.
    if (p.BodyLen() < 4) return;
    uint32_t csdw = le32(p.Body());
    bool ipdh = (csdw >> 31) & 1;
    uint32_t count = csdw & 0xFFF;
    std::size_t stride = 8 + (ipdh ? 8 : 0) + 4;
    std::size_t pos = 4;
    for (uint32_t i = 0; i < count && pos + stride <= p.BodyLen(); ++i, pos += stride) {
        uint64_t rtc = p.header.IptsUsesSecondaryTime() ? p.header.rtc
                       : (le64(p.Body() + pos) & 0x0000FFFFFFFFFFFFull);
        uint32_t entry = le32(p.Body() + pos + stride - 4);
        uint32_t number = entry & 0xFFF;
        uint32_t occurrence = (entry >> 12) & 0xFFFF;
        bool during = (entry >> 28) & 1;
        Annotation a;
        a.rtc_start = a.rtc_end = rtc;
        a.channel = p.header.channel_id;
        a.severity = Severity::Marker;
        a.source = AnnotationSource::Event;
        a.packet_index = index;
        std::string desc = tmats_.Get("R-1\\EV\\D-" + std::to_string(number + 1));
        if (desc.empty()) desc = tmats_.Get("R-1\\EV\\ID-" + std::to_string(number + 1));
        a.text = "Event " + std::to_string(number) + (desc.empty() ? "" : ": " + desc) +
                 " (occurrence " + std::to_string(occurrence) +
                 (during ? ")" : ", between recordings)");
        annotations_.push_back(std::move(a));
    }
}

void Session::AddAnnotation(const Annotation& a) {
    annotations_.push_back(a);
    std::stable_sort(annotations_.begin(), annotations_.end(),
                     [](const Annotation& x, const Annotation& y) {
                         return x.rtc_start < y.rtc_start;
                     });
}

bool Session::RemoveAnnotation(std::size_t i) {
    if (i >= annotations_.size() || annotations_[i].source != AnnotationSource::User)
        return false;
    annotations_.erase(annotations_.begin() + static_cast<std::ptrdiff_t>(i));
    return true;
}

std::vector<Annotation> Session::UserAnnotations() const {
    std::vector<Annotation> v;
    for (const auto& a : annotations_)
        if (a.source == AnnotationSource::User) v.push_back(a);
    return v;
}

bool Session::Open(const std::filesystem::path& path,
                   const ProgressFn& progress) {
    Close();
    Reader rd(path);
    if (!rd.IsOpen()) return Fail(rd.Error());

    path_      = path;
    file_size_ = static_cast<uint64_t>(rd.file_size());

    std::map<int, ChannelStats> stats;
    std::map<int, int> last_seq;
    std::vector<Packet> event_packets;      // decoded after TMATS is final
    Throttle th;
    Packet p;
    bool tmats_done = false;

    while (rd.Next(p)) {
        ++packet_count_;
        ChannelStats& cs = stats[p.header.channel_id];
        cs.channel_id = p.header.channel_id;
        cs.data_type  = p.header.data_type;
        ++cs.packets;
        cs.bytes += p.header.packet_len;

        PacketIndexEntry e;
        e.offset     = static_cast<uint64_t>(p.file_offset);
        e.rtc        = p.header.rtc;
        e.packet_len = p.header.packet_len;
        e.data_len   = p.header.data_len;
        e.channel    = p.header.channel_id;
        e.data_type  = p.header.data_type;
        e.seq        = p.header.seq_num;
        e.flags      = p.header.flags;
        index_.push_back(e);
        const uint64_t pidx = index_.size() - 1;
        if (index_.size() == 1 || e.rtc < rtc_min_) rtc_min_ = e.rtc;
        if (e.rtc > rtc_max_) rtc_max_ = e.rtc;

        // --- anomaly checks on every packet
        auto ls = last_seq.find(e.channel);
        if (ls != last_seq.end() && ((ls->second + 1) & 0xFF) != e.seq)
            NoteAnomaly(e.rtc, e.channel, pidx,
                        "Sequence gap: expected " + std::to_string((ls->second + 1) & 0xFF) +
                        ", got " + std::to_string(e.seq) + " (packets lost?)");
        last_seq[e.channel] = e.seq;
        if (p.header.RtcSyncError()) NoteAnomaly(e.rtc, e.channel, pidx, "RTC sync error flag set");
        if (p.header.DataOverflow()) NoteAnomaly(e.rtc, e.channel, pidx, "Data overflow flag set");

        switch (p.header.data_type) {
            case kCompGenF1_Tmats:
                if (!tmats_done) tmats_.AddPacket(p.Body(), p.BodyLen());
                break;
            case kTimeF1: {
                tmats_done = true;
                ++time_packets_;
                if (!time_.HasReference()) {
                    TimeF1 t;
                    if (DecodeTimeF1(p.Body(), p.BodyLen(), t))
                        time_.SetReference(p.header.rtc, t);
                }
                break;
            }
            case kMilStd1553F1:
                tmats_done = true;
                if (p.BodyLen() >= 4) {
                    uint32_t n = le32(p.Body()) & 0x00FFFFFF;
                    cs.messages += n;
                    // Walk the messages for block-status errors (cheap: no decode).
                    std::size_t pos = 4;
                    for (uint32_t m = 0; m < n && pos + 14 <= p.BodyLen(); ++m) {
                        ms1553::BlockStatus bsw; bsw.raw = le16(p.Body() + pos + 8);
                        uint16_t len = le16(p.Body() + pos + 12);
                        if (bsw.AnyError()) {
                            uint64_t mrtc = p.header.IptsUsesSecondaryTime() ? p.header.rtc
                                            : (le64(p.Body() + pos) & 0x0000FFFFFFFFFFFFull);
                            std::string cmd;
                            if (pos + 14 + 2 <= p.BodyLen()) {
                                ms1553::CommandWord c; c.raw = le16(p.Body() + pos + 14);
                                cmd = " " + c.ToString();
                            }
                            NoteAnomaly(mrtc, e.channel, pidx,
                                        "1553 error [" + bsw.ErrorFlags() + "]" + cmd);
                        }
                        pos += 14 + len;
                    }
                }
                break;
            case kEthernetF0:
            case 0x69:
            case kMessageF0:
                tmats_done = true;
                if (p.BodyLen() >= 4) cs.messages += le32(p.Body()) & 0xFFFF;   // frame / message count
                break;
            case kCompGenF0_UserDefined:
                tmats_done = true;
                if (IsAnnotationBody(p.Body(), p.BodyLen())) {
                    Annotation a;
                    if (DecodeAnnotationBody(p.Body(), p.BodyLen(), a)) {
                        a.packet_index = pidx;
                        annotations_.push_back(std::move(a));
                    }
                }
                break;
            case kCompGenF2_Events:
                tmats_done = true;
                if (event_packets.size() < 5000) event_packets.push_back(p);
                break;
            case kVideoF0:
            case kVideoF1:
                tmats_done = true;
                if (p.BodyLen() >= 4) {
                    if (cs.packets == 1)
                        cs.video = video::DecodeCsdw(p.header.data_type,
                                                     le32(p.Body()));
                    // count MPEG packets without copying them
                    if (cs.video.transport) {
                        std::size_t stride = (cs.video.iph ? 8 : 0) + 188;
                        cs.messages += (p.BodyLen() - 4) / stride;
                    }
                }
                break;
            default:
                tmats_done = true;
                break;
        }

        if (!th.Report(progress, static_cast<uint64_t>(p.file_offset) +
                                     p.header.packet_len, file_size_))
            return Fail("cancelled");
    }
    resync_count_ = rd.resync_count();
    tmats_.Finalize();

    for (const auto& ep : event_packets) {
        auto it = std::lower_bound(index_.begin(), index_.end(),
                                   static_cast<uint64_t>(ep.file_offset),
                                   [](const PacketIndexEntry& x, uint64_t off) { return x.offset < off; });
        ScanEventPacket(ep, static_cast<uint64_t>(it - index_.begin()));
    }
    for (std::streamoff off : rd.resync_offsets()) {
        auto it = std::lower_bound(index_.begin(), index_.end(), static_cast<uint64_t>(off),
                                   [](const PacketIndexEntry& x, uint64_t o) { return x.offset < o; });
        uint64_t rtc = it != index_.end() ? it->rtc : rtc_max_;
        int ch = it != index_.end() ? it->channel : -1;
        char buf[64];
        std::snprintf(buf, sizeof buf, "Corrupt data at offset 0x%llX, resynced",
                      static_cast<unsigned long long>(off));
        NoteAnomaly(rtc, ch, static_cast<uint64_t>(it - index_.begin()), buf);
    }
    std::stable_sort(annotations_.begin(), annotations_.end(),
                     [](const Annotation& x, const Annotation& y) { return x.rtc_start < y.rtc_start; });
    std::stable_sort(anomalies_.begin(), anomalies_.end(),
                     [](const Annotation& x, const Annotation& y) { return x.rtc_start < y.rtc_start; });

    // Time-ordered view and per-channel views of the index.
    by_time_.resize(index_.size());
    for (uint32_t i = 0; i < by_time_.size(); ++i) by_time_[i] = i;
    std::stable_sort(by_time_.begin(), by_time_.end(),
                     [this](uint32_t a, uint32_t b) {
                         return index_[a].rtc < index_[b].rtc;
                     });
    for (uint32_t i = 0; i < index_.size(); ++i)
        by_channel_[index_[i].channel].push_back(i);

    // Data-flow bins: bytes per channel per time slice.
    const double span = static_cast<double>(rtc_max_ - rtc_min_);
    for (auto& kv : stats) {
        ActivityRow row;
        row.channel_id = kv.first;
        row.data_type  = kv.second.data_type;
        row.bytes.assign(kActivityBins, 0);
        activity_.push_back(std::move(row));
    }
    for (const auto& e : index_) {
        int bin = span > 0 ? static_cast<int>((e.rtc - rtc_min_) / span *
                                              (kActivityBins - 1)) : 0;
        for (auto& row : activity_)
            if (row.channel_id == e.channel) {
                row.bytes[bin] += e.packet_len;
                row.max_bytes = std::max(row.max_bytes, row.bytes[bin]);
                break;
            }
    }
    reader_ = std::make_unique<Reader>(path_);

    channels_.clear();
    for (auto& kv : stats) {
        ChannelStats cs = kv.second;
        auto it = tmats_.channels().find(cs.channel_id);
        if (it != tmats_.channels().end()) {
            cs.source     = it->second.source;
            cs.tmats_type = it->second.type;
        }
        channels_.push_back(cs);
    }
    open_ = true;
    return true;
}

std::vector<int> Session::ChannelsOfType(uint8_t data_type) const {
    std::vector<int> ids;
    for (const auto& c : channels_)
        if (c.data_type == data_type) ids.push_back(c.channel_id);
    return ids;
}

const ChannelStats* Session::Channel(int id) const {
    for (const auto& c : channels_)
        if (c.channel_id == id) return &c;
    return nullptr;
}

std::string Session::RelativeString(uint64_t rtc) const {
    int64_t ms = (static_cast<int64_t>(rtc) - static_cast<int64_t>(rtc_min_)) / 10000;
    char sign = ms < 0 ? '-' : '+';
    if (ms < 0) ms = -ms;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%c%02lld:%02lld:%02lld.%03lld", sign,
                  static_cast<long long>(ms / 3600000),
                  static_cast<long long>((ms / 60000) % 60),
                  static_cast<long long>((ms / 1000) % 60),
                  static_cast<long long>(ms % 1000));
    return buf;
}

std::size_t Session::FindByTime(uint64_t rtc, int channel) const {
    if (channel < 0) {
        auto it = std::lower_bound(by_time_.begin(), by_time_.end(), rtc,
                                   [this](uint32_t i, uint64_t t) {
                                       return index_[i].rtc < t;
                                   });
        return it == by_time_.end() ? index_.size() : *it;
    }
    const auto& v = ChannelPackets(channel);
    auto it = std::lower_bound(v.begin(), v.end(), rtc,
                               [this](uint32_t i, uint64_t t) {
                                   return index_[i].rtc < t;
                               });
    return it == v.end() ? index_.size() : *it;
}

const std::vector<uint32_t>& Session::ChannelPackets(int channel) const {
    static const std::vector<uint32_t> kEmpty;
    auto it = by_channel_.find(channel);
    return it == by_channel_.end() ? kEmpty : it->second;
}

bool Session::ReadPacket(std::size_t i, Packet& out) {
    if (!reader_ || i >= index_.size()) return false;
    return reader_->ReadAt(static_cast<std::streamoff>(index_[i].offset), out);
}

// ---------------------------------------------------------------------------
bool Session::ExportTmats(const std::filesystem::path& out) const {
    if (tmats_.empty()) return false;
    std::ofstream f(out, std::ios::binary);
    if (!f) return false;
    f.write(tmats_.text().data(),
            static_cast<std::streamsize>(tmats_.text().size()));
    return static_cast<bool>(f);
}

// ---------------------------------------------------------------------------
bool Session::ForEach1553(int channel, long max_msgs, bool correlate_time,
                          const std::function<bool(const Message1553Row&)>& fn,
                          const ProgressFn& progress) {
    if (!open_) return Fail("no file open");
    Reader rd(path_);
    if (!rd.IsOpen()) return Fail(rd.Error());

    Throttle th;
    Packet p;
    uint64_t count = 0;
    while (rd.Next(p)) {
        if (!th.Report(progress, static_cast<uint64_t>(p.file_offset) +
                                     p.header.packet_len, file_size_))
            return Fail("cancelled");
        if (p.header.data_type != kMilStd1553F1) continue;
        if (channel >= 0 && p.header.channel_id != channel) continue;

        ms1553::PacketContent pc;
        ms1553::DecodePacket(p.Body(), p.BodyLen(), p.header.flags, pc);
        // On a malformed packet keep whatever messages decoded before the
        // error and carry on with the next packet.
        for (const auto& m : pc.messages) {
            if (max_msgs >= 0 && static_cast<long>(count) >= max_msgs)
                return true;
            ++count;
            if (!fn(MakeRow(count, p.header.channel_id, p.header.flags, m,
                            time_, correlate_time)))
                return true;
        }
    }
    return true;
}

bool Session::Load1553(int channel, long max_msgs, bool correlate_time,
                       std::vector<Message1553Row>& out,
                       const ProgressFn& progress) {
    out.clear();
    return ForEach1553(channel, max_msgs, correlate_time,
                       [&out](const Message1553Row& r) {
                           out.push_back(r);
                           return true;
                       },
                       progress);
}

std::string Session::CsvHeader() {
    return "msg_index,channel,time,bus,type,rt,tr,sa,wc_mc,cmd1,cmd2,"
           "status1,status2,errors,gap1_0.1us,gap2_0.1us,word_count,data_hex\n";
}

std::string Session::CsvLine(const Message1553Row& r) {
    char buf[512];
    std::string cmd2 = r.has_cmd2 ?
        (std::snprintf(buf, sizeof buf, "0x%04X", r.cmd2), std::string(buf)) : "";
    std::string st1 = r.has_status1 ?
        (std::snprintf(buf, sizeof buf, "0x%04X", r.status1), std::string(buf)) : "";
    std::string st2 = r.has_status2 ?
        (std::snprintf(buf, sizeof buf, "0x%04X", r.status2), std::string(buf)) : "";
    std::snprintf(buf, sizeof buf,
                  "%llu,%d,%s,%c,%s,%d,%c,%d,%d,0x%04X,%s,%s,%s,%s,%d,%d,%zu,",
                  static_cast<unsigned long long>(r.index), r.channel,
                  r.time.c_str(), r.bus, r.type.c_str(), r.rt, r.tr, r.sa,
                  r.wc_mc, r.cmd1, cmd2.c_str(), st1.c_str(), st2.c_str(),
                  r.errors.c_str(), r.gap1, r.gap2, r.data.size());
    return std::string(buf) + r.data_hex + "\n";
}

bool Session::Export1553Csv(const std::filesystem::path& out, int channel,
                            bool correlate_time, const ProgressFn& progress) {
    std::ofstream f(out, std::ios::binary);
    if (!f) return Fail("cannot write " + out.string());
    f << CsvHeader();
    bool ok = ForEach1553(channel, -1, correlate_time,
                          [&f](const Message1553Row& r) {
                              f << CsvLine(r);
                              return static_cast<bool>(f);
                          },
                          progress);
    return ok && f.good();
}

// ---------------------------------------------------------------------------
bool Session::ExtractVideo(int channel, const std::filesystem::path& out,
                           VideoResult& result, const ProgressFn& progress) {
    if (!open_) return Fail("no file open");
    Reader rd(path_);
    if (!rd.IsOpen()) return Fail(rd.Error());
    std::ofstream f(out, std::ios::binary);
    if (!f) return Fail("cannot write " + out.string());

    result = VideoResult();
    Throttle th;
    Packet p;
    std::vector<uint8_t> chunk;
    while (rd.Next(p)) {
        if (!th.Report(progress, static_cast<uint64_t>(p.file_offset) +
                                     p.header.packet_len, file_size_))
            return Fail("cancelled");
        if (p.header.data_type != kVideoF0 && p.header.data_type != kVideoF1)
            continue;
        if (p.header.channel_id != channel) continue;

        chunk.clear();
        bool raw = false;
        result.packets += video::Extract(p.header.data_type, p.Body(),
                                         p.BodyLen(), chunk, &raw);
        if (raw) result.raw_copy_warning = true;
        f.write(reinterpret_cast<const char*>(chunk.data()),
                static_cast<std::streamsize>(chunk.size()));
        result.bytes += chunk.size();
    }
    return f.good();
}

} // namespace ch10
