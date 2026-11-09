// ch10/reader.h
// Sequential Chapter 10 file reader. Iterates packets, validates the header
// checksum, and resynchronizes on the 0xEB25 sync pattern after corruption.
//
// Uses only <fstream>/standard C++17 so it builds identically with MSVC on
// Windows and GCC/Clang on Linux. std::streamoff is 64-bit on both, so files
// larger than 4 GB work without platform-specific code.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ch10/packet.h"

namespace ch10 {

// One packet pulled from the file. `raw` holds the entire packet
// (header + optional secondary header + body + trailer).
struct Packet {
    PacketHeader          header;
    std::streamoff        file_offset = 0;
    std::vector<uint8_t>  raw;

    // Pointer to the packet body (channel-specific data word onward).
    const uint8_t* Body() const { return raw.data() + header.BodyOffset(); }
    // Valid body length per the header's data length field.
    std::size_t    BodyLen() const { return header.data_len; }
    // 8 bytes of secondary-header time, or nullptr if not present.
    const uint8_t* SecondaryTime() const {
        return header.HasSecondaryHeader() ? raw.data() + kHeaderSize : nullptr;
    }
};

class Reader {
public:
    // std::filesystem::path keeps non-ASCII Windows paths intact (the GUI
    // passes a wide path); the CLI's std::string converts implicitly.
    explicit Reader(const std::filesystem::path& path);

    bool IsOpen() const { return file_.is_open(); }
    const std::string& Error() const { return error_; }

    // Read the next packet. Returns false at end of file (or fatal error).
    // Corrupt regions are skipped by scanning for the next valid header;
    // `resync_count()` reports how often that happened.
    bool Next(Packet& out);

    // Restart from the beginning of the file.
    void Rewind();

    // Random access: read the packet whose header starts at `offset`
    // (an offset previously returned in Packet::file_offset).
    bool ReadAt(std::streamoff offset, Packet& out);

    uint64_t       resync_count() const { return resyncs_; }
    std::streamoff file_size()    const { return file_size_; }
    // File offsets where a corrupt/missing header forced a resync (first 1000).
    const std::vector<std::streamoff>& resync_offsets() const { return resync_offsets_; }

private:
    bool Resync();   // scan forward for a valid header; leaves file positioned on it

    std::ifstream  file_;
    std::string    error_;
    std::streamoff file_size_ = 0;
    uint64_t       resyncs_   = 0;
    std::vector<std::streamoff> resync_offsets_;
};

} // namespace ch10
