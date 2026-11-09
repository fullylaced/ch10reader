// ch10/tmats.h
// TMATS setup record — Computer-Generated Data Packet, Format 1 (10.6.7.2).
// The packet body is a 4-byte CSDW followed by the Chapter 9 TMATS text
// ("CODE:VALUE;" attributes). A single setup record may span several
// consecutive setup packets; feed each packet to AddPacket() in file order.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ch10 {

struct TmatsChannelInfo {
    int         channel_id = -1;   // R-x\TK1-n
    std::string source;            // R-x\DSI-n
    std::string type;              // R-x\CDT-n (e.g. 1553IN, TIMEIN, VIDIN)
};

class Tmats {
public:
    // Append the body of one setup-record packet (CSDW + text).
    void AddPacket(const uint8_t* body, std::size_t len);

    // Call once all setup packets have been fed in.
    void Finalize();

    bool empty() const { return attributes_.empty(); }

    // CSDW fields (from the first setup packet)
    bool    xml_format()   const { return frmt_; }     // 0=ASCII Ch9, 1=XML
    bool    config_changed() const { return srcc_; }
    uint8_t ch10_version() const { return ch10ver_; }
    const char* Ch10VersionName() const;

    // Raw TMATS text exactly as recorded.
    const std::string& text() const { return text_; }

    // Attributes in file order and by code lookup.
    const std::vector<std::pair<std::string, std::string>>& attributes() const {
        return attributes_;
    }
    // Returns empty string if the code is absent.
    std::string Get(const std::string& code) const;

    // Channel map assembled from R-x\TK1-n / DSI-n / CDT-n attributes,
    // keyed by Chapter 10 channel ID.
    const std::map<int, TmatsChannelInfo>& channels() const { return channels_; }

private:
    void ParseAttributes();
    void BuildChannelMap();

    bool        have_csdw_ = false;
    bool        frmt_      = false;
    bool        srcc_      = false;
    uint8_t     ch10ver_   = 0;
    std::string text_;

    std::vector<std::pair<std::string, std::string>> attributes_;
    std::map<std::string, std::string>               lookup_;
    std::map<int, TmatsChannelInfo>                  channels_;
};

} // namespace ch10
