// src/message_f0.cpp
#include "ch10/message_f0.h"

#include "ch10/packet.h"

namespace ch10 {

std::string Message::Text() const {
    std::string s;
    s.reserve(data.size());
    for (uint8_t b : data) s += (b >= 32 && b < 127) ? static_cast<char>(b) : (b == '\n' || b == '\r' || b == '\t') ? ' ' : '.';
    return s;
}

bool Message::LooksLikeText() const {
    if (data.empty()) return false;
    std::size_t printable = 0;
    for (uint8_t b : data) if ((b >= 32 && b < 127) || b == '\n' || b == '\r' || b == '\t') ++printable;
    return printable * 10 >= data.size() * 9;
}

bool DecodeMessageF0(const uint8_t* body, std::size_t len, uint8_t flags, uint64_t packet_rtc,
                     MessageCsdw& csdw, std::vector<Message>& out) {
    out.clear();
    if (len < 4) return false;
    uint32_t w = le32(body);                          // Figure 10-47
    csdw.type = (w >> 16) & 0x3;
    csdw.counter = w & 0xFFFF;
    const bool sec = (flags & 0x40) != 0;
    std::size_t pos = 4;
    // Complete-messages packets: counter = number of messages. Segmented
    // packets (type != 0) hold one segment; expose it as one message.
    uint32_t n = csdw.type == 0 ? csdw.counter : 1;
    for (uint32_t i = 0; i < n && pos + 12 <= len; ++i) {
        Message m;
        m.rtc = sec ? packet_rtc : (le64(body + pos) & 0x0000FFFFFFFFFFFFull);
        uint32_t id = le32(body + pos + 8);           // Figure 10-50
        m.data_error = (id >> 31) & 1;
        m.format_error = (id >> 30) & 1;
        m.subchannel = (id >> 16) & 0x3FFF;
        uint16_t mlen = id & 0xFFFF;
        pos += 12;
        if (pos + mlen > len) break;
        m.data.assign(body + pos, body + pos + mlen);
        out.push_back(std::move(m));
        pos += mlen + (mlen & 1);
    }
    return true;
}

std::vector<uint8_t> EncodeMessageF0(const std::vector<Message>& msgs) {
    std::vector<uint8_t> v;
    auto put32 = [&v](uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF); };
    auto put64 = [&v](uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back((x >> (8 * i)) & 0xFF); };
    put32(static_cast<uint32_t>(msgs.size() & 0xFFFF));           // type 00 complete
    for (const Message& m : msgs) {
        put64(m.rtc & 0x0000FFFFFFFFFFFFull);
        uint32_t id = (m.data_error ? 1u << 31 : 0) | (m.format_error ? 1u << 30 : 0) |
                      (static_cast<uint32_t>(m.subchannel & 0x3FFF) << 16) |
                      static_cast<uint32_t>(m.data.size() & 0xFFFF);
        put32(id);
        v.insert(v.end(), m.data.begin(), m.data.end());
        if (m.data.size() & 1) v.push_back(0);
    }
    return v;
}

} // namespace ch10
