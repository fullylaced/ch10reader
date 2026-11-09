// src/userdata.cpp
#include "ch10/userdata.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "ch10/message_f0.h"
#include "ch10/wordmap.h"

namespace ch10 {

namespace {

std::string Trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Split a line on commas, honouring double quotes. A field that was quoted
// is returned without the quotes and flagged in `quoted`.
void SplitFields(const std::string& line, std::vector<std::string>& fields, std::vector<bool>& quoted) {
    fields.clear(); quoted.clear();
    std::string cur; bool inq = false, was_q = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inq) {
            if (c == '"') { if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; } else inq = false; }
            else cur += c;
        } else if (c == '"') { inq = true; was_q = true; cur.clear(); }
        else if (c == ',') { fields.push_back(was_q ? cur : Trim(cur)); quoted.push_back(was_q); cur.clear(); was_q = false; }
        else cur += c;
    }
    fields.push_back(was_q ? cur : Trim(cur)); quoted.push_back(was_q);
}

// "12.5", "12.5-13.2", "270:14:30:05.250", "14:30:05.250-14:30:06"
bool ParseTime(const std::string& t, const Session& s, uint64_t& rtc, uint64_t& rtc_end, std::string& err) {
    std::string a = t, b;
    std::size_t dash = t.find('-', 1);
    if (dash != std::string::npos) { a = Trim(t.substr(0, dash)); b = Trim(t.substr(dash + 1)); }
    auto one = [&](const std::string& x, uint64_t& out) {
        if (x.find(':') != std::string::npos) {
            if (!s.time().StringToRtc(x, out)) { err = "cannot resolve absolute time '" + x + "' (recording has no time reference?)"; return false; }
            return true;
        }
        char* end = nullptr;
        double secs = std::strtod(x.c_str(), &end);
        if (end == x.c_str() || secs < 0) { err = "bad time '" + x + "'"; return false; }
        out = s.rtc_min() + static_cast<uint64_t>(secs * 1e7);
        return true;
    };
    if (!one(a, rtc)) return false;
    rtc_end = rtc;
    if (!b.empty() && !one(b, rtc_end)) return false;
    if (rtc_end < rtc) std::swap(rtc, rtc_end);
    return true;
}

Severity ParseSeverity(const std::string& s) {
    std::string l; for (char c : s) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "warning") return Severity::Warning;
    if (l == "anomaly") return Severity::Anomaly;
    if (l == "marker")  return Severity::Marker;
    return Severity::Note;
}

bool ParseWords(const std::string& field, bool quoted, std::vector<uint16_t>& words, std::string& err) {
    words.clear();
    if (quoted) { words = AsciiToWords(field); }
    else {
        std::stringstream ss(field);
        std::string tok;
        while (ss >> tok) {
            char* end = nullptr;
            long v = std::strtol(tok.c_str(), &end, 16);
            if (*end != '\0' || v < 0 || v > 0xFFFF) { err = "bad data word '" + tok + "' (hex expected)"; return false; }
            words.push_back(static_cast<uint16_t>(v));
        }
    }
    if (words.empty()) { err = "no data words"; return false; }
    if (words.size() > 32) { err = "more than 32 data words"; return false; }
    return true;
}

} // namespace

// ===========================================================================
UserDataParse UserData::ParseFile(const std::string& path, const Session& s) {
    std::ifstream f(path);
    UserDataParse r;
    if (!f) { r.errors.push_back("cannot open " + path); return r; }
    std::stringstream ss; ss << f.rdbuf();
    return Parse(ss.str(), s);
}

UserDataParse UserData::Parse(const std::string& text, const Session& s) {
    UserDataParse r;
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    std::vector<std::string> f; std::vector<bool> q;
    while (std::getline(in, line)) {
        ++lineno;
        std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;
        SplitFields(t, f, q);
        auto fail = [&](const std::string& msg) { r.errors.push_back("line " + std::to_string(lineno) + ": " + msg); };
        if (f.size() < 3) { fail("expected: time, kind, ..."); continue; }

        UserRecord rec; rec.line = lineno;
        std::string err;
        if (!ParseTime(f[0], s, rec.rtc, rec.rtc_end, err)) { fail(err); continue; }
        std::string kind; for (char c : f[1]) kind += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (kind == "note") {
            rec.kind = UserRecord::Note;
            if (f.size() >= 4) { rec.severity = ParseSeverity(f[2]); rec.text = f[3]; }
            else rec.text = f[2];
            if (rec.text.empty()) { fail("note has no text"); continue; }
            ++r.notes;
        } else if (kind == "text") {
            if (f.size() < 4) { fail("text needs: time, text, channel, \"message\""); continue; }
            rec.kind = UserRecord::Text;
            rec.channel = std::atoi(f[2].c_str());
            if (rec.channel <= 0 || rec.channel > 0xFFFF) { fail("bad channel '" + f[2] + "'"); continue; }
            rec.text = f[3];
            ++r.texts;
        } else if (kind == "1553") {
            if (f.size() < 8) { fail("1553 needs: time, 1553, channel, bus, rt, R|T, sa, words"); continue; }
            rec.kind = UserRecord::Ms1553;
            rec.channel = std::atoi(f[2].c_str());
            rec.bus_b = !f[3].empty() && (f[3][0] == 'B' || f[3][0] == 'b');
            rec.rt = std::atoi(f[4].c_str());
            rec.transmit = !f[5].empty() && (f[5][0] == 'T' || f[5][0] == 't');
            rec.sa = std::atoi(f[6].c_str());
            if (rec.channel <= 0 || rec.rt < 0 || rec.rt > 31 || rec.sa < 0 || rec.sa > 31) { fail("bad channel/RT/SA"); continue; }
            // words: everything from field 7 on (unquoted hex may have been split on commas by mistake; rejoin)
            std::string wf = f[7]; bool wq = q[7];
            for (std::size_t i = 8; i < f.size(); ++i) wf += " " + f[i];
            if (!ParseWords(wf, wq, rec.words, err)) { fail(err); continue; }
            rec.text = wq ? f[7] : "";
            ++r.messages1553;
        } else {
            fail("unknown kind '" + f[1] + "' (note, text, 1553)");
            continue;
        }
        r.records.push_back(std::move(rec));
    }
    std::stable_sort(r.records.begin(), r.records.end(),
                     [](const UserRecord& a, const UserRecord& b) { return a.rtc < b.rtc; });
    return r;
}

Injection UserData::ToInjection(const std::vector<UserRecord>& records, const Session& s) {
    Injection inj;
    std::vector<uint16_t> seen;
    auto note_channel = [&](int ch, uint8_t type, const std::string& name) {
        if (std::find(seen.begin(), seen.end(), static_cast<uint16_t>(ch)) != seen.end()) return;
        seen.push_back(static_cast<uint16_t>(ch));
        if (!s.Channel(ch)) inj.channels.push_back({static_cast<uint16_t>(ch), type, name});
    };

    // Group consecutive 1553 records on the same channel within 100 ms.
    std::vector<std::vector<uint8_t>> group; int group_ch = -1; uint64_t group_rtc = 0;
    auto flush_group = [&]() {
        if (group.empty()) return;
        InjectPacket ip; ip.rtc = group_rtc; ip.channel = static_cast<uint16_t>(group_ch);
        ip.data_type = kMilStd1553F1; ip.body = Build1553PacketBody(group);
        inj.packets.push_back(std::move(ip));
        group.clear(); group_ch = -1;
    };

    for (const UserRecord& r : records) {
        switch (r.kind) {
            case UserRecord::Note: {
                Annotation a; a.rtc_start = r.rtc; a.rtc_end = r.rtc_end; a.severity = r.severity; a.text = r.text;
                InjectPacket ip; ip.rtc = r.rtc; ip.channel = kAnnotationChannel;
                ip.data_type = kCompGenF0_UserDefined; ip.body = EncodeAnnotationBody(a);
                inj.packets.push_back(std::move(ip));
                break;
            }
            case UserRecord::Text: {
                Message m; m.rtc = r.rtc; m.data.assign(r.text.begin(), r.text.end());
                InjectPacket ip; ip.rtc = r.rtc; ip.channel = static_cast<uint16_t>(r.channel);
                ip.data_type = kMessageF0; ip.body = EncodeMessageF0({m});
                inj.packets.push_back(std::move(ip));
                note_channel(r.channel, kMessageF0, "User text");
                break;
            }
            case UserRecord::Ms1553: {
                if (group_ch != r.channel || r.rtc - group_rtc > 1000000 || group.size() >= 64) {
                    flush_group(); group_ch = r.channel; group_rtc = r.rtc;
                }
                group.push_back(Build1553Message(r.rtc, r.bus_b, r.rt, r.transmit, r.sa, r.words));
                note_channel(r.channel, kMilStd1553F1, "User 1553");
                break;
            }
        }
    }
    flush_group();
    std::stable_sort(inj.packets.begin(), inj.packets.end(),
                     [](const InjectPacket& a, const InjectPacket& b) { return a.rtc < b.rtc; });
    return inj;
}

std::string UserData::Template() {
    return
        "# Chapter 10 user data - one record per line, fields separated by commas,\n"
        "# text in double quotes. Lines starting with # are ignored.\n"
        "#\n"
        "# time  = seconds from the start of the recording (12.5), a range (12.5-13.2),\n"
        "#         or absolute DDD:HH:MM:SS.mmm / HH:MM:SS.mmm on the recording's day\n"
        "#\n"
        "# time, note, severity, \"text\"                 severity: note | warning | anomaly | marker\n"
        "#   -> annotation packet, shown on the timeline and in the Annotations tab\n"
        "# time, text, channel, \"text\"\n"
        "#   -> Message Data (Format 0) packet: readable text any Chapter 10 tool can list\n"
        "# time, 1553, channel, bus, rt, R|T, sa, words\n"
        "#   -> a MIL-STD-1553 message; words are hex (0041 0042) or \"ASCII text\"\n"
        "#      (two characters per data word, up to 32 words)\n"
        "\n"
        "1.500,        note, warning, \"Check RT5 wraparound here\"\n"
        "2.000-2.400,  note, anomaly, \"Burst of retries on bus A\"\n"
        "0.500,        text, 200, \"FLIGHT LOG: pad idle, GPS 3D fix\"\n"
        "1.000,        text, 200, \"ALT=0 VEL=0 STATE=ARMED\"\n"
        "2.000,        text, 200, \"ALT=412 VEL=88 STATE=COAST\"\n"
        "1.250,        1553, 16, A, 7, R, 3, 0041 0042 0043 0044\n"
        "1.300,        1553, 16, A, 7, R, 4, \"HELLO FROM THE PAD\"\n"
        "1.350,        1553, 16, B, 7, T, 4, \"RT7 STATUS OK\"\n";
}

} // namespace ch10
