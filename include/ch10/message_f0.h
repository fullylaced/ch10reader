// ch10/message_f0.h
// Message Data Packets, Format 0 (10.6.9.1): the spec's container for
// serial / free-form messages. Each message carries an IPTS and an ID word
// (subchannel, length, error bits). This is where user-supplied readable
// text lands when injected into a recording, so any Chapter 10 tool that
// understands Message F0 can show it.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ch10 {

struct Message {
    uint64_t rtc = 0;
    uint16_t subchannel = 0;
    bool     data_error = false, format_error = false;
    std::vector<uint8_t> data;

    // Data as text with non-printables replaced by '.' (no size limit).
    std::string Text() const;
    bool LooksLikeText() const;
};

struct MessageCsdw { uint8_t type = 0; uint16_t counter = 0; };

bool DecodeMessageF0(const uint8_t* body, std::size_t len, uint8_t packet_flags,
                     uint64_t packet_rtc, MessageCsdw& csdw, std::vector<Message>& out);

// Body (CSDW + IPH + data + filler) for a "complete messages" packet whose
// IPTS are 48-bit RTC values.
std::vector<uint8_t> EncodeMessageF0(const std::vector<Message>& msgs);

} // namespace ch10
