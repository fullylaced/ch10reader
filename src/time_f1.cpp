// src/time_f1.cpp
#include "ch10/time_f1.h"

#include <cstdio>

namespace ch10 {

const char* TimeF1::FmtName() const {
    switch (fmt) {
        case 0x0: return "IRIG-B";
        case 0x1: return "IRIG-A";
        case 0x2: return "IRIG-G";
        case 0x3: return "Real-Time Clock";
        case 0x4: return "UTC from GPS";
        case 0x5: return "Native GPS";
        case 0xF: return "None";
        default:  return "Reserved";
    }
}

const char* TimeF1::SrcName() const {
    switch (src) {
        case 0x0: return "Internal";
        case 0x1: return "External";
        case 0x2: return "Internal from RMM";
        case 0xF: return "None";
        default:  return "Reserved";
    }
}

bool DecodeTimeF1(const uint8_t* body, std::size_t len, TimeF1& out) {
    if (len < 4 + 6) return false;
    uint32_t csdw = le32(body);              // Figure 10-19
    out.src       = csdw & 0xF;
    out.fmt       = (csdw >> 4) & 0xF;
    out.leap_year = (csdw >> 8) & 1;
    out.dmy       = (csdw >> 9) & 1;

    const uint8_t* p = body + 4;
    uint16_t w1 = le16(p + 0);
    uint16_t w2 = le16(p + 2);
    uint16_t w3 = le16(p + 4);

    // Figure 10-20 / 10-21 BCD fields
    int Tmn = w1 & 0xF;              // tens of ms
    int Hmn = (w1 >> 4) & 0xF;       // hundreds of ms
    int Sn  = (w1 >> 8) & 0xF;       // units of seconds
    int TSn = (w1 >> 12) & 0x7;      // tens of seconds
    int Mn  = w2 & 0xF;              // units of minutes
    int TMn = (w2 >> 4) & 0x7;       // tens of minutes
    int Hn  = (w2 >> 8) & 0xF;       // units of hours
    int THn = (w2 >> 12) & 0x3;      // tens of hours

    out.millis  = (Hmn * 100 + Tmn * 10);
    out.seconds = TSn * 10 + Sn;
    out.minutes = TMn * 10 + Mn;
    out.hours   = THn * 10 + Hn;

    if (!out.dmy) {                  // Figure 10-20: day-of-year
        int Dn  = w3 & 0xF;
        int TDn = (w3 >> 4) & 0xF;
        int HDn = (w3 >> 8) & 0x3;
        out.day = HDn * 100 + TDn * 10 + Dn;
    } else {                         // Figure 10-21: day/month/year
        if (len < 4 + 8) return false;
        uint16_t w4 = le16(p + 6);
        int Dn  = w3 & 0xF;
        int TDn = (w3 >> 4) & 0xF;
        int On  = (w3 >> 8) & 0xF;
        int TOn = (w3 >> 12) & 0x1;
        int Yn  = w4 & 0xF;
        int TYn = (w4 >> 4) & 0xF;
        int HYn = (w4 >> 8) & 0xF;
        int OYn = (w4 >> 12) & 0x3;
        out.day   = TDn * 10 + Dn;
        out.month = TOn * 10 + On;
        out.year  = OYn * 1000 + HYn * 100 + TYn * 10 + Yn;
    }
    return true;
}

std::string TimeF1::ToString() const {
    char buf[48];
    if (dmy)
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                      year, month, day, hours, minutes, seconds, millis);
    else
        std::snprintf(buf, sizeof buf, "%03d:%02d:%02d:%02d.%03d",
                      day, hours, minutes, seconds, millis);
    return buf;
}

uint64_t TimeF1::TimeOfDayNs() const {
    uint64_t ns = static_cast<uint64_t>(hours) * 3600ull +
                  static_cast<uint64_t>(minutes) * 60ull + seconds;
    ns = ns * 1000ull + millis;
    return ns * 1000000ull;
}

int64_t TimeF1::EpochNs() const {
    int64_t days;
    if (!dmy) {
        days = day;                               // day-of-year, year unknown
    } else {
        // days from civil (Howard Hinnant's algorithm)
        int y = year, m = month, d = day;
        y -= m <= 2;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        days = era * 146097 + static_cast<int>(doe) - 719468;
    }
    return days * 86400ll * 1000000000ll + static_cast<int64_t>(TimeOfDayNs());
}

// ---------------------------------------------------------------------------
void TimeCorrelator::SetReference(uint64_t header_rtc, const TimeF1& t) {
    if (t.fmt == 0xF) return;        // "None": payload invalid
    ref_rtc_  = header_rtc;
    ref_time_ = t;
    has_ref_  = true;
}

int64_t TimeCorrelator::RtcDeltaNs(uint64_t rtc) const {
    // RTC ticks at 10 MHz -> 100 ns per count.
    return (static_cast<int64_t>(rtc) - static_cast<int64_t>(ref_rtc_)) * 100;
}

std::string TimeCorrelator::RtcToString(uint64_t rtc) const {
    if (!has_ref_) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "RTC %.6f s", rtc / 1e7);
        return buf;
    }
    int64_t ns  = static_cast<int64_t>(ref_time_.TimeOfDayNs()) + RtcDeltaNs(rtc);
    int64_t day_adj = 0;
    constexpr int64_t kDayNs = 86400ll * 1000000000ll;
    while (ns < 0)       { ns += kDayNs; --day_adj; }
    while (ns >= kDayNs) { ns -= kDayNs; ++day_adj; }

    int h  = static_cast<int>(ns / 3600000000000ll);
    int m  = static_cast<int>((ns / 60000000000ll) % 60);
    int s  = static_cast<int>((ns / 1000000000ll) % 60);
    int us = static_cast<int>((ns / 1000ll) % 1000000);

    char buf[64];
    if (ref_time_.dmy)   // keep the recorded date; note day rollover if any
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%06d",
                      ref_time_.year, ref_time_.month,
                      ref_time_.day + static_cast<int>(day_adj), h, m, s, us);
    else
        std::snprintf(buf, sizeof buf, "%03d:%02d:%02d:%02d.%06d",
                      ref_time_.day + static_cast<int>(day_adj), h, m, s, us);
    return buf;
}

bool TimeCorrelator::StringToRtc(const std::string& text, uint64_t& rtc) const {
    if (!has_ref_) return false;
    int a = 0, b = 0, c = 0, d = 0; double sec = 0;
    int day = ref_time_.day;
    if (std::sscanf(text.c_str(), "%d:%d:%d:%lf", &a, &b, &c, &sec) == 4) { day = a; }
    else if (std::sscanf(text.c_str(), "%d:%d:%lf", &b, &c, &sec) == 3) { }
    else return false;
    (void)d;
    int64_t tod_ns = (static_cast<int64_t>(b) * 3600 + c * 60) * 1000000000ll +
                     static_cast<int64_t>(sec * 1e9);
    int64_t target = (static_cast<int64_t>(day) - ref_time_.day) * 86400ll * 1000000000ll + tod_ns;
    int64_t delta_ns = target - static_cast<int64_t>(ref_time_.TimeOfDayNs());
    int64_t r = static_cast<int64_t>(ref_rtc_) + delta_ns / 100;
    if (r < 0) return false;
    rtc = static_cast<uint64_t>(r);
    return true;
}

// ---------------------------------------------------------------------------
uint64_t SecondaryTimeToNs(const uint8_t* ts, uint8_t fmt) {
    uint32_t lslw = le32(ts);
    uint32_t mslw = le32(ts + 4);
    switch (fmt) {
        case kSecTimeIeee1588:
            // LSLW = nanoseconds, MSLW = seconds (10.6.1.2.a, Fig. 10-13)
            return static_cast<uint64_t>(mslw) * 1000000000ull + lslw;
        case kSecTimeErtc:
            // 64-bit 1 ns counter (Fig. 10-14)
            return (static_cast<uint64_t>(mslw) << 32) | lslw;
        case kSecTimeCh4BinaryWeighted:
        default: {
            // Fig. 10-12: LSLW = [microseconds word | reserved],
            // MSLW = [high order | low order] = count of 10 ms units.
            uint32_t usec  = lslw >> 16;
            uint64_t tenms = mslw;
            return (tenms * 10000ull + usec) * 1000ull;
        }
    }
}

} // namespace ch10
