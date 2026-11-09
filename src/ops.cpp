// src/ops.cpp
#include "ch10/ops.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <queue>

#include "ch10/video.h"
#include "ch10/writer.h"

namespace ch10 {

namespace {

constexpr uint8_t kAnnotationFlags = 0x02;   // no secondary header, 16-bit checksum

void Store48(uint8_t* p, uint64_t v) { for (int i = 0; i < 6; ++i) p[i] = (v >> (8 * i)) & 0xFF; p[6] = 0; p[7] = 0; }

bool Fail(OpResult& r, const std::string& msg) { r.error = msg; return false; }

// --- write helpers ----------------------------------------------------------
// Write a packet from a (possibly modified) Packet: header fields from p,
// body from `body`.
bool WriteParts(Writer& w, const Packet& p, uint16_t channel, uint64_t rtc,
                const uint8_t* body, std::size_t len) {
    return w.Write(channel, p.header.data_type, p.header.data_type_ver, p.header.flags,
                   rtc, p.SecondaryTime(), body, len);
}

// Write the setup record(s) of a session, stamped with `rtc` (UINT64_MAX
// keeps the original) so a cut or a split part starts at its own time.
bool CopySetupRecords(Session& s, Writer& w, uint64_t rtc = UINT64_MAX) {
    Packet p;
    for (std::size_t i = 0; i < s.packets().size(); ++i) {
        if (s.packets()[i].data_type != kCompGenF1_Tmats) {
            if (i > 0) break;           // setup records are the first packets
            continue;
        }
        if (!s.ReadPacket(i, p) || !w.WriteCopy(p, -1, rtc, rtc != UINT64_MAX)) return false;
    }
    return true;
}

// Index of the last Time F1 packet at or before `rtc` (npos if none).
std::size_t LastTimePacketBefore(const Session& s, uint64_t rtc) {
    std::size_t best = std::string::npos;
    uint64_t best_rtc = 0;
    for (int ch : s.ChannelsOfType(kTimeF1)) {
        const auto& v = s.ChannelPackets(ch);
        auto it = std::upper_bound(v.begin(), v.end(), rtc,
                                   [&s](uint64_t t, uint32_t i) { return t < s.packets()[i].rtc; });
        if (it == v.begin()) continue;
        --it;
        uint64_t r = s.packets()[*it].rtc;
        if (best == std::string::npos || r > best_rtc) { best = *it; best_rtc = r; }
    }
    return best;
}

// --- 1553 message filtering -------------------------------------------------
// Rebuild a 1553 F1 body keeping only messages that pass `f`. Returns the
// number of messages kept.
uint32_t Filter1553(const uint8_t* body, std::size_t len, const Ms1553Filter& f,
                    std::vector<uint8_t>& out) {
    out.clear();
    if (len < 4) return 0;
    uint32_t csdw = le32(body);
    uint32_t n = csdw & 0x00FFFFFF;
    out.insert(out.end(), body, body + 4);        // CSDW, count patched below
    uint32_t kept = 0;
    std::size_t pos = 4;
    for (uint32_t m = 0; m < n && pos + 14 <= len; ++m) {
        uint16_t mlen = le16(body + pos + 12);
        std::size_t total = 14 + mlen;
        if (pos + total > len) break;
        ms1553::BlockStatus bsw; bsw.raw = le16(body + pos + 8);
        bool keep = true;
        if (mlen >= 2) {
            ms1553::CommandWord c; c.raw = le16(body + pos + 14);
            ms1553::CommandWord c2; bool has2 = bsw.RtToRt() && mlen >= 4;
            if (has2) c2.raw = le16(body + pos + 16);
            auto rt_ok = [&](const std::set<int>& s) {
                return s.empty() || s.count(c.Rt()) || (has2 && s.count(c2.Rt()));
            };
            auto sa_ok = [&](const std::set<int>& s) {
                return s.empty() || s.count(c.Sa()) || (has2 && s.count(c2.Sa()));
            };
            if (!rt_ok(f.rts)) keep = false;
            if (!sa_ok(f.sas)) keep = false;
            if (f.tr == 0 && c.Transmit()) keep = false;
            if (f.tr == 1 && !c.Transmit()) keep = false;
        }
        if (f.errors_only && !bsw.AnyError()) keep = false;
        if (keep) { out.insert(out.end(), body + pos, body + pos + total); ++kept; }
        pos += total;
    }
    uint32_t new_csdw = (csdw & 0xFF000000u) | (kept & 0x00FFFFFF);
    for (int i = 0; i < 4; ++i) out[i] = (new_csdw >> (8 * i)) & 0xFF;
    return kept;
}

// --- IPTS rebasing for merge -----------------------------------------------
// Shift every RTC-based intra-packet time stamp in `body` by delta. Returns
// false if the data type carries IPTS we do not know how to rewrite.
bool RebaseIpts(const PacketHeader& h, std::vector<uint8_t>& body, int64_t delta) {
    if (delta == 0 || h.IptsUsesSecondaryTime()) return true;
    auto shift = [&](std::size_t at) {
        if (at + 8 > body.size()) return;
        uint64_t v = le64(body.data() + at) & 0x0000FFFFFFFFFFFFull;
        Store48(body.data() + at, static_cast<uint64_t>(static_cast<int64_t>(v) + delta));
    };
    if (body.size() < 4) return true;
    uint32_t csdw = le32(body.data());
    switch (h.data_type) {
        case kMilStd1553F1: {
            uint32_t n = csdw & 0x00FFFFFF;
            std::size_t pos = 4;
            for (uint32_t m = 0; m < n && pos + 14 <= body.size(); ++m) {
                shift(pos);
                pos += 14 + le16(body.data() + pos + 12);
            }
            return true;
        }
        case kVideoF0:
        case kVideoF1: {
            video::Info vi = video::DecodeCsdw(h.data_type, csdw);
            if (!vi.iph) return true;
            if (!vi.transport) return false;
            for (std::size_t pos = 4; pos + 8 + 188 <= body.size(); pos += 8 + 188) shift(pos);
            return true;
        }
        case kCompGenF2_Events: {
            bool ipdh = (csdw >> 31) & 1;
            uint32_t n = csdw & 0xFFF;
            std::size_t stride = 8 + (ipdh ? 8 : 0) + 4;
            for (uint32_t i = 0, pos = 4; i < n && pos + stride <= body.size(); ++i, pos += static_cast<uint32_t>(stride))
                shift(pos);
            return true;
        }
        case kCompGenF0_UserDefined: {
            Annotation a;
            if (DecodeAnnotationBody(body.data(), body.size(), a)) {
                a.rtc_start = static_cast<uint64_t>(static_cast<int64_t>(a.rtc_start) + delta);
                a.rtc_end   = static_cast<uint64_t>(static_cast<int64_t>(a.rtc_end) + delta);
                body = EncodeAnnotationBody(a);
            }
            return true;
        }
        case kTimeF1:
        case kCompGenF1_Tmats:
        case kCompGenF3_Index:
            return true;                          // no IPTS
        default:
            return false;                         // PCM, ARINC, analog, ... : unknown layout
    }
}

// --- TMATS merging ----------------------------------------------------------
// Rewrite "R-x\..." codes of another file so they follow the base record:
// R-x becomes R-(x+rec_offset) and TK1 (channel ID) values gain chan_offset.
std::string RenumberedTmats(const Tmats& t, int rec_offset, int chan_offset, int& max_r) {
    std::string out;
    for (const auto& kv : t.attributes()) {
        std::string code = kv.first, val = kv.second;
        if (code.compare(0, 2, "R-") == 0) {
            std::size_t bs = code.find('\\');
            if (bs != std::string::npos) {
                int r = std::atoi(code.substr(2, bs - 2).c_str()) + rec_offset;
                max_r = std::max(max_r, r);
                std::string attr = code.substr(bs + 1);
                code = "R-" + std::to_string(r) + "\\" + attr;
                if (attr.compare(0, 4, "TK1-") == 0 && chan_offset != 0)
                    val = std::to_string(std::atoi(val.c_str()) + chan_offset);
            }
        } else if (code.compare(0, 2, "G\\") == 0) {
            continue;                              // general section from base only
        }
        out += code + ":" + val + ";\n";
    }
    return out;
}

int MaxRecorderIndex(const Tmats& t) {
    int m = 0;
    for (const auto& kv : t.attributes())
        if (kv.first.compare(0, 2, "R-") == 0)
            m = std::max(m, std::atoi(kv.first.c_str() + 2));
    return m;
}

} // namespace

// ===========================================================================
// 1553 message building (for injection)
// ===========================================================================
std::vector<uint8_t> Build1553Message(uint64_t rtc, bool bus_b, int rt, bool transmit, int sa,
                                      const std::vector<uint16_t>& data) {
    std::vector<uint8_t> m;
    auto put16 = [&m](uint16_t x) { m.push_back(x & 0xFF); m.push_back(x >> 8); };
    for (int i = 0; i < 8; ++i) m.push_back((rtc >> (8 * i)) & (i < 6 ? 0xFF : 0));   // IPTS
    std::size_t n = std::min<std::size_t>(data.size(), 32);
    uint16_t wc = static_cast<uint16_t>(n == 32 ? 0 : n);
    uint16_t cmd = static_cast<uint16_t>(((rt & 0x1F) << 11) | ((transmit ? 1 : 0) << 10) | ((sa & 0x1F) << 5) | wc);
    uint16_t status = static_cast<uint16_t>((rt & 0x1F) << 11);
    uint16_t words = static_cast<uint16_t>(2 + n);
    put16(bus_b ? 0x2000 : 0x0000);                    // BSW: bus id, no errors
    put16(0x0000);                                     // gap times
    put16(static_cast<uint16_t>(words * 2));           // length in bytes
    put16(cmd);
    if (transmit) { put16(status); for (std::size_t i = 0; i < n; ++i) put16(data[i]); }
    else          { for (std::size_t i = 0; i < n; ++i) put16(data[i]); put16(status); }
    return m;
}

std::vector<uint8_t> Build1553PacketBody(const std::vector<std::vector<uint8_t>>& messages) {
    std::vector<uint8_t> body;
    uint32_t csdw = static_cast<uint32_t>(messages.size() & 0x00FFFFFF);
    for (int i = 0; i < 4; ++i) body.push_back((csdw >> (8 * i)) & 0xFF);
    for (const auto& m : messages) body.insert(body.end(), m.begin(), m.end());
    return body;
}

// Append TMATS lines describing new channels to a copy of the setup record.
static std::string AugmentTmats(const Tmats& t, const std::vector<InjectChannel>& chans) {
    std::string text = t.text();
    if (chans.empty()) return text;
    int n = 0;
    for (const auto& kv : t.attributes())
        if (kv.first.compare(0, 8, "R-1\\TK1-") == 0) n = std::max(n, std::atoi(kv.first.c_str() + 8));
    if (text.find("R-1\\ID:") == std::string::npos) text += "R-1\\ID:USERDATA;\n";
    std::string added;
    int total = n;
    for (const auto& c : chans) {
        if (t.channels().count(c.channel)) continue;
        ++total;
        const char* cdt = c.data_type == kMilStd1553F1 ? "1553IN" : c.data_type == kMessageF0 ? "MSGIN" :
                          c.data_type == kEthernetF0 ? "ETHIN" : "OTHER";
        added += "R-1\\DSI-" + std::to_string(total) + ":" + c.name + ";\n";
        added += "R-1\\TK1-" + std::to_string(total) + ":" + std::to_string(c.channel) + ";\n";
        added += "R-1\\CDT-" + std::to_string(total) + ":" + cdt + ";\n";
    }
    if (added.empty()) return text;
    std::size_t p = text.find("R-1\\N:");
    if (p != std::string::npos) {
        std::size_t e = text.find(';', p);
        if (e != std::string::npos) text.replace(p, e - p, "R-1\\N:" + std::to_string(total));
    } else {
        added = "R-1\\N:" + std::to_string(total) + ";\n" + added;
    }
    return text + added;
}

// ===========================================================================
// Cut
// ===========================================================================
bool Cut(Session& src, const FilterSpec& spec, const std::vector<Annotation>& extra,
         const std::filesystem::path& out, OpResult& res, const ProgressFn& progress) {
    return Cut(src, spec, extra, Injection(), out, res, progress);
}

bool Cut(Session& src, const FilterSpec& spec, const std::vector<Annotation>& extra,
         const Injection& inject, const std::filesystem::path& out, OpResult& res,
         const ProgressFn& progress) {
    res = OpResult();
    if (!src.IsOpen()) return Fail(res, "no source file open");
    Writer w(out);
    if (!w.IsOpen()) return Fail(res, "cannot create " + out.string());

    // A time packet just before the range keeps the cut time-correlated; the
    // setup record is stamped with the earliest time the output contains.
    std::size_t lead_time = std::string::npos;
    uint64_t start_rtc = UINT64_MAX;
    if (spec.rtc_from > src.rtc_min()) {
        lead_time = LastTimePacketBefore(src, spec.rtc_from);
        start_rtc = spec.rtc_from;
        if (lead_time != std::string::npos)
            start_rtc = std::min(start_rtc, src.packets()[lead_time].rtc);
    }
    if (inject.channels.empty()) {
        if (!CopySetupRecords(src, w, start_rtc)) return Fail(res, "write failed (setup record)");
    } else {
        // Rewrite the setup record with the new channels described.
        std::string text = AugmentTmats(src.tmats(), inject.channels);
        std::vector<uint8_t> body(4, 0);
        uint32_t csdw = (src.tmats().xml_format() ? (1u << 9) : 0) | src.tmats().ch10_version();
        for (int i = 0; i < 4; ++i) body[i] = (csdw >> (8 * i)) & 0xFF;
        body.insert(body.end(), text.begin(), text.end());
        uint64_t rtc = start_rtc != UINT64_MAX ? start_rtc : src.rtc_min();
        if (!w.Write(0, kCompGenF1_Tmats, 0x07, 0x02, rtc, nullptr, body.data(), body.size()))
            return Fail(res, "write failed (setup record)");
    }
    if (lead_time != std::string::npos) {
        Packet p;
        if (src.ReadPacket(lead_time, p) && !w.WriteCopy(p)) return Fail(res, "write failed");
    }

    // Everything to insert, in time order: annotations and injected packets.
    std::vector<InjectPacket> pending = inject.packets;
    for (const Annotation& a : extra) {
        InjectPacket ip;
        ip.rtc = a.rtc_start; ip.channel = kAnnotationChannel; ip.data_type = kCompGenF0_UserDefined;
        ip.body = EncodeAnnotationBody(a);
        pending.push_back(std::move(ip));
    }
    std::stable_sort(pending.begin(), pending.end(),
                     [](const InjectPacket& a, const InjectPacket& b) { return a.rtc < b.rtc; });
    std::size_t next_ann = 0;
    auto write_pending = [&](const InjectPacket& ip) {
        return w.Write(ip.channel, ip.data_type, 0x06, ip.flags, ip.rtc, nullptr,
                       ip.body.data(), ip.body.size());
    };

    Packet p;
    std::vector<uint8_t> body;
    const auto& idx = src.packets();
    const uint64_t total = src.file_size();
    uint64_t done = 0, last_report = 0;

    // Walk in time order: recorders may commit packets up to a second late,
    // so file order is not time order, and the output should be sorted.
    for (uint32_t i : src.ByTime()) {
        const PacketIndexEntry& e = idx[i];
        ++res.packets_in;
        done += e.packet_len;
        if (progress && (done - last_report > (1u << 20))) {
            last_report = done;
            if (!progress(done, total)) return Fail(res, "cancelled");
        }
        if (e.data_type == kCompGenF1_Tmats) continue;

        while (next_ann < pending.size() && pending[next_ann].rtc <= e.rtc) {
            if (!write_pending(pending[next_ann++])) return Fail(res, "write failed");
        }
        if (e.rtc < spec.rtc_from || e.rtc > spec.rtc_to) continue;

        const bool is_time = e.data_type == kTimeF1;
        if (!is_time) {
            if (!spec.channels.empty() && !spec.channels.count(e.channel) &&
                e.channel != kAnnotationChannel) continue;
            if (spec.drop_types.count(e.data_type)) continue;
        }
        if (!src.ReadPacket(i, p)) continue;

        if (e.data_type == kMilStd1553F1 && spec.m1553.enabled) {
            if (Filter1553(p.Body(), p.BodyLen(), spec.m1553, body) == 0) continue;
            if (!WriteParts(w, p, p.header.channel_id, p.header.rtc, body.data(), body.size()))
                return Fail(res, "write failed");
        } else if (!w.WriteCopy(p)) {
            return Fail(res, "write failed");
        }
    }
    for (; next_ann < pending.size(); ++next_ann)
        if (!write_pending(pending[next_ann])) return Fail(res, "write failed");

    if (!w.Close()) return Fail(res, "write failed on close");
    res.packets_out = w.packets();
    res.bytes_out   = w.bytes();
    res.outputs.push_back(out);
    return true;
}

// ===========================================================================
// Split
// ===========================================================================
bool Split(Session& src, double seconds, uint64_t max_bytes,
           const std::filesystem::path& prefix, OpResult& res,
           const ProgressFn& progress) {
    res = OpResult();
    if (!src.IsOpen()) return Fail(res, "no source file open");
    if (seconds <= 0 && max_bytes == 0) return Fail(res, "choose a duration or a size");

    std::unique_ptr<Writer> w;
    int part = 0;
    auto start_part = [&](uint64_t at_rtc) -> bool {
        if (w && !w->Close()) return false;
        std::filesystem::path name = prefix;
        char suffix[32];
        std::snprintf(suffix, sizeof suffix, "_part%02d.ch10", ++part);
        name += suffix;
        w = std::make_unique<Writer>(name);
        if (!w->IsOpen()) return false;
        res.outputs.push_back(name);
        std::size_t t = std::string::npos;
        uint64_t start_rtc = UINT64_MAX;
        if (at_rtc > src.rtc_min()) {
            t = LastTimePacketBefore(src, at_rtc);
            start_rtc = at_rtc;
            if (t != std::string::npos) start_rtc = std::min(start_rtc, src.packets()[t].rtc);
        }
        if (!CopySetupRecords(src, *w, start_rtc)) return false;
        if (t != std::string::npos) {
            Packet tp;
            if (src.ReadPacket(t, tp) && tp.header.rtc < at_rtc && !w->WriteCopy(tp)) return false;
        }
        return true;
    };

    const uint64_t span = seconds > 0 ? static_cast<uint64_t>(seconds * 1e7) : 0;
    uint64_t next_boundary = src.rtc_min() + span;
    if (!start_part(src.rtc_min())) return Fail(res, "cannot create output part");

    Packet p;
    const auto& idx = src.packets();
    uint64_t done = 0, last_report = 0;
    for (uint32_t i : src.ByTime()) {
        const PacketIndexEntry& e = idx[i];
        ++res.packets_in;
        done += e.packet_len;
        if (progress && (done - last_report > (1u << 20))) {
            last_report = done;
            if (!progress(done, src.file_size())) return Fail(res, "cancelled");
        }
        if (e.data_type == kCompGenF1_Tmats) continue;

        bool new_part = false;
        if (span) {
            while (e.rtc >= next_boundary) { next_boundary += span; new_part = true; }
        } else if (w->bytes() > 0 && w->bytes() + e.packet_len > max_bytes &&
                   w->packets() > 1) {
            new_part = true;
        }
        if (new_part && !start_part(e.rtc)) return Fail(res, "cannot create output part");
        if (!src.ReadPacket(i, p)) continue;
        if (!w->WriteCopy(p)) return Fail(res, "write failed");
        res.packets_out++;
        res.bytes_out += e.packet_len;
    }
    if (!w->Close()) return Fail(res, "write failed on close");
    return true;
}

// ===========================================================================
// Merge
// ===========================================================================
std::vector<int> SuggestChannelOffsets(const std::vector<Session*>& sessions) {
    std::vector<int> offsets;
    int next_free = 0;
    for (Session* s : sessions) {
        int lo = INT32_MAX, hi = 0;
        for (const auto& c : s->channels()) {
            if (c.channel_id == 0) continue;
            lo = std::min(lo, c.channel_id);
            hi = std::max(hi, c.channel_id);
        }
        if (lo == INT32_MAX) { offsets.push_back(0); continue; }
        int off = 0;
        if (lo <= next_free) off = ((next_free - lo) / 100 + 1) * 100;   // next multiple of 100
        offsets.push_back(off);
        next_free = std::max(next_free, hi + off + 1);
    }
    return offsets;
}

bool Merge(const std::vector<MergeInput>& inputs, const std::filesystem::path& out,
           OpResult& res, const ProgressFn& progress) {
    res = OpResult();
    if (inputs.size() < 2) return Fail(res, "merge needs at least two files");

    std::vector<std::unique_ptr<Session>> ss;
    uint64_t total_bytes = 0;
    for (const auto& in : inputs) {
        auto s = std::make_unique<Session>();
        if (!s->Open(in.path)) return Fail(res, in.path.string() + ": " + s->error());
        total_bytes += s->file_size();
        ss.push_back(std::move(s));
    }

    // --- time alignment -----------------------------------------------------
    bool all_timed = true;
    for (auto& s : ss) all_timed = all_timed && s->time().HasReference();
    std::vector<int64_t> delta(ss.size(), 0);
    if (all_timed) {
        const int64_t base_epoch = ss[0]->time().reference().EpochNs();
        const int64_t base_rtc   = static_cast<int64_t>(ss[0]->time().ref_rtc());
        for (std::size_t i = 1; i < ss.size(); ++i) {
            int64_t epoch_i = ss[i]->time().reference().EpochNs();
            int64_t want_rtc = base_rtc + (epoch_i - base_epoch) / 100;   // ns -> 100 ns ticks
            delta[i] = want_rtc - static_cast<int64_t>(ss[i]->time().ref_rtc());
        }
    } else {
        res.warning += "Not every file has a time packet; files were placed back to back. ";
        int64_t end = static_cast<int64_t>(ss[0]->rtc_max());
        for (std::size_t i = 1; i < ss.size(); ++i) {
            delta[i] = end + 10000000 - static_cast<int64_t>(ss[i]->rtc_min());
            end = static_cast<int64_t>(ss[i]->rtc_max()) + delta[i];
        }
    }

    Writer w(out);
    if (!w.IsOpen()) return Fail(res, "cannot create " + out.string());

    // --- merged setup record ------------------------------------------------
    {
        std::string text = ss[0]->tmats().text();
        int max_r = MaxRecorderIndex(ss[0]->tmats());
        for (std::size_t i = 1; i < ss.size(); ++i) {
            if (ss[i]->tmats().empty()) continue;
            text += "\n";
            text += RenumberedTmats(ss[i]->tmats(), max_r, inputs[i].channel_offset, max_r);
        }
        std::vector<uint8_t> body(4, 0);
        uint32_t csdw = (ss[0]->tmats().xml_format() ? (1u << 9) : 0) | ss[0]->tmats().ch10_version();
        for (int i = 0; i < 4; ++i) body[i] = (csdw >> (8 * i)) & 0xFF;
        body.insert(body.end(), text.begin(), text.end());
        uint64_t first_rtc = UINT64_MAX;
        for (std::size_t i = 0; i < ss.size(); ++i)
            first_rtc = std::min<uint64_t>(first_rtc,
                static_cast<uint64_t>(static_cast<int64_t>(ss[i]->rtc_min()) + delta[i]));
        if (!w.Write(0, kCompGenF1_Tmats, 0x07, kAnnotationFlags, first_rtc, nullptr,
                     body.data(), body.size()))
            return Fail(res, "write failed (setup record)");
    }

    // --- k-way merge by output time -----------------------------------------
    struct Head { uint64_t rtc; std::size_t file; std::size_t pos; };
    auto cmp = [](const Head& a, const Head& b) { return a.rtc > b.rtc; };
    std::priority_queue<Head, std::vector<Head>, decltype(cmp)> heap(cmp);
    auto push = [&](std::size_t f, std::size_t pos) {
        const auto& order = ss[f]->ByTime();
        while (pos < order.size()) {
            const auto& e = ss[f]->packets()[order[pos]];
            if (e.data_type != kCompGenF1_Tmats) {
                heap.push({static_cast<uint64_t>(static_cast<int64_t>(e.rtc) + delta[f]), f, pos});
                return;
            }
            ++pos;
        }
    };
    for (std::size_t f = 0; f < ss.size(); ++f) push(f, 0);

    bool ipts_warning = false;
    uint64_t done = 0, last_report = 0;
    Packet p;
    std::vector<uint8_t> body;
    while (!heap.empty()) {
        Head h = heap.top(); heap.pop();
        Session& s = *ss[h.file];
        uint32_t pidx = s.ByTime()[h.pos];
        const PacketIndexEntry& e = s.packets()[pidx];
        push(h.file, h.pos + 1);

        ++res.packets_in;
        done += e.packet_len;
        if (progress && done - last_report > (1u << 20)) {
            last_report = done;
            if (!progress(done, total_bytes)) return Fail(res, "cancelled");
        }
        if (!s.ReadPacket(pidx, p)) continue;

        int channel = e.channel == 0 ? 0 : e.channel + inputs[h.file].channel_offset;
        if (channel > 0xFFFF) return Fail(res, "channel offset pushes a channel ID past 65535");
        uint64_t rtc = h.rtc;

        if (delta[h.file] != 0) {
            body.assign(p.Body(), p.Body() + p.BodyLen());
            if (!RebaseIpts(p.header, body, delta[h.file])) ipts_warning = true;
            if (!WriteParts(w, p, static_cast<uint16_t>(channel), rtc, body.data(), body.size()))
                return Fail(res, "write failed");
        } else if (!w.WriteCopy(p, channel, rtc, true)) {
            return Fail(res, "write failed");
        }
    }
    if (ipts_warning)
        res.warning += "Some packet types (e.g. PCM, ARINC-429, analog) carry intra-packet "
                       "time stamps this tool does not rebase; their header times are "
                       "correct but IPTS values keep the original file's clock. ";
    if (!w.Close()) return Fail(res, "write failed on close");
    res.packets_out = w.packets();
    res.bytes_out   = w.bytes();
    res.outputs.push_back(out);
    return true;
}

} // namespace ch10
