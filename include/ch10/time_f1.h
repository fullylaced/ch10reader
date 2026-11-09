// ch10/time_f1.h
// Time Data Packet, Format 1 (10.6.3.2) and time correlation.
//
// Each Time F1 packet pairs an absolute time (BCD words in the body, Figures
// 10-20/10-21) with the 10 MHz RTC in its packet header. TimeCorrelator keeps
// that pair and converts any later 48-bit RTC value — e.g. a 1553 message's
// intra-packet time stamp — to absolute time.

#pragma once

#include <cstdint>
#include <string>

#include "ch10/packet.h"

namespace ch10 {

struct TimeF1 {
    // CSDW (Figure 10-19)
    uint8_t src        = 0;      // 0 internal, 1 external, 2 RMM, 0xF none
    uint8_t fmt        = 0;      // 0 IRIG-B, 1 IRIG-A, 2 IRIG-G, 3 RTC, 4 UTC/GPS, 5 GPS
    bool    dmy        = false;  // false: IRIG day-of-year; true: day/month/year
    bool    leap_year  = false;

    // Decoded time-of-day
    int millis = 0, seconds = 0, minutes = 0, hours = 0;
    int day = 0;                 // day-of-year (day format) or day-of-month (DMY)
    int month = 0, year = 0;     // DMY format only

    const char* FmtName() const;
    const char* SrcName() const;
    std::string ToString() const;   // "DDD:HH:MM:SS.mmm" or "YYYY-MM-DD HH:MM:SS.mmm"
    // Time of day in nanoseconds (date portion excluded).
    uint64_t TimeOfDayNs() const;
    // Nanoseconds since an arbitrary epoch including the date: day-of-year
    // files count from day 0 of an unknown year, DMY files from 1970-01-01.
    // Comparable between files that use the same date format.
    int64_t EpochNs() const;
};

// Decode a Time F1 packet body (CSDW + BCD words). Returns false if the body
// is too short.
bool DecodeTimeF1(const uint8_t* body, std::size_t len, TimeF1& out);

// ---------------------------------------------------------------------------
class TimeCorrelator {
public:
    // Feed each Time F1 packet as it is encountered (header RTC + decoded body).
    void SetReference(uint64_t header_rtc, const TimeF1& t);

    bool HasReference() const { return has_ref_; }

    // Absolute time string for an RTC value, using the most recent time packet
    // as reference; falls back to raw seconds if no time packet was seen.
    std::string RtcToString(uint64_t rtc) const;

    // Signed nanoseconds between an RTC value and the reference time packet.
    int64_t RtcDeltaNs(uint64_t rtc) const;

    // "DDD:HH:MM:SS.mmm" or "HH:MM:SS.mmm" (reference day) -> RTC. False if
    // there is no reference or the string does not parse.
    bool StringToRtc(const std::string& text, uint64_t& rtc) const;

    const TimeF1& reference() const { return ref_time_; }
    uint64_t      ref_rtc()   const { return ref_rtc_; }

private:
    bool     has_ref_  = false;
    uint64_t ref_rtc_  = 0;
    TimeF1   ref_time_;
};

// Decode an 8-byte secondary-header / intra-packet time stamp when packet
// flags bit 6 selects secondary time (formats of 10.6.1.2.a). Returns a
// nanosecond count whose epoch depends on the format; `fmt` is
// PacketHeader::SecondaryTimeFormat().
uint64_t SecondaryTimeToNs(const uint8_t* ts8, uint8_t fmt);

} // namespace ch10
