// src/video.cpp
#include "ch10/video.h"

namespace ch10 {
namespace video {

const char* Info::FormatName() const {
    if (format == 0) return "MPEG-2/H.264 TS";
    return transport ? "MPEG-2 TS (ISO 13818-1)" : "MPEG-2 PS (ISO 13818-1)";
}

Info DecodeCsdw(uint8_t data_type, uint32_t w) {
    Info i;
    if (data_type == kVideoF1) {                 // Figure 10-55
        i.format        = 1;
        i.klv           = (w >> 21) & 1;
        i.scr_rtc_sync  = (w >> 20) & 1;
        i.iph           = (w >> 19) & 1;
        i.profile_level = (w >> 15) & 0xF;
        i.embedded_time = (w >> 14) & 1;
        i.variable_rate = (w >> 13) & 1;
        i.transport     = ((w >> 12) & 1) == 0;  // TP: 0 = transport, 1 = program
        i.packet_count  = w & 0xFFF;
    } else {                                     // Figure 10-51 (Format 0)
        i.format        = 0;
        i.embedded_time = (w >> 31) & 1;
        i.iph           = (w >> 30) & 1;
        i.scr_rtc_sync  = (w >> 29) & 1;
        i.klv           = (w >> 28) & 1;
        i.profile_level = (w >> 24) & 0xF;
        i.byte_aligned  = (w >> 23) & 1;
        i.transport     = true;
    }
    return i;
}

const char* Extension(const Info& info) {
    return info.transport ? ".ts" : ".mpg";
}

std::size_t Extract(uint8_t data_type, const uint8_t* body, std::size_t len,
                    std::vector<uint8_t>& out, bool* raw_copy) {
    if (raw_copy) *raw_copy = false;
    if (len < 4) return 0;
    Info info = DecodeCsdw(data_type, le32(body));
    std::size_t pos = 4;

    if (!info.transport) {
        // Program stream: variable-length packs. Copy the payload as-is.
        if (raw_copy) *raw_copy = info.iph;
        out.insert(out.end(), body + pos, body + len);
        return 1;
    }

    constexpr std::size_t kTs = 188;
    const std::size_t stride = (info.iph ? 8 : 0) + kTs;
    std::size_t count = 0;

    while (pos + stride <= len) {
        if (info.iph) pos += 8;                  // skip the IPTS
        if (!info.byte_aligned) {
            out.insert(out.end(), body + pos, body + pos + kTs);
        } else {
            // 16-bit aligned words: restore byte order of the stream.
            for (std::size_t i = 0; i < kTs; i += 2) {
                out.push_back(body[pos + i + 1]);
                out.push_back(body[pos + i]);
            }
        }
        pos += kTs;
        ++count;
    }
    return count;
}

} // namespace video
} // namespace ch10
