// ch10/userdata.h
// Let a user add their own readable data to a recording from a plain text
// file: notes become annotation packets, text lines become Message Data
// Format 0 packets (the spec's serial-message container), and 1553 records
// become real MIL-STD-1553 F1 messages whose data words carry hex values or
// ASCII text. New channels are described in the setup record (TMATS) so any
// tool sees what they are.
//
// File format (one record per line, comma separated, text in double quotes):
//
//   # time is seconds from the start of the recording, or absolute
//   # DDD:HH:MM:SS.mmm (day-of-year) / HH:MM:SS.mmm on the recording's day.
//   12.500,       note, warning, "Camera 2 lost lock"
//   12.500-13.200, note, anomaly, "Retry burst on bus A"
//   12.600,       text, 200, "ALT=412 VEL=88 STATE=COAST"
//   12.700,       1553, 16, A, 5, R, 1, 0x0041 0x0042 0x0043
//   12.720,       1553, 16, A, 5, R, 1, "HELLO WORLD"
//
// See UserData::Template() for the annotated template.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ch10/annotation.h"
#include "ch10/ops.h"
#include "ch10/session.h"

namespace ch10 {

struct UserRecord {
    enum Kind { Note, Text, Ms1553 } kind = Text;
    int         line = 0;
    uint64_t    rtc = 0, rtc_end = 0;
    int         channel = -1;
    Severity    severity = Severity::Note;
    std::string text;                // note / text / ascii words
    bool        bus_b = false;
    int         rt = 0, sa = 0;
    bool        transmit = false;
    std::vector<uint16_t> words;     // 1553 data words
};

struct UserDataParse {
    std::vector<UserRecord> records;
    std::vector<std::string> errors; // "line N: ..." (parsing continues)
    int notes = 0, texts = 0, messages1553 = 0;
};

class UserData {
public:
    // Parse the text; times are resolved against `session` (start time and
    // time reference).
    static UserDataParse Parse(const std::string& text, const Session& session);
    static UserDataParse ParseFile(const std::string& path, const Session& session);

    // Turn records into packets (and channel descriptions for TMATS).
    static Injection ToInjection(const std::vector<UserRecord>& records, const Session& session);

    static std::string Template();
};

} // namespace ch10
