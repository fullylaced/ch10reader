// src/ms1553_f1.cpp
#include "ch10/ms1553_f1.h"

#include <cstdio>

namespace ch10 {
namespace ms1553 {

std::string BlockStatus::ErrorFlags() const {
    std::string s;
    auto add = [&s](const char* f) { if (!s.empty()) s += '|'; s += f; };
    if (MessageError()) add("ME");
    if (FormatError())  add("FE");
    if (RespTimeout())  add("TO");
    if (WordCountErr()) add("WC");
    if (SyncTypeErr())  add("SE");
    if (InvalidWord())  add("WE");
    return s;
}

std::string CommandWord::ToString() const {
    char buf[40];
    if (IsModeCode())
        std::snprintf(buf, sizeof buf, "RT%02u %c MC%02u%s",
                      Rt(), Transmit() ? 'T' : 'R', WcMc(),
                      Sa() == 31 ? " (SA31)" : "");
    else
        std::snprintf(buf, sizeof buf, "RT%02u %c SA%02u WC%02d",
                      Rt(), Transmit() ? 'T' : 'R', Sa(), WordCount());
    return buf;
}

std::string StatusWord::Flags() const {
    std::string s;
    auto add = [&s](const char* f) { if (!s.empty()) s += '|'; s += f; };
    if (MessageError())      add("ME");
    if (Instrumentation())   add("INSTR");
    if (ServiceRequest())    add("SRQ");
    if (BroadcastReceived()) add("BCR");
    if (Busy())              add("BUSY");
    if (SubsystemFlag())     add("SSF");
    if (DynamicBusAccept())  add("DBA");
    if (TerminalFlag())      add("TF");
    return s;
}

std::string Message::TypeName() const {
    if (bsw.RtToRt() || has_cmd2) return cmd1.Broadcast() ? "RT->RT(bcast)" : "RT->RT";
    if (cmd1.IsModeCode())        return cmd1.Broadcast() ? "Mode(bcast)"   : "Mode Code";
    if (cmd1.Broadcast())         return "BC->RT(bcast)";
    return cmd1.Transmit() ? "RT->BC" : "BC->RT";
}

// Classify the words that follow the command word(s) into status/data. Only
// attempted when the block status word reports no errors; otherwise the raw
// word list is left for the caller to display.
static void ClassifyWords(Message& m) {
    const std::size_t n = m.words.size();
    if (n == 0 || m.bsw.AnyError()) return;

    std::size_t i = m.has_cmd2 ? 2 : 1;   // skip command word(s)

    if (m.has_cmd2) {
        // RT-to-RT: [rx cmd][tx cmd][tx status][data...][rx status]
        if (i < n) { m.status1.raw = m.words[i++]; m.has_status1 = true; }
        while (i + 1 < n) m.data.push_back(m.words[i++]);
        if (i < n) { m.status2.raw = m.words[i++]; m.has_status2 = true; }
        return;
    }

    if (m.cmd1.IsModeCode()) {
        // Mode code: T -> [cmd][status][data?]; R -> [cmd][data?][status];
        // broadcast -> no status.
        bool has_data = m.cmd1.WcMc() >= 16;   // mode codes 16-31 carry a data word
        if (m.cmd1.Broadcast()) {
            while (i < n) m.data.push_back(m.words[i++]);
        } else if (m.cmd1.Transmit()) {
            if (i < n) { m.status1.raw = m.words[i++]; m.has_status1 = true; }
            while (i < n) m.data.push_back(m.words[i++]);
        } else {
            if (has_data && i < n) m.data.push_back(m.words[i++]);
            if (i < n) { m.status1.raw = m.words[i++]; m.has_status1 = true; }
        }
        return;
    }

    if (m.cmd1.Transmit()) {
        // RT -> BC: [cmd][status][data...]
        if (i < n) { m.status1.raw = m.words[i++]; m.has_status1 = true; }
        while (i < n) m.data.push_back(m.words[i++]);
    } else if (m.cmd1.Broadcast()) {
        // Broadcast receive: [cmd][data...] — no status
        while (i < n) m.data.push_back(m.words[i++]);
    } else {
        // BC -> RT: [cmd][data...][status]
        while (i + 1 < n) m.data.push_back(m.words[i++]);
        if (i < n) { m.status1.raw = m.words[i++]; m.has_status1 = true; }
    }
}

bool DecodePacket(const uint8_t* body, std::size_t len, uint8_t flags,
                  PacketContent& out) {
    out.messages.clear();
    out.error.clear();
    if (len < 4) { out.error = "body shorter than CSDW"; return false; }

    uint32_t csdw = le32(body);              // Figure 10-22
    out.csdw.msg_count = csdw & 0x00FFFFFF;
    out.csdw.ttb       = (csdw >> 30) & 0x3;

    const bool secondary_ipts = (flags & 0x40) != 0;
    std::size_t pos = 4;

    for (uint32_t i = 0; i < out.csdw.msg_count; ++i) {
        if (pos + 8 + 6 > len) {
            out.error = "packet truncated inside message " + std::to_string(i + 1);
            return false;
        }
        Message m;

        // Intra-packet time stamp (8 bytes, 10.6.4.2.c(1)).
        for (int b = 0; b < 8; ++b) m.ipts_bytes[b] = body[pos + b];
        if (!secondary_ipts) {
            // 48-bit RTC in the two long words; upper 16 bits zero-filled.
            m.rtc = le64(m.ipts_bytes) & 0x0000FFFFFFFFFFFFull;
        }
        pos += 8;

        // Intra-packet data header (6 bytes, Figure 10-23).
        m.bsw.raw = le16(body + pos);
        uint16_t gap = le16(body + pos + 2);
        m.gap1 = gap & 0xFF;                 // Figure 10-25
        m.gap2 = (gap >> 8) & 0xFF;
        m.length = le16(body + pos + 4);
        pos += 6;

        if (pos + m.length > len) {
            out.error = "message " + std::to_string(i + 1) +
                        " length runs past packet body";
            return false;
        }

        std::size_t nwords = m.length / 2;
        m.words.reserve(nwords);
        for (std::size_t w = 0; w < nwords; ++w)
            m.words.push_back(le16(body + pos + w * 2));
        pos += m.length;

        if (!m.words.empty()) {
            m.cmd1.raw = m.words[0];
            if (m.bsw.RtToRt() && m.words.size() >= 2) {
                m.cmd2.raw = m.words[1];
                m.has_cmd2 = true;
            }
        }
        ClassifyWords(m);
        out.messages.push_back(std::move(m));
    }
    return true;
}

} // namespace ms1553
} // namespace ch10
