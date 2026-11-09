// src/ethernet.cpp
#include "ch10/ethernet.h"

#include <cstdio>
#include <cstring>
#include <fstream>

#include "ch10/reader.h"

namespace ch10 {
namespace eth {

namespace {
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
}

std::string Frame::ErrorFlags() const {
    std::string s;
    auto add = [&s](const char* f) { if (!s.empty()) s += '|'; s += f; };
    if (fce) add("FCE");
    if (fe)  add("FE");
    if (dce) add("DCE");
    if (le)  add("LE");
    if (arinc664 && error) add("ERR");
    return s;
}

bool DecodeF0(const uint8_t* body, std::size_t len, uint8_t flags, uint64_t packet_rtc,
              Csdw& csdw, std::vector<Frame>& out) {
    out.clear();
    if (len < 4) return false;
    uint32_t w = le32(body);                         // Figure 10-85
    csdw.format = (w >> 28) & 0xF;
    csdw.ttb    = (w >> 25) & 0x7;
    csdw.frames = w & 0xFFFF;
    const bool sec = (flags & 0x40) != 0;
    std::size_t pos = 4;
    for (uint16_t i = 0; i < csdw.frames && pos + 12 <= len; ++i) {
        Frame f;
        f.rtc = sec ? packet_rtc : (le64(body + pos) & 0x0000FFFFFFFFFFFFull);
        uint32_t id = le32(body + pos + 8);              // Figure 10-87
        f.fce = (id >> 31) & 1; f.fe = (id >> 30) & 1;
        f.payload_only = ((id >> 28) & 0x3) == 1;
        f.speed = (id >> 24) & 0xF; f.netid = (id >> 16) & 0xFF;
        f.dce = (id >> 15) & 1; f.le = (id >> 14) & 1;
        f.length = id & 0x3FFF;
        pos += 12;
        if (pos + f.length > len) break;
        f.data = body + pos;
        out.push_back(f);
        pos += f.length + (f.length & 1);            // 16-bit filler
    }
    return true;
}

bool DecodeF1(const uint8_t* body, std::size_t len, uint8_t flags, uint64_t packet_rtc,
              Csdw& csdw, std::vector<Frame>& out) {
    out.clear();
    if (len < 4) return false;
    uint32_t w = le32(body);                         // Figure 10-88
    csdw.iph_len = w >> 16;
    csdw.frames  = w & 0xFFFF;
    csdw.format  = 1;
    const bool sec = (flags & 0x40) != 0;
    std::size_t iph = csdw.iph_len ? csdw.iph_len : 24;
    std::size_t pos = 4;
    for (uint16_t i = 0; i < csdw.frames && pos + iph <= len; ++i) {
        Frame f;
        f.arinc664 = true;
        f.rtc = sec ? packet_rtc : (le64(body + pos) & 0x0000FFFFFFFFFFFFull);
        const uint8_t* h = body + pos + 8;               // Figure 10-90
        uint32_t w1 = le32(h), w2 = le32(h + 4);
        f.length = w1 >> 16; f.error = (w1 >> 8) & 0xFF; f.flags = w1 & 0xFF;
        f.virtual_link = w2 & 0xFFFF;
        f.src_ip = le32(h + 8); f.dst_ip = le32(h + 12);
        uint32_t w5 = le32(h + 16);
        f.src_port = w5 >> 16; f.dst_port = w5 & 0xFFFF;
        pos += iph;
        if (pos + f.length > len) break;
        f.data = body + pos;
        out.push_back(f);
        pos += f.length + (f.length & 1);
    }
    return true;
}

bool Decode(uint8_t type, const uint8_t* body, std::size_t len, uint8_t flags,
            uint64_t rtc, Csdw& csdw, std::vector<Frame>& out) {
    if (type == kEthernetF0) return DecodeF0(body, len, flags, rtc, csdw, out);
    if (type == 0x69)        return DecodeF1(body, len, flags, rtc, csdw, out);
    return false;
}

std::string MacString(const uint8_t* m) {
    char b[24];
    std::snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

std::string IpString(uint32_t ip) {
    char b[20];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

Parsed Parse(const uint8_t* d, std::size_t len, bool payload_only) {
    Parsed p;
    std::size_t pos = 0;
    if (!payload_only) {
        if (len < 14) return p;
        std::memcpy(p.dst_mac, d, 6); std::memcpy(p.src_mac, d + 6, 6);
        p.ethertype = be16(d + 12);
        pos = 14;
        if (p.ethertype == 0x8100 && len >= 18) {         // 802.1Q
            p.vlan = true; p.vlan_id = be16(d + 14) & 0x0FFF;
            p.ethertype = be16(d + 16);
            pos = 18;
        }
    } else {
        if (len < 2) return p;
        p.ethertype = be16(d);                            // CONTENT=01 starts at EtherType
        pos = 2;
    }
    p.ok = true;
    p.payload = d + pos; p.payload_len = len - pos;
    if (p.ethertype == 0x0806) { p.arp = true; return p; }
    if (p.ethertype == 0x86DD) { p.ipv6 = true; return p; }
    if (p.ethertype != 0x0800 || len < pos + 20) return p;

    const uint8_t* ip = d + pos;
    if ((ip[0] >> 4) != 4) return p;
    std::size_t ihl = (ip[0] & 0xF) * 4u;
    p.ipv4 = true;
    p.ip_proto = ip[9];
    p.src_ip = be32(ip + 12); p.dst_ip = be32(ip + 16);
    std::size_t total = be16(ip + 2);
    std::size_t ip_end = std::min(len, pos + std::max<std::size_t>(total, ihl));
    pos += ihl;
    if (pos > ip_end) return p;
    p.payload = d + pos; p.payload_len = ip_end - pos;
    if (p.ip_proto == 17 && p.payload_len >= 8) {             // UDP
        p.has_ports = true;
        p.src_port = be16(p.payload); p.dst_port = be16(p.payload + 2);
        std::size_t ulen = be16(p.payload + 4);
        std::size_t dl = ulen >= 8 ? std::min(ulen - 8, p.payload_len - 8) : p.payload_len - 8;
        p.payload += 8; p.payload_len = dl;
    } else if (p.ip_proto == 6 && p.payload_len >= 20) {      // TCP
        p.has_ports = true;
        p.src_port = be16(p.payload); p.dst_port = be16(p.payload + 2);
        std::size_t doff = (p.payload[12] >> 4) * 4u;
        if (doff <= p.payload_len) { p.payload += doff; p.payload_len -= doff; }
    }
    return p;
}

Parsed ParseArinc664(const Frame& f) {
    Parsed p;
    p.ok = true; p.ipv4 = true; p.has_ports = true; p.ip_proto = 17; p.ethertype = 0x0800;
    p.src_ip = f.src_ip; p.dst_ip = f.dst_ip; p.src_port = f.src_port; p.dst_port = f.dst_port;
    p.payload = f.data; p.payload_len = f.length;
    // ARINC-664 destination MAC: 03:00:00:00 + VL
    p.dst_mac[0] = 0x03; p.dst_mac[4] = f.virtual_link >> 8; p.dst_mac[5] = f.virtual_link & 0xFF;
    p.src_mac[0] = 0x02;
    return p;
}

std::string Parsed::Protocol() const {
    if (!ok) return "?";
    if (arp) return "ARP";
    if (ipv6) return "IPv6";
    if (ipv4) return ip_proto == 17 ? "UDP" : ip_proto == 6 ? "TCP" : ip_proto == 1 ? "ICMP" :
                     "IP/" + std::to_string(ip_proto);
    char b[12]; std::snprintf(b, sizeof b, "0x%04X", ethertype); return b;
}

std::string Parsed::Source() const {
    if (ipv4) return has_ports ? IpString(src_ip) + ":" + std::to_string(src_port) : IpString(src_ip);
    return MacString(src_mac);
}

std::string Parsed::Destination() const {
    if (ipv4) return has_ports ? IpString(dst_ip) + ":" + std::to_string(dst_port) : IpString(dst_ip);
    return MacString(dst_mac);
}

std::string Parsed::Info() const {
    std::string s = Protocol() + " " + Source() + " -> " + Destination();
    if (vlan) s += " VLAN " + std::to_string(vlan_id);
    s += "  " + std::to_string(payload_len) + " B";
    return s;
}

// ---------------------------------------------------------------------------
bool WritePcap(Session& s, int channel, const std::filesystem::path& out,
               uint64_t& written, const ProgressFn& progress) {
    written = 0;
    std::ofstream f(out, std::ios::binary);
    if (!f) return false;
    auto put32 = [&f](uint32_t v) { char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char(v >> 24)}; f.write(b, 4); };
    auto put16 = [&f](uint16_t v) { char b[2] = {char(v & 0xFF), char(v >> 8)}; f.write(b, 2); };
    // Global header: magic (microsecond resolution), v2.4, LINKTYPE_ETHERNET
    put32(0xA1B2C3D4); put16(2); put16(4); put32(0); put32(0); put32(65535); put32(1);

    // Absolute time base: ns since epoch at rtc_min (day-of-year: days from 1970).
    int64_t base_ns = 0;
    if (s.time().HasReference())
        base_ns = s.time().reference().EpochNs() + s.time().RtcDeltaNs(s.rtc_min());

    Reader rd(s.path());
    if (!rd.IsOpen()) return false;
    Packet p;
    std::vector<Frame> frames;
    Csdw csdw;
    uint64_t done = 0, last = 0;
    for (uint32_t i : s.ByTime()) {
        const auto& e = s.packets()[i];
        done += e.packet_len;
        if (progress && done - last > (1u << 20)) { last = done; if (!progress(done, s.file_size())) return false; }
        if (e.data_type != kEthernetF0 && e.data_type != 0x69) continue;
        if (channel >= 0 && e.channel != channel) continue;
        if (!rd.ReadAt(static_cast<std::streamoff>(e.offset), p)) continue;
        if (!Decode(e.data_type, p.Body(), p.BodyLen(), p.header.flags, p.header.rtc, csdw, frames)) continue;
        for (const Frame& fr : frames) {
            int64_t ns = base_ns + (static_cast<int64_t>(fr.rtc) - static_cast<int64_t>(s.rtc_min())) * 100;
            if (ns < 0) ns = 0;
            std::vector<uint8_t> bytes;
            if (fr.arinc664) {
                // Synthesise Ethernet + IPv4 + UDP headers around the payload.
                Parsed pa = ParseArinc664(fr);
                std::size_t udp_len = 8 + fr.length, ip_len = 20 + udp_len;
                bytes.reserve(14 + ip_len);
                bytes.insert(bytes.end(), pa.dst_mac, pa.dst_mac + 6);
                bytes.insert(bytes.end(), pa.src_mac, pa.src_mac + 6);
                bytes.push_back(0x08); bytes.push_back(0x00);
                uint8_t ip[20] = {0x45, 0, uint8_t(ip_len >> 8), uint8_t(ip_len), 0, 0, 0, 0, 64, 17, 0, 0,
                                  uint8_t(fr.src_ip >> 24), uint8_t(fr.src_ip >> 16), uint8_t(fr.src_ip >> 8), uint8_t(fr.src_ip),
                                  uint8_t(fr.dst_ip >> 24), uint8_t(fr.dst_ip >> 16), uint8_t(fr.dst_ip >> 8), uint8_t(fr.dst_ip)};
                uint32_t sum = 0; for (int k = 0; k < 20; k += 2) sum += (ip[k] << 8) | ip[k + 1];
                while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
                uint16_t ck = static_cast<uint16_t>(~sum); ip[10] = ck >> 8; ip[11] = ck & 0xFF;
                bytes.insert(bytes.end(), ip, ip + 20);
                uint8_t udp[8] = {uint8_t(fr.src_port >> 8), uint8_t(fr.src_port), uint8_t(fr.dst_port >> 8), uint8_t(fr.dst_port),
                                  uint8_t(udp_len >> 8), uint8_t(udp_len), 0, 0};
                bytes.insert(bytes.end(), udp, udp + 8);
                bytes.insert(bytes.end(), fr.data, fr.data + fr.length);
            } else if (fr.payload_only) {
                uint8_t hdr[12] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0, 0, 0, 0, fr.netid};
                bytes.insert(bytes.end(), hdr, hdr + 12);
                bytes.insert(bytes.end(), fr.data, fr.data + fr.length);   // starts with EtherType
            } else {
                bytes.assign(fr.data, fr.data + fr.length);
            }
            put32(static_cast<uint32_t>(ns / 1000000000)); put32(static_cast<uint32_t>((ns / 1000) % 1000000));
            put32(static_cast<uint32_t>(bytes.size())); put32(static_cast<uint32_t>(bytes.size()));
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            ++written;
        }
    }
    return f.good();
}

} // namespace eth
} // namespace ch10
