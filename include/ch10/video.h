// ch10/video.h
// Video Data Packets, Format 0 (10.6.10.1) and Format 1 (10.6.10.2).
//
// Format 0: MPEG-2/H.264 in 188-byte transport-stream packets. CSDW per
//           Figure 10-51 (ET/IPH/SRS/KLV/PL/BA).
// Format 1: complete ISO 13818-1 bit stream, transport OR program stream.
//           CSDW per Figure 10-55 (KLV/SRS/IPH/EPL/ET/MD/TP/PC).
//
// In both formats an optional 8-byte intra-packet time stamp may precede each
// MPEG packet (CSDW IPH bit). Stripping those and concatenating the MPEG
// bytes for a channel yields a .ts (or .mpg) file any media player can open.

#pragma once

#include <cstdint>
#include <vector>

#include "ch10/packet.h"

namespace ch10 {
namespace video {

struct Info {
    uint8_t format        = 0;      // 0 or 1
    bool    transport     = true;   // false = program stream (F1 TP bit only)
    bool    iph           = false;  // IPTS before each MPEG packet
    bool    embedded_time = false;
    bool    scr_rtc_sync  = false;
    bool    klv           = false;
    bool    byte_aligned  = false;  // F0 BA bit: 16-bit words, swap pairs
    uint8_t profile_level = 0;      // F0 PL (bits 27-24) / F1 EPL (bits 18-15)
    bool    variable_rate = false;  // F1 MD bit
    uint16_t packet_count = 0;      // F1 PC (may be 0 = unknown)

    const char* FormatName() const;
};

// Decode the CSDW for a Video F0 (0x40) or F1 (0x41) packet.
Info DecodeCsdw(uint8_t data_type, uint32_t csdw);

// Append the MPEG bytes contained in one video packet body to `out`,
// skipping the CSDW and any intra-packet time stamps. Returns the number of
// MPEG packets appended (188-byte TS packets; for a program stream without
// IPH the whole payload is appended and 1 is returned).
//
// Limitation: a Format 1 *program* stream recorded with IPH set cannot be
// split without parsing MPEG pack headers, so its payload is copied raw and
// `raw_copy` is set to true.
std::size_t Extract(uint8_t data_type, const uint8_t* body, std::size_t len,
                    std::vector<uint8_t>& out, bool* raw_copy = nullptr);

// Suggested file extension for a channel of this kind (".ts" or ".mpg").
const char* Extension(const Info& info);

} // namespace video
} // namespace ch10
