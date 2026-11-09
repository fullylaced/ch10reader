// src/writer.cpp
#include "ch10/writer.h"

#include <cstring>

namespace ch10 {

namespace {
void Store16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void Store32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xFF; }
void Store48(uint8_t* p, uint64_t v) { for (int i = 0; i < 6; ++i) p[i] = (v >> (8 * i)) & 0xFF; }
}

Writer::Writer(const std::filesystem::path& path) : out_(path, std::ios::binary) {}

bool Writer::Close() {
    if (!out_.is_open()) return true;
    out_.flush();
    bool ok = out_.good();
    out_.close();
    return ok;
}

uint8_t Writer::NextSeq(uint16_t channel) {
    auto it = seq_.find(channel);
    if (it == seq_.end()) { seq_[channel] = 0; return 0; }
    return ++it->second;
}

bool Writer::Emit(const uint8_t* data, std::size_t n) {
    out_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    bytes_ += n;
    return out_.good();
}

bool Writer::WriteCopy(const Packet& p, int channel_override,
                       uint64_t rtc_override, bool has_rtc_override) {
    scratch_.assign(p.raw.begin(), p.raw.end());
    uint8_t* h = scratch_.data();
    uint16_t channel = channel_override >= 0 ? static_cast<uint16_t>(channel_override)
                                             : p.header.channel_id;
    Store16(h + 2, channel);
    h[13] = NextSeq(channel);
    if (has_rtc_override) Store48(h + 16, rtc_override);
    Store16(h + 22, HeaderChecksum(h));
    ++packets_;
    return Emit(scratch_.data(), scratch_.size());
}

bool Writer::Write(uint16_t channel, uint8_t data_type, uint8_t version, uint8_t flags,
                   uint64_t rtc, const uint8_t* secondary12,
                   const uint8_t* body, std::size_t body_len) {
    const bool sec = (flags & 0x80) != 0;
    const uint8_t cktype = flags & 0x3;
    const std::size_t cklen = cktype == 0 ? 0 : (cktype == 1 ? 1 : (cktype == 2 ? 2 : 4));

    // Trailer: filler so that body + checksum ends on a 4-byte boundary.
    std::size_t filler = 0;
    while ((body_len + filler + cklen) % 4) ++filler;
    std::size_t packet_len = kHeaderSize + (sec ? kSecondaryHeaderSize : 0) +
                             body_len + filler + cklen;

    scratch_.assign(packet_len, 0);
    uint8_t* h = scratch_.data();
    Store16(h + 0, kSyncPattern);
    Store16(h + 2, channel);
    Store32(h + 4, static_cast<uint32_t>(packet_len));
    Store32(h + 8, static_cast<uint32_t>(body_len));
    h[12] = version;
    h[13] = NextSeq(channel);
    h[14] = flags;
    h[15] = data_type;
    Store48(h + 16, rtc);
    Store16(h + 22, HeaderChecksum(h));

    uint8_t* q = h + kHeaderSize;
    if (sec) {
        if (secondary12) std::memcpy(q, secondary12, kSecondaryHeaderSize);
        // Secondary header checksum: 16-bit sum of its first 5 words.
        uint32_t s = 0;
        for (int i = 0; i < 10; i += 2) s += le16(q + i);
        Store16(q + 10, static_cast<uint16_t>(s & 0xFFFF));
        q += kSecondaryHeaderSize;
    }
    std::memcpy(q, body, body_len);
    // filler already zero
    if (cklen) {
        uint32_t ck = DataChecksum(q, body_len + filler, cktype);
        uint8_t* c = q + body_len + filler;
        if (cklen == 1) c[0] = static_cast<uint8_t>(ck);
        else if (cklen == 2) Store16(c, static_cast<uint16_t>(ck));
        else Store32(c, ck);
    }
    ++packets_;
    return Emit(scratch_.data(), scratch_.size());
}

} // namespace ch10
