// ch10/ms1553_f1.h
// MIL-STD-1553 Bus Data Packet, Format 1 (10.6.4.2).
//
// Packet body: CSDW (message count, time-tag bits) followed by N messages,
// each preceded by an 8-byte intra-packet time stamp and a 6-byte intra-packet
// data header (block status word, gap times word, length word). The message
// itself is the raw 1553 transaction: command word(s), status word(s), and
// data words, 16 bits each.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ch10/packet.h"

namespace ch10 {
namespace ms1553 {

// CSDW, Figure 10-22
struct Csdw {
    uint32_t msg_count = 0;   // bits 23-0
    uint8_t  ttb       = 0;   // bits 31-30: what the IPTS time-tags
};

// Block status word, Figure 10-24
struct BlockStatus {
    uint16_t raw = 0;
    bool BusB()          const { return (raw >> 13) & 1; }  // 0=A, 1=B
    bool MessageError()  const { return (raw >> 12) & 1; }
    bool RtToRt()        const { return (raw >> 11) & 1; }
    bool FormatError()   const { return (raw >> 10) & 1; }
    bool RespTimeout()   const { return (raw >> 9) & 1; }
    bool WordCountErr()  const { return (raw >> 5) & 1; }
    bool SyncTypeErr()   const { return (raw >> 4) & 1; }
    bool InvalidWord()   const { return (raw >> 3) & 1; }
    bool AnyError() const {
        return MessageError() || FormatError() || RespTimeout() ||
               WordCountErr() || SyncTypeErr() || InvalidWord();
    }
    std::string ErrorFlags() const;   // e.g. "ME,TO" or "" when clean
};

// MIL-STD-1553B command word decode
struct CommandWord {
    uint16_t raw = 0;
    uint8_t Rt()        const { return (raw >> 11) & 0x1F; }
    bool    Transmit()  const { return (raw >> 10) & 1; }
    uint8_t Sa()        const { return (raw >> 5) & 0x1F; }
    uint8_t WcMc()      const { return raw & 0x1F; }
    bool    IsModeCode()const { uint8_t sa = Sa(); return sa == 0 || sa == 31; }
    bool    Broadcast() const { return Rt() == 31; }
    // Data word count implied by the command (word count 0 means 32).
    int     WordCount() const { return WcMc() == 0 ? 32 : WcMc(); }
    std::string ToString() const;     // "RT05 T SA01 WC08" / "RT05 MC17"
};

// MIL-STD-1553B status word decode
struct StatusWord {
    uint16_t raw = 0;
    uint8_t Rt()               const { return (raw >> 11) & 0x1F; }
    bool MessageError()        const { return (raw >> 10) & 1; }
    bool Instrumentation()     const { return (raw >> 9) & 1; }
    bool ServiceRequest()      const { return (raw >> 8) & 1; }
    bool BroadcastReceived()   const { return (raw >> 4) & 1; }
    bool Busy()                const { return (raw >> 3) & 1; }
    bool SubsystemFlag()       const { return (raw >> 2) & 1; }
    bool DynamicBusAccept()    const { return (raw >> 1) & 1; }
    bool TerminalFlag()        const { return raw & 1; }
    std::string Flags() const;
};

// One decoded 1553 transaction
struct Message {
    uint8_t     ipts_bytes[8] = {0}; // raw intra-packet time stamp (see flags)
    uint64_t    rtc        = 0;    // 48-bit RTC when IPTS is RTC-based
    BlockStatus bsw;
    uint8_t     gap1       = 0;    // tenths of microseconds
    uint8_t     gap2       = 0;
    uint16_t    length     = 0;    // message bytes (cmd + status + data)

    CommandWord cmd1;              // always present (unless errored)
    CommandWord cmd2;              // second command word for RT-to-RT
    bool        has_cmd2   = false;

    std::vector<uint16_t> words;   // every 16-bit word of the message, in order

    // Words after the command word(s), with status words separated out where
    // the layout is unambiguous (no error flags set).
    std::vector<uint16_t> data;
    StatusWord  status1, status2;
    bool        has_status1 = false, has_status2 = false;

    std::string TypeName() const;  // "BC->RT", "RT->BC", "RT->RT", "Mode Code", ...
};

struct PacketContent {
    Csdw                 csdw;
    std::vector<Message> messages;
    std::string          error;    // non-empty if the body was malformed
};

// Decode a full 1553 F1 packet body. `flags` is PacketHeader::flags —
// bit 6 decides whether the IPTS is 48-bit RTC or secondary-header time.
bool DecodePacket(const uint8_t* body, std::size_t len, uint8_t flags,
                  PacketContent& out);

} // namespace ms1553
} // namespace ch10
