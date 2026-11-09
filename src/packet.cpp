// src/packet.cpp
#include "ch10/packet.h"

namespace ch10 {

const char* DataTypeName(uint8_t type) {
    switch (type) {
        case 0x00: return "Computer-Generated F0 (User-Defined)";
        case 0x01: return "Computer-Generated F1 (TMATS Setup Record)";
        case 0x02: return "Computer-Generated F2 (Recording Events)";
        case 0x03: return "Computer-Generated F3 (Recording Index)";
        case 0x09: return "PCM F1 (Chapter 4/8)";
        case 0x11: return "Time F1 (IRIG/GPS/RTC)";
        case 0x19: return "MIL-STD-1553 F1";
        case 0x1A: return "MIL-STD-1553 F2 (16PP194)";
        case 0x21: return "Analog F1";
        case 0x29: return "Discrete F1";
        case 0x30: return "Message F0";
        case 0x38: return "ARINC-429 F0";
        case 0x40: return "Video F0 (MPEG-2/H.264 TS)";
        case 0x41: return "Video F1 (ISO 13818-1 MPEG-2)";
        case 0x42: return "Video F2 (MPEG-4 AVC/H.264)";
        case 0x43: return "Video F3 (MJPEG)";
        case 0x44: return "Video F4 (MJPEG 2000)";
        case 0x48: return "Image F0";
        case 0x50: return "UART F0";
        case 0x68: return "Ethernet F0";
        case 0x69: return "Ethernet F1 (UDP Payload)";
        case 0x78: return "CAN Bus";
        default:   return "Unknown/Reserved";
    }
}

bool ParseHeader(const uint8_t* b, PacketHeader& h) {
    h.sync          = le16(b + 0);
    h.channel_id    = le16(b + 2);
    h.packet_len    = le32(b + 4);
    h.data_len      = le32(b + 8);
    h.data_type_ver = b[12];
    h.seq_num       = b[13];
    h.flags         = b[14];
    h.data_type     = b[15];
    h.rtc           = le48(b + 16);
    h.checksum      = le16(b + 22);
    return h.sync == kSyncPattern;
}

uint16_t HeaderChecksum(const uint8_t* b) {
    uint32_t sum = 0;
    for (int i = 0; i < 22; i += 2) sum += le16(b + i);
    return static_cast<uint16_t>(sum & 0xFFFF);
}

bool HeaderLooksValid(const uint8_t* b) {
    if (le16(b) != kSyncPattern) return false;
    PacketHeader h;
    ParseHeader(b, h);
    if (h.checksum != HeaderChecksum(b)) return false;
    // Sanity on lengths: multiple of 4, big enough for its own headers,
    // and data must fit inside the packet.
    if (h.packet_len & 0x3) return false;
    std::size_t min_len = kHeaderSize +
        (h.HasSecondaryHeader() ? kSecondaryHeaderSize : 0);
    if (h.packet_len < min_len || h.packet_len > kMaxPacketSize) return false;
    if (h.data_len > h.packet_len - min_len) return false;
    return true;
}

uint32_t DataChecksum(const uint8_t* body, std::size_t len, uint8_t type) {
    // 10.6.1.4: arithmetic sum of bytes/words/long words over the packet body
    // plus filler, excluding headers and the checksum itself.
    uint32_t sum = 0;
    switch (type) {
        case 1:  // 8-bit
            for (std::size_t i = 0; i < len; ++i) sum += body[i];
            return sum & 0xFF;
        case 2:  // 16-bit
            for (std::size_t i = 0; i + 1 < len; i += 2) sum += le16(body + i);
            return sum & 0xFFFF;
        case 3:  // 32-bit
            for (std::size_t i = 0; i + 3 < len; i += 4) sum += le32(body + i);
            return sum;
        default:
            return 0;
    }
}

} // namespace ch10
