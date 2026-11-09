// ch10/annotation.h
// Annotations and anomaly markers on the recording timeline.
//
// User annotations are stored inside a Chapter 10 file as Computer-Generated
// Data, Format 0 (user-defined, 10.6.7.1) packets on a dedicated channel.
// The body is the reserved CSDW followed by:
//
//   offset  size  field
//   0       8     magic "CH10ANN1"
//   8       2     format version (1)                little-endian
//   10      1     severity (Severity enum)
//   11      1     flags (bit 0: has range)
//   12      8     rtc_start (10 MHz RTC, same clock as packet headers)
//   20      8     rtc_end   (== rtc_start for a point annotation)
//   28      4     channel (int32, -1 = whole recording)
//   32      2     text length in bytes (UTF-8)
//   34      n     text
//
// The packet header RTC equals rtc_start, so any Chapter 10 reader lists the
// packet at the right time even if it does not understand the payload.
// Recording Event packets (Format 2) found in a file are surfaced as
// annotations too, and anomalies detected while scanning (sequence gaps,
// RTC/overflow flags, 1553 block-status errors, resyncs) share the struct.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ch10 {

enum class Severity : uint8_t { Note = 0, Warning = 1, Anomaly = 2, Marker = 3 };
const char* SeverityName(Severity s);

enum class AnnotationSource : uint8_t {
    User,       // added in this session, not yet saved to a file
    File,       // read from a CH10ANN1 packet
    Event,      // Recording Event (Format 2) packet
    Auto,       // detected anomaly (never written to a file unless converted)
};

struct Annotation {
    uint64_t         rtc_start = 0;
    uint64_t         rtc_end   = 0;      // == rtc_start for a point
    int              channel   = -1;     // -1 = not tied to a channel
    Severity         severity  = Severity::Note;
    AnnotationSource source    = AnnotationSource::User;
    std::string      text;
    uint64_t         packet_index = 0;   // for File/Event/Auto: where it came from

    bool IsRange() const { return rtc_end > rtc_start; }
};

constexpr uint16_t kAnnotationChannel = 0xA550;   // default channel for our packets
constexpr char     kAnnotationMagic[9] = "CH10ANN1";

// Build the packet body (CSDW + payload) for a Format 0 annotation packet.
std::vector<uint8_t> EncodeAnnotationBody(const Annotation& a);
// Recognise / decode a Format 0 body. Returns false if not an annotation.
bool IsAnnotationBody(const uint8_t* body, std::size_t len);
bool DecodeAnnotationBody(const uint8_t* body, std::size_t len, Annotation& out);

} // namespace ch10
