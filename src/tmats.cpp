// src/tmats.cpp
#include "ch10/tmats.h"

#include "ch10/packet.h"

#include <cctype>
#include <cstdlib>

namespace ch10 {

namespace {
std::string Trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Extract the numeric suffix after the last '-' in a TMATS code,
// e.g. "R-1\TK1-3" -> 3. Returns -1 if absent.
int CodeIndex(const std::string& code) {
    std::size_t dash = code.rfind('-');
    if (dash == std::string::npos || dash + 1 >= code.size()) return -1;
    for (std::size_t i = dash + 1; i < code.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(code[i]))) return -1;
    return std::atoi(code.c_str() + dash + 1);
}
} // namespace

void Tmats::AddPacket(const uint8_t* body, std::size_t len) {
    if (len < 4) return;
    if (!have_csdw_) {
        uint32_t csdw = le32(body);          // Figure 10-36
        ch10ver_ = static_cast<uint8_t>(csdw & 0xFF);
        srcc_    = (csdw >> 8) & 1;
        frmt_    = (csdw >> 9) & 1;
        have_csdw_ = true;
    }
    text_.append(reinterpret_cast<const char*>(body) + 4, len - 4);
}

void Tmats::Finalize() {
    // Strip trailing NUL padding some recorders append.
    while (!text_.empty() && (text_.back() == '\0')) text_.pop_back();
    if (!frmt_) ParseAttributes();   // key/value parsing applies to ASCII Ch9
    BuildChannelMap();
}

const char* Tmats::Ch10VersionName() const {
    switch (ch10ver_) {
        case 0x07: return "RCC 106-07";
        case 0x08: return "RCC 106-09";
        case 0x09: return "RCC 106-11";
        case 0x0A: return "RCC 106-13";
        case 0x0B: return "RCC 106-15";
        case 0x0C: return "RCC 106-17";
        case 0x0D: return "RCC 106-19";
        case 0x0E: return "RCC 106-22";
        default:   return "unknown";
    }
}

void Tmats::ParseAttributes() {
    // Chapter 9 ASCII: attributes are "CODE:VALUE;" — the code runs to the
    // first ':' and the value to the terminating ';'. CR/LF is cosmetic.
    std::size_t pos = 0;
    while (pos < text_.size()) {
        std::size_t semi = text_.find(';', pos);
        if (semi == std::string::npos) break;
        std::string entry = text_.substr(pos, semi - pos);
        pos = semi + 1;

        std::size_t colon = entry.find(':');
        if (colon == std::string::npos) continue;
        std::string code = Trim(entry.substr(0, colon));
        std::string val  = Trim(entry.substr(colon + 1));
        if (code.empty()) continue;

        attributes_.emplace_back(code, val);
        lookup_[code] = val;
    }
}

std::string Tmats::Get(const std::string& code) const {
    auto it = lookup_.find(code);
    return it == lookup_.end() ? std::string() : it->second;
}

void Tmats::BuildChannelMap() {
    // Group R-x\TK1-n (channel ID), R-x\DSI-n (source), R-x\CDT-n (type)
    // by their -n index within each recorder section R-x.
    std::map<std::string, TmatsChannelInfo> staging; // key "x:n"
    for (const auto& kv : attributes_) {
        const std::string& code = kv.first;
        std::size_t bs = code.find('\\');
        if (bs == std::string::npos || code.size() < 3) continue;
        if (code.compare(0, 2, "R-") != 0) continue;

        std::string section = code.substr(0, bs);   // "R-x"
        std::string attr    = code.substr(bs + 1);  // "TK1-n" etc.
        int idx = CodeIndex(attr);
        if (idx < 0) continue;
        std::string key = section + ":" + std::to_string(idx);

        if (attr.compare(0, 4, "TK1-") == 0)
            staging[key].channel_id = std::atoi(kv.second.c_str());
        else if (attr.compare(0, 4, "DSI-") == 0)
            staging[key].source = kv.second;
        else if (attr.compare(0, 4, "CDT-") == 0)
            staging[key].type = kv.second;
    }
    for (auto& kv : staging)
        if (kv.second.channel_id >= 0)
            channels_[kv.second.channel_id] = kv.second;
}

} // namespace ch10
