// ch10/writer.h
// Writes Chapter 10 packets to a new file. Handles the parts a writer must
// get right per 10.6.1: header checksum, 32-bit alignment filler, the data
// checksum selected by the packet flags, and per-channel sequence numbers.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include "ch10/packet.h"
#include "ch10/reader.h"

namespace ch10 {

class Writer {
public:
    explicit Writer(const std::filesystem::path& path);
    bool IsOpen() const { return out_.is_open(); }
    bool Close();                     // flush; false on I/O error

    // Copy a packet byte-for-byte, optionally changing channel and/or RTC in
    // the header (header checksum is recomputed). Sequence numbers are
    // always renumbered per output channel so every file we write is valid.
    bool WriteCopy(const Packet& p, int channel_override = -1,
                   uint64_t rtc_override = UINT64_MAX, bool has_rtc_override = false);

    // Assemble a packet from parts. `secondary12` may be null when flags bit
    // 7 is clear. Filler and data checksum are generated from `flags`.
    bool Write(uint16_t channel, uint8_t data_type, uint8_t version, uint8_t flags,
               uint64_t rtc, const uint8_t* secondary12,
               const uint8_t* body, std::size_t body_len);

    uint64_t packets() const { return packets_; }
    uint64_t bytes()   const { return bytes_; }

private:
    uint8_t NextSeq(uint16_t channel);
    bool Emit(const uint8_t* data, std::size_t n);

    std::ofstream                 out_;
    std::map<uint16_t, uint8_t>   seq_;
    uint64_t                      packets_ = 0;
    uint64_t                      bytes_   = 0;
    std::vector<uint8_t>          scratch_;
};

} // namespace ch10
