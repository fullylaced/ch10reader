// src/annotation.cpp
#include "ch10/annotation.h"

#include <cstring>

#include "ch10/packet.h"

namespace ch10 {

const char* SeverityName(Severity s) {
    switch (s) {
        case Severity::Note:    return "Note";
        case Severity::Warning: return "Warning";
        case Severity::Anomaly: return "Anomaly";
        case Severity::Marker:  return "Marker";
    }
    return "Note";
}

namespace {
void Put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF); }
void Put64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back((x >> (8 * i)) & 0xFF); }
}

std::vector<uint8_t> EncodeAnnotationBody(const Annotation& a) {
    std::vector<uint8_t> v;
    Put32(v, 0);                                   // Format 0 CSDW: reserved
    v.insert(v.end(), kAnnotationMagic, kAnnotationMagic + 8);
    Put16(v, 1);                                   // version
    v.push_back(static_cast<uint8_t>(a.severity));
    v.push_back(a.IsRange() ? 1 : 0);
    Put64(v, a.rtc_start);
    Put64(v, a.IsRange() ? a.rtc_end : a.rtc_start);
    Put32(v, static_cast<uint32_t>(a.channel));
    std::size_t n = a.text.size() > 60000 ? 60000 : a.text.size();
    Put16(v, static_cast<uint16_t>(n));
    v.insert(v.end(), a.text.begin(), a.text.begin() + static_cast<std::ptrdiff_t>(n));
    return v;
}

bool IsAnnotationBody(const uint8_t* body, std::size_t len) {
    return len >= 4 + 34 && std::memcmp(body + 4, kAnnotationMagic, 8) == 0;
}

bool DecodeAnnotationBody(const uint8_t* body, std::size_t len, Annotation& out) {
    if (!IsAnnotationBody(body, len)) return false;
    const uint8_t* p = body + 4 + 8;
    uint16_t version = le16(p);
    if (version != 1) return false;
    out.severity  = static_cast<Severity>(p[2] & 0x3);
    out.rtc_start = le64(p + 4);
    out.rtc_end   = (p[3] & 1) ? le64(p + 12) : out.rtc_start;
    out.channel   = static_cast<int32_t>(le32(p + 20));
    uint16_t n    = le16(p + 24);
    if (static_cast<std::size_t>(4 + 34) + n > len) return false;
    out.text.assign(reinterpret_cast<const char*>(p + 26), n);
    out.source = AnnotationSource::File;
    return true;
}

} // namespace ch10
