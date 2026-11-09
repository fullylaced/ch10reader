// src/wordmap.cpp
#include "ch10/wordmap.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ch10 {

namespace {
std::string Trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
int ParseWild(const std::string& s) { std::string t = Trim(s); return (t.empty() || t == "*") ? -1 : std::atoi(t.c_str()); }
}

std::string WordsToAscii(const std::vector<uint16_t>& w, std::size_t first, std::size_t count) {
    std::string s;
    for (std::size_t i = first; i < w.size() && i < first + count; ++i) {
        char hi = static_cast<char>(w[i] >> 8), lo = static_cast<char>(w[i] & 0xFF);
        s += (hi >= 32 && hi < 127) ? hi : '.';
        s += (lo >= 32 && lo < 127) ? lo : '.';
    }
    return s;
}

std::vector<uint16_t> AsciiToWords(const std::string& text) {
    std::vector<uint16_t> w;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        uint8_t hi = static_cast<uint8_t>(text[i]);
        uint8_t lo = i + 1 < text.size() ? static_cast<uint8_t>(text[i + 1]) : ' ';
        w.push_back(static_cast<uint16_t>((hi << 8) | lo));
    }
    return w;
}

bool WordMap::LoadCsv(const std::string& path, std::string* error) {
    std::ifstream f(path);
    if (!f) { if (error) *error = "cannot open " + path; return false; }
    std::stringstream ss; ss << f.rdbuf();
    return ParseCsv(ss.str(), error);
}

bool WordMap::ParseCsv(const std::string& text, std::string* error) {
    entries_.clear();
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;
        std::vector<std::string> cols;
        std::stringstream ls(t);
        std::string c;
        while (std::getline(ls, c, ',')) cols.push_back(Trim(c));
        if (cols.size() < 7) {
            if (cols[0] == "channel") continue;             // header row
            if (error) *error = "line " + std::to_string(lineno) + ": expected channel,rt,sa,tr,word,name,type[,scale,offset,units]";
            return false;
        }
        WordMapEntry e;
        e.channel = ParseWild(cols[0]); e.rt = ParseWild(cols[1]); e.sa = ParseWild(cols[2]);
        std::string tr = cols[3];
        e.tr = (tr.empty() || tr == "*") ? -1 : (tr[0] == 'T' || tr[0] == 't') ? 1 : 0;
        e.word = std::atoi(cols[4].c_str());
        e.name = cols[5];
        e.type = cols[6];
        if (e.type.compare(0, 6, "ascii:") == 0) { e.ascii_words = std::max(1, std::atoi(e.type.c_str() + 6)); e.type = "ascii"; }
        if (cols.size() > 7 && !cols[7].empty()) e.scale = std::atof(cols[7].c_str());
        if (cols.size() > 8 && !cols[8].empty()) e.offset = std::atof(cols[8].c_str());
        if (cols.size() > 9) e.units = cols[9];
        entries_.push_back(e);
    }
    return true;
}

std::vector<DecodedWord> WordMap::Decode(int channel, int rt, int sa, bool transmit,
                                         const std::vector<uint16_t>& d) const {
    std::vector<DecodedWord> out;
    for (const auto& e : entries_) {
        if (e.channel >= 0 && e.channel != channel) continue;
        if (e.rt >= 0 && e.rt != rt) continue;
        if (e.sa >= 0 && e.sa != sa) continue;
        if (e.tr >= 0 && (e.tr == 1) != transmit) continue;
        if (e.word < 0 || static_cast<std::size_t>(e.word) >= d.size()) continue;
        DecodedWord w; w.word = e.word; w.name = e.name; w.units = e.units;
        uint16_t v = d[static_cast<std::size_t>(e.word)];
        bool two = static_cast<std::size_t>(e.word) + 1 < d.size();
        uint32_t v32 = two ? (static_cast<uint32_t>(v) << 16) | d[static_cast<std::size_t>(e.word) + 1] : v;
        char buf[64];
        auto num = [&](double x) {
            double y = x * e.scale + e.offset;
            if (std::fabs(y - std::round(y)) < 1e-9 && std::fabs(y) < 1e9) std::snprintf(buf, sizeof buf, "%.0f", y);
            else std::snprintf(buf, sizeof buf, "%.4g", y);
            w.value = buf;
        };
        if (e.type == "u16")      num(v);
        else if (e.type == "s16") num(static_cast<int16_t>(v));
        else if (e.type == "u32") num(v32);
        else if (e.type == "s32") num(static_cast<int32_t>(v32));
        else if (e.type == "f32") { float fl; std::memcpy(&fl, &v32, 4); num(fl); }
        else if (e.type == "hex") { std::snprintf(buf, sizeof buf, "0x%04X", v); w.value = buf; }
        else if (e.type == "bin") { std::string b; for (int i = 15; i >= 0; --i) { b += ((v >> i) & 1) ? '1' : '0'; if (i % 4 == 0 && i) b += ' '; } w.value = b; }
        else if (e.type == "ascii") w.value = "\"" + WordsToAscii(d, static_cast<std::size_t>(e.word), static_cast<std::size_t>(e.ascii_words)) + "\"";
        else { std::snprintf(buf, sizeof buf, "%u", v); w.value = buf; }
        out.push_back(w);
    }
    return out;
}

std::string WordMap::ExampleCsv() {
    return
        "# 1553 word map: names and scaling for data words\n"
        "# channel,rt,sa,tr,word,name,type,scale,offset,units\n"
        "#   '*' = any; tr = R (BC->RT) or T (RT->BC); word = zero-based data word index\n"
        "#   type: u16 s16 u32 s32 f32 hex bin ascii:N (N words = 2N characters)\n"
        "*,5,1,R,0,frame_counter,u16,1,0,\n"
        "*,5,1,R,1,altitude,s16,0.5,0,m\n"
        "*,5,1,R,2,status_bits,bin,,,\n"
        "*,5,2,T,0,marker,hex,,,\n"
        "*,5,2,T,1,vel_x,s16,0.01,0,m/s\n";
}

} // namespace ch10
